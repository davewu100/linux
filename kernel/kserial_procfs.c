// SPDX-License-Identifier: GPL-2.0
/*
 * k-serial procfs interface
 *
 * Provides /proc/kserial for querying current task's cgroup fields
 *
 * Usage:
 *   1. Write schema to the file (struct ks_schema)
 *   2. Read result from the file (struct ks_result)
 */

#include <linux/kserial.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/cgroup.h>
#include <linux/memcontrol.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/cred.h>
#include <linux/capability.h>
#include <linux/cpumask.h>
#include <linux/bpf.h>
#include <linux/btf.h>
#include <linux/mm.h>
#include <asm/io.h>

/* Field metadata cache */
struct ks_field_info {
	u32 offset;        /* Bit offset from BTF */
	u32 size;          /* Field size in bytes */
	u32 type_id;       /* BTF type ID */
	bool is_valid;     /* Cache entry valid */
};

/* Per-file subscription context (stateful) */
struct ks_context {
	/* Subscription info */
	char struct_name[64];
	char fields[32][128];
	u32 nr_fields;
	u32 pid;
	u32 flags;
	bool include_descriptor;

	/* Cached BTF lookup results */
	struct ks_field_info field_cache[32];

	/* Cached target addresses */
	void *struct_addr;
	struct task_struct *target_task;
	bool need_put_task;

	/* State */
	bool subscribed;
	u64 read_count;
	u64 last_read_ns;

	/* Shared buffer for multiple transport methods */
	void *shared_buffer;      /* Kernel buffer (can be mmap-ed) */
	size_t buffer_size;       /* Buffer size */
	size_t data_len;          /* Valid data length in buffer */
	spinlock_t buffer_lock;   /* Protect concurrent access */
};

/* Per-file private data */
struct ks_proc_data {
	struct ks_schema *schema;      /* Legacy mode */
	struct ks_result *result;       /* Legacy mode */
	bool result_ready;              /* Legacy mode */

	struct ks_context *ctx;         /* Subscribe-publish mode */
};

/**
 * ks_proc_open - Initialize per-file state
 */
static int ks_proc_open(struct inode *inode, struct file *file)
{
	struct ks_proc_data *data;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	/* Allocate legacy mode buffers */
	data->schema = kzalloc(sizeof(*data->schema), GFP_KERNEL);
	data->result = kzalloc(sizeof(*data->result), GFP_KERNEL);

	/* Allocate subscribe context */
	data->ctx = kzalloc(sizeof(*data->ctx), GFP_KERNEL);

	if (!data->schema || !data->result || !data->ctx) {
		kfree(data->schema);
		kfree(data->result);
		kfree(data->ctx);
		kfree(data);
		return -ENOMEM;
	}

	data->result_ready = false;
	data->ctx->subscribed = false;
	file->private_data = data;

	return 0;
}

/**
 * ks_get_target_task - Get target task by PID with permission checking
 * @pid: Target PID (0 = current process)
 *
 * Returns: task_struct pointer on success, ERR_PTR on error
 * Note: Caller must call put_task_struct() if task != current
 */
static struct task_struct *ks_get_target_task(u32 pid)
{
	struct task_struct *task;

	/* Default: current process */
	if (pid == 0)
		return current;

	rcu_read_lock();

	/* Find task by PID */
	task = find_task_by_vpid(pid);
	if (!task) {
		rcu_read_unlock();
		pr_warn("k-serial: PID %u not found\n", pid);
		return ERR_PTR(-ESRCH);
	}

	/* Permission check: same UID or CAP_SYS_ADMIN */
	if (!uid_eq(current_uid(), task_uid(task)) &&
	    !capable(CAP_SYS_ADMIN)) {
		rcu_read_unlock();
		pr_warn("k-serial: permission denied for PID %u\n", pid);
		return ERR_PTR(-EPERM);
	}

	/* PID namespace check */
	if (task_active_pid_ns(current) != task_active_pid_ns(task)) {
		rcu_read_unlock();
		pr_warn("k-serial: PID namespace mismatch for PID %u\n", pid);
		return ERR_PTR(-EINVAL);
	}

	/* Increase reference count to prevent task from disappearing */
	get_task_struct(task);
	rcu_read_unlock();

	return task;
}

/**
 * ks_proc_write - Accept schema from userspace
 *
 * User writes struct ks_schema to specify which fields to query
 * The schema can specify different struct types via struct_name field
 * and optionally target a specific process via target_pid
 */
static ssize_t ks_proc_write(struct file *file, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	struct ks_proc_data *data = file->private_data;
	struct task_struct *target_task = NULL;
	void *target_struct = NULL;
	const char *struct_name;
	bool need_put_task = false;
	int ret;

	if (count != sizeof(struct ks_schema))
		return -EINVAL;

	/* Copy schema from userspace */
	if (copy_from_user(data->schema, buf, sizeof(*data->schema)))
		return -EFAULT;

	/* Determine target struct type (default to "cgroup" if not specified) */
	if (data->schema->struct_name[0] == '\0') {
		strncpy(data->schema->struct_name, "cgroup", KS_FIELD_NAME_LEN - 1);
	}
	struct_name = data->schema->struct_name;

	/* Get target task (may be current or specified PID) */
	target_task = ks_get_target_task(data->schema->target_pid);
	if (IS_ERR(target_task)) {
		return PTR_ERR(target_task);
	}

	/* Remember to release task reference if not current */
	if (target_task != current)
		need_put_task = true;

	/* Get target struct based on type */
	rcu_read_lock();

	if (!strcmp(struct_name, "cgroup")) {
		/* Query target task's cgroup */
		target_struct = task_dfl_cgroup(target_task);
		if (!target_struct) {
			rcu_read_unlock();
			if (need_put_task)
				put_task_struct(target_task);
			pr_warn("k-serial: failed to get cgroup for PID %u\n",
				data->schema->target_pid);
			return -ENOENT;
		}
	} else if (!strcmp(struct_name, "mem_cgroup")) {
		/* Query target task's mem_cgroup */
		target_struct = mem_cgroup_from_task(target_task);
		if (!target_struct) {
			rcu_read_unlock();
			if (need_put_task)
				put_task_struct(target_task);
			pr_warn("k-serial: failed to get mem_cgroup for PID %u\n",
				data->schema->target_pid);
			return -ENOENT;
		}
		/* Force flush stats before reading to ensure we get up-to-date values */
		/* First call mem_cgroup_flush_stats to trigger update tracking, then force flush */
		{
			struct mem_cgroup *memcg = (struct mem_cgroup *)target_struct;
			struct cgroup_subsys_state *css = &memcg->css;
			int cpu;

			/* Call mem_cgroup_flush_stats first to ensure stats are marked for update */
			mem_cgroup_flush_stats(memcg);

			/* Then directly flush all CPUs to ensure complete aggregation */
			/* This ensures we get all per-CPU stats, including those not yet marked */
			if (css->ss && css->ss->css_rstat_flush) {
				for_each_possible_cpu(cpu) {
					css->ss->css_rstat_flush(css, cpu);
				}
			}
		}
	} else if (!strcmp(struct_name, "task_struct")) {
		/* Query target task itself */
		target_struct = target_task;
	} else {
		rcu_read_unlock();
		if (need_put_task)
			put_task_struct(target_task);
		pr_warn("k-serial: unsupported struct type '%s'\n", struct_name);
		return -EINVAL;
	}

	/* Query the struct using k-serial */
	ret = ks_query_struct(target_struct, struct_name, data->schema, data->result);
	rcu_read_unlock();

	/* Release task reference if we acquired one */
	if (need_put_task)
		put_task_struct(target_task);

	if (ret) {
		pr_warn("k-serial: query of struct '%s' failed: %d\n", struct_name, ret);
		return ret;
	}

	data->result_ready = true;
	*ppos = 0; /* Reset read position */

	return count;
}

/**
 * ks_generate_data - Generate data into buffer (transport-agnostic)
 *
 * This is the core data generation function, independent of transport method.
 * Can be used by read(), mmap, io_uring, or any other transport.
 *
 * @ctx: Subscription context
 * @buf: Output buffer
 * @buf_size: Buffer size
 *
 * Returns: Number of bytes written, or negative error code
 */
static ssize_t ks_generate_data(struct ks_context *ctx, void *buf, size_t buf_size)
{
	size_t pos = 0;
	int i;
	unsigned long flags;

	if (!ctx->subscribed)
		return -EINVAL;

	spin_lock_irqsave(&ctx->buffer_lock, flags);

	/* Optional: Write descriptor header (first read only) */
	if (ctx->include_descriptor && ctx->read_count == 0) {
		if (pos + 8 + ctx->nr_fields * 12 > buf_size) {
			spin_unlock_irqrestore(&ctx->buffer_lock, flags);
			return -E2BIG;
		}

		/* Descriptor header */
		u32 *desc_hdr = (u32 *)(buf + pos);
		desc_hdr[0] = 0xDE5C0000;  /* Magic: DESC */
		desc_hdr[1] = ctx->nr_fields;
		pos += 8;

		/* Field descriptors */
		for (i = 0; i < ctx->nr_fields; i++) {
			struct ks_field_info *fi = &ctx->field_cache[i];
			u32 *field_desc = (u32 *)(buf + pos);
			field_desc[0] = fi->offset;
			field_desc[1] = fi->size;
			field_desc[2] = fi->type_id;
			pos += 12;
		}
	}

	/* Read field values using cached offsets (core data) */
	rcu_read_lock();
	for (i = 0; i < ctx->nr_fields; i++) {
		struct ks_field_info *fi = &ctx->field_cache[i];
		void *field_addr = ctx->struct_addr + (fi->offset / 8);

		if (pos + fi->size > buf_size) {
			rcu_read_unlock();
			spin_unlock_irqrestore(&ctx->buffer_lock, flags);
			return -E2BIG;
		}

		/* Direct memcpy - no BTF lookup! */
		memcpy(buf + pos, field_addr, fi->size);
		pos += fi->size;
	}
	rcu_read_unlock();

	ctx->read_count++;
	ctx->last_read_ns = ktime_get_ns();

	spin_unlock_irqrestore(&ctx->buffer_lock, flags);

	return pos;
}

/**
 * ks_resolve_field_offset - Resolve field offset using BTF and cache it
 */
static int ks_resolve_field_offset(const char *struct_name, const char *field_path,
				   struct ks_field_info *info)
{
	const struct btf *btf = bpf_get_btf_vmlinux();
	const struct btf_type *t;
	const struct btf_member *m;
	u32 type_id, i;

	if (!btf) {
		pr_warn("k-serial: btf_vmlinux not available\n");
		return -ENOENT;
	}

	/* Find struct type */
	type_id = btf_find_by_name_kind(btf, struct_name, BTF_KIND_STRUCT);
	if (type_id <= 0) {
		pr_warn("k-serial: struct '%s' not found in BTF\n", struct_name);
		return -ENOENT;
	}

	t = btf_type_by_id(btf, type_id);

	/* Simple field lookup (TODO: support nested paths) */
	for (i = 0, m = btf_type_member(t); i < btf_type_vlen(t); i++, m++) {
		const char *member_name = btf_name_by_offset(btf, m->name_off);

		if (!strcmp(member_name, field_path)) {
			info->offset = btf_member_bit_offset(t, i);
			info->type_id = m->type;

			/* Get field size */
			const struct btf_type *field_type = btf_type_by_id(btf, m->type);
			info->size = field_type->size;
			if (info->size == 0) {
				/* For pointers */
				info->size = sizeof(void *);
			}

			info->is_valid = true;
			return 0;
		}
	}

	pr_warn("k-serial: field '%s' not found in struct '%s'\n",
		field_path, struct_name);
	return -ENOENT;
}

/**
 * ks_proc_ioctl - Handle subscribe/unsubscribe operations
 */
static long ks_proc_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct ks_proc_data *data = file->private_data;
	struct ks_context *ctx = data->ctx;
	struct ks_subscribe *sub;
	struct task_struct *target_task;
	void *target_struct;
	int ret, i;

	switch (cmd) {
	case KS_IOCTL_SUBSCRIBE:
		/* Allocate subscription buffer */
		sub = kzalloc(sizeof(*sub), GFP_KERNEL);
		if (!sub)
			return -ENOMEM;

		/* Copy subscription from user */
		if (copy_from_user(sub, (void __user *)arg, sizeof(*sub))) {
			kfree(sub);
			return -EFAULT;
		}

		/* Validate */
		if (sub->nr_fields == 0 || sub->nr_fields > 32) {
			kfree(sub);
			return -EINVAL;
		}

		/* Save subscription info to context */
		strncpy(ctx->struct_name, sub->struct_name, sizeof(ctx->struct_name) - 1);
		ctx->nr_fields = sub->nr_fields;
		ctx->pid = sub->pid;
		ctx->flags = sub->flags;
		ctx->include_descriptor = sub->include_descriptor;

		for (i = 0; i < sub->nr_fields; i++) {
			strncpy(ctx->fields[i], sub->fields[i], sizeof(ctx->fields[i]) - 1);
		}

		/* Get target task */
		target_task = ks_get_target_task(ctx->pid);
		if (IS_ERR(target_task)) {
			kfree(sub);
			return PTR_ERR(target_task);
		}

		ctx->target_task = target_task;
		ctx->need_put_task = (target_task != current);

		/* Resolve struct address */
		rcu_read_lock();

		if (!strcmp(ctx->struct_name, "cgroup")) {
			target_struct = task_dfl_cgroup(target_task);
		} else if (!strcmp(ctx->struct_name, "mem_cgroup")) {
			target_struct = mem_cgroup_from_task(target_task);
			if (target_struct) {
				/* Flush stats */
				struct mem_cgroup *memcg = target_struct;
				mem_cgroup_flush_stats(memcg);
			}
		} else if (!strcmp(ctx->struct_name, "task_struct")) {
			target_struct = target_task;
		} else {
			rcu_read_unlock();
			if (ctx->need_put_task)
				put_task_struct(target_task);
			kfree(sub);
			pr_warn("k-serial: unsupported struct type '%s'\n", ctx->struct_name);
			return -EINVAL;
		}

		if (!target_struct) {
			rcu_read_unlock();
			if (ctx->need_put_task)
				put_task_struct(target_task);
			kfree(sub);
			return -ENOENT;
		}

		ctx->struct_addr = target_struct;
		rcu_read_unlock();

		/* BTF lookup for all fields (cache!) */
		for (i = 0; i < ctx->nr_fields; i++) {
			ret = ks_resolve_field_offset(ctx->struct_name, ctx->fields[i],
						      &ctx->field_cache[i]);
			if (ret) {
				pr_warn("k-serial: failed to resolve field '%s': %d\n",
					ctx->fields[i], ret);
				if (ctx->need_put_task)
					put_task_struct(ctx->target_task);
				kfree(sub);
				return ret;
			}
		}

		ctx->subscribed = true;
		ctx->read_count = 0;
		spin_lock_init(&ctx->buffer_lock);

		/* Allocate shared buffer for mmap support */
		ctx->buffer_size = PAGE_SIZE;
		ctx->shared_buffer = (void *)__get_free_pages(GFP_KERNEL, 0);
		if (!ctx->shared_buffer) {
			pr_warn("k-serial: failed to allocate shared buffer\n");
			if (ctx->need_put_task)
				put_task_struct(ctx->target_task);
			ctx->subscribed = false;
			kfree(sub);
			return -ENOMEM;
		}

		pr_info("k-serial: subscribed to %u fields from '%s'\n",
			ctx->nr_fields, ctx->struct_name);
		kfree(sub);
		return 0;

	case KS_IOCTL_REFRESH:
		/* Refresh shared buffer for mmap users */
		if (!ctx->subscribed)
			return -EINVAL;

		if (!ctx->shared_buffer)
			return -ENOMEM;

		/* Generate fresh data into shared buffer */
		ret = ks_generate_data(ctx, ctx->shared_buffer, ctx->buffer_size);
		if (ret < 0)
			return ret;

		ctx->data_len = ret;
		return 0;

	case KS_IOCTL_UNSUBSCRIBE:
		if (ctx->subscribed && ctx->need_put_task && ctx->target_task) {
			put_task_struct(ctx->target_task);
		}
		memset(ctx, 0, sizeof(*ctx));
		pr_info("k-serial: unsubscribed\n");
		return 0;

	default:
		return -EINVAL;
	}
}

/**
 * ks_proc_read - Return query result to userspace (read() transport)
 *
 * Two modes:
 * 1. Legacy mode: read() after write() returns TLV result
 * 2. Subscribe mode: use unified data generation
 */
static ssize_t ks_proc_read(struct file *file, char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct ks_proc_data *data = file->private_data;
	struct ks_context *ctx = data->ctx;
	char *kbuf;
	ssize_t ret;

	/* Subscribe-publish mode: use unified data generation */
	if (ctx->subscribed) {
		if (*ppos != 0)
			return 0; /* EOF, reset ppos for next read */

		kbuf = kzalloc(4096, GFP_KERNEL);
		if (!kbuf)
			return -ENOMEM;

		/* Generate data using transport-agnostic function */
		ret = ks_generate_data(ctx, kbuf, 4096);
		if (ret < 0) {
			kfree(kbuf);
			return ret;
		}

		/* Copy to user (read() specific) */
		if (copy_to_user(buf, kbuf, ret)) {
			kfree(kbuf);
			return -EFAULT;
		}

		kfree(kbuf);
		*ppos = ret; /* Mark as read for this iteration */
		return ret;
	}

	/* Legacy mode: return pre-computed result */
	if (!data->result_ready)
		return -EAGAIN;

	size_t result_size = sizeof(data->result->total_len) + data->result->total_len;

	if (*ppos >= result_size)
		return 0; /* EOF */

	size_t to_copy = min(count, result_size - (size_t)*ppos);

	if (copy_to_user(buf, (char *)data->result + *ppos, to_copy))
		return -EFAULT;

	*ppos += to_copy;
	return to_copy;
}

/**
 * ks_proc_mmap - Map shared buffer to user space (mmap transport)
 */
static int ks_proc_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct ks_proc_data *data = file->private_data;
	struct ks_context *ctx = data->ctx;
	unsigned long pfn;
	size_t size;
	ssize_t ret;

	if (!ctx->subscribed)
		return -EINVAL;

	if (!ctx->shared_buffer)
		return -ENOMEM;

	size = vma->vm_end - vma->vm_start;
	if (size > ctx->buffer_size)
		return -EINVAL;

	pfn = virt_to_phys(ctx->shared_buffer) >> PAGE_SHIFT;
	if (remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot))
		return -EAGAIN;

	/* Generate initial data */
	ret = ks_generate_data(ctx, ctx->shared_buffer, ctx->buffer_size);
	if (ret > 0)
		ctx->data_len = ret;

	return 0;
}

/**
 * ks_proc_release - Cleanup per-file state
 */
static int ks_proc_release(struct inode *inode, struct file *file)
{
	struct ks_proc_data *data = file->private_data;

	if (data) {
		/* Clean up subscribe context */
		if (data->ctx) {
			if (data->ctx->subscribed && data->ctx->need_put_task &&
			    data->ctx->target_task) {
				put_task_struct(data->ctx->target_task);
			}

			/* Free shared buffer */
			if (data->ctx->shared_buffer) {
				free_pages((unsigned long)data->ctx->shared_buffer, 0);
			}

			kfree(data->ctx);
		}

		kfree(data->schema);
		kfree(data->result);
		kfree(data);
	}

	return 0;
}

static const struct proc_ops ks_proc_ops = {
	.proc_open    = ks_proc_open,
	.proc_read    = ks_proc_read,
	.proc_write   = ks_proc_write,
	.proc_ioctl   = ks_proc_ioctl,
	.proc_mmap    = ks_proc_mmap,
	.proc_release = ks_proc_release,
};

/**
 * ks_procfs_init - Initialize procfs interface
 */
static int __init ks_procfs_init(void)
{
	struct proc_dir_entry *entry;

	entry = proc_create("kserial", 0600, NULL, &ks_proc_ops);
	if (!entry) {
		pr_err("k-serial: failed to create /proc/kserial\n");
		return -ENOMEM;
	}

	pr_info("k-serial: initialized /proc/kserial interface\n");
	return 0;
}

/**
 * ks_procfs_exit - Cleanup procfs interface
 */
static void __exit ks_procfs_exit(void)
{
	remove_proc_entry("kserial", NULL);
	pr_info("k-serial: removed /proc/kserial interface\n");
}

module_init(ks_procfs_init);
module_exit(ks_procfs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jianyue Wu <wujianyue000@gmail.com>");
MODULE_DESCRIPTION("k-serial procfs interface for cgroup field queries");

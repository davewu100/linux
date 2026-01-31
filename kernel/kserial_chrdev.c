// SPDX-License-Identifier: GPL-2.0
/*
 * k-serial character device interface with io_uring support
 *
 * Provides /dev/kserial for io_uring-based async queries
 * While /proc/kserial continues to work for sync queries
 */

#include <linux/kserial.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/cgroup.h>
#include <linux/memcontrol.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/cred.h>
#include <linux/capability.h>
#include <linux/bpf.h>
#include <linux/btf.h>
#include <linux/mm.h>
#include <linux/io_uring/cmd.h>
#include <asm/io.h>

/* Field metadata cache */
struct ks_field_info {
	u32 offset;      /* Bit offset in struct */
	u32 size;        /* Size in bytes */
	u32 type_id;     /* BTF type ID */
};

/* Per-file descriptor subscription context */
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

/* Per-file private data - supports both modes */
struct ks_chrdev_data {
	struct ks_schema *schema;       /* Legacy mode */
	struct ks_result *result;       /* Legacy mode */
	bool result_ready;              /* Legacy mode */

	struct ks_context *ctx;         /* Subscribe-publish mode */
};

extern int ks_query_struct(void *struct_addr, const char *struct_name,
			   const struct ks_schema *schema, struct ks_result *result);

/**
 * ks_get_target_task - Get target task by PID with permission checks
 * @pid: Target PID (0 = current task)
 *
 * Returns: Task pointer on success, ERR_PTR on error
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
 * ks_generate_data - Generate query data (transport-agnostic)
 *
 * This function is called by ALL transport methods:
 * - read():     copy data to user buffer
 * - mmap:       update shared buffer
 * - io_uring:   async copy to user buffer
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

	/* Read field values using cached offsets */
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
 * ks_chrdev_write - Accept schema from userspace (Legacy mode)
 *
 * User writes struct ks_schema to specify which fields to query
 */
static ssize_t ks_chrdev_write(struct file *file, const char __user *buf,
			       size_t count, loff_t *ppos)
{
	struct ks_chrdev_data *data = file->private_data;
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
		/* Force flush stats before reading */
		{
			struct mem_cgroup *memcg = (struct mem_cgroup *)target_struct;
			struct cgroup_subsys_state *css = &memcg->css;
			int cpu;

			mem_cgroup_flush_stats(memcg);
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
 * ks_chrdev_open - Allocate per-file context
 */
static int ks_chrdev_open(struct inode *inode, struct file *file)
{
	struct ks_chrdev_data *data;

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

	spin_lock_init(&data->ctx->buffer_lock);
	file->private_data = data;

	return 0;
}

/**
 * ks_chrdev_ioctl - Handle subscribe/unsubscribe/refresh
 */
static long ks_chrdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct ks_chrdev_data *data = file->private_data;
	struct ks_context *ctx = data->ctx;
	struct ks_subscribe *sub;
	ssize_t ret;

	switch (cmd) {
	case KS_IOCTL_SUBSCRIBE:
		/* Allocate temporary buffer */
		sub = kzalloc(sizeof(*sub), GFP_KERNEL);
		if (!sub)
			return -ENOMEM;

		if (copy_from_user(sub, (void __user *)arg, sizeof(*sub))) {
			kfree(sub);
			return -EFAULT;
		}

		/* Copy subscription parameters */
		memcpy(ctx->struct_name, sub->struct_name, sizeof(ctx->struct_name));
		ctx->nr_fields = sub->nr_fields;
		ctx->pid = sub->pid;
		ctx->flags = sub->flags;
		ctx->include_descriptor = sub->include_descriptor;

		for (int i = 0; i < sub->nr_fields && i < 32; i++)
			memcpy(ctx->fields[i], sub->fields[i], sizeof(ctx->fields[i]));

		/* TODO: Resolve BTF offsets and cache them */
		/* This would call ks_query_struct() once to populate field_cache[] */

		/* Allocate shared buffer */
		ctx->buffer_size = PAGE_SIZE;
		ctx->shared_buffer = (void *)__get_free_pages(GFP_KERNEL, 0);
		if (!ctx->shared_buffer) {
			kfree(sub);
			return -ENOMEM;
		}

		ctx->subscribed = true;
		kfree(sub);
		return 0;

	case KS_IOCTL_UNSUBSCRIBE:
		ctx->subscribed = false;
		return 0;

	case KS_IOCTL_REFRESH:
		if (!ctx->subscribed)
			return -EINVAL;

		ret = ks_generate_data(ctx, ctx->shared_buffer, ctx->buffer_size);
		if (ret > 0)
			ctx->data_len = ret;

		return ret;

	default:
		return -ENOTTY;
	}
}

/**
 * ks_chrdev_read - Standard read() transport
 */
static ssize_t ks_chrdev_read(struct file *file, char __user *buf,
			      size_t count, loff_t *ppos)
{
	struct ks_context *ctx = file->private_data;
	ssize_t ret;

	if (!ctx->subscribed)
		return -EINVAL;

	/* Generate data */
	ret = ks_generate_data(ctx, ctx->shared_buffer, ctx->buffer_size);
	if (ret < 0)
		return ret;

	if (count > ret)
		count = ret;

	/* Copy to userspace */
	if (copy_to_user(buf, ctx->shared_buffer, count))
		return -EFAULT;

	return count;
}

/**
 * ks_chrdev_mmap - Zero-copy mmap transport
 */
static int ks_chrdev_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct ks_context *ctx = file->private_data;
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
 * ks_chrdev_uring_cmd - io_uring async transport (ULTIMATE PERFORMANCE)
 */
static int ks_chrdev_uring_cmd(struct io_uring_cmd *ioucmd, unsigned int issue_flags)
{
	struct file *file = ioucmd->file;
	struct ks_context *ctx = file->private_data;
	const u64 *cmd_data;
	void __user *user_buf;
	u32 cmd_op;
	u32 buf_len;
	ssize_t ret;

	if (!ctx || !ctx->subscribed)
		return -EINVAL;

	/* Get command data from SQE */
	cmd_data = io_uring_sqe_cmd(ioucmd->sqe);
	cmd_op = ioucmd->cmd_op;  /* Use cmd_op instead of cmd_len */

	switch (cmd_op) {
	case KS_URING_CMD_READ:
		/* Async read: generate + copy */
		user_buf = u64_to_user_ptr(cmd_data[0]);
		buf_len = (u32)cmd_data[1];

		ret = ks_generate_data(ctx, ctx->shared_buffer, ctx->buffer_size);
		if (ret < 0)
			goto out;

		if (buf_len > ret)
			buf_len = ret;

		if (copy_to_user(user_buf, ctx->shared_buffer, buf_len)) {
			ret = -EFAULT;
			goto out;
		}

		ret = buf_len;
		break;

	case KS_URING_CMD_REFRESH:
		/* Async refresh (for mmap users) */
		ret = ks_generate_data(ctx, ctx->shared_buffer, ctx->buffer_size);
		if (ret < 0)
			goto out;

		ctx->data_len = ret;
		break;

	default:
		ret = -EINVAL;
		goto out;
	}

out:
	/* Complete the io_uring command */
	io_uring_cmd_done(ioucmd, ret, issue_flags);
	return -EIOCBQUEUED;
}

/**
 * ks_chrdev_release - Cleanup context
 */
static int ks_chrdev_release(struct inode *inode, struct file *file)
{
	struct ks_context *ctx = file->private_data;

	if (ctx) {
		if (ctx->subscribed && ctx->need_put_task && ctx->target_task)
			put_task_struct(ctx->target_task);

		if (ctx->shared_buffer)
			free_pages((unsigned long)ctx->shared_buffer, 0);

		kfree(ctx);
	}

	return 0;
}

static const struct file_operations ks_chrdev_fops = {
	.owner          = THIS_MODULE,
	.open           = ks_chrdev_open,
	.read           = ks_chrdev_read,
	.write          = ks_chrdev_write,  /* Legacy mode support */
	.unlocked_ioctl = ks_chrdev_ioctl,
	.mmap           = ks_chrdev_mmap,
	.uring_cmd      = ks_chrdev_uring_cmd,
	.release        = ks_chrdev_release,
};

static struct miscdevice ks_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "kserial",
	.fops  = &ks_chrdev_fops,
};

/* Forward declarations for debugfs */
extern int ks_debug_init(void);
extern void ks_debug_cleanup(void);

static int __init ks_chrdev_init(void)
{
	int ret;

	/* Initialize BTF query cache */
	ret = ks_cache_init();
	if (ret) {
		pr_err("kserial: failed to initialize cache: %d\n", ret);
		return ret;
	}

	ret = misc_register(&ks_miscdev);
	if (ret) {
		pr_err("kserial: failed to register /dev/kserial: %d\n", ret);
		ks_cache_cleanup();
		return ret;
	}

	/* Initialize debugfs interface */
	ret = ks_debug_init();
	if (ret) {
		pr_warn("kserial: debugfs init failed (non-fatal): %d\n", ret);
		/* Continue even if debugfs fails */
	}

	pr_info("kserial: /dev/kserial registered (io_uring + BTF cache)\n");
	return 0;
}

static void __exit ks_chrdev_exit(void)
{
	ks_debug_cleanup();
	misc_deregister(&ks_miscdev);
	ks_cache_cleanup();
	pr_info("kserial: /dev/kserial unregistered\n");
}

module_init(ks_chrdev_init);
module_exit(ks_chrdev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("k-serial developers");
MODULE_DESCRIPTION("k-serial character device with io_uring support");

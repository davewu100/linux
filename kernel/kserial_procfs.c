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

/* Per-file private data */
struct ks_proc_data {
	struct ks_schema *schema;
	struct ks_result *result;
	bool result_ready;
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

	data->schema = kzalloc(sizeof(*data->schema), GFP_KERNEL);
	data->result = kzalloc(sizeof(*data->result), GFP_KERNEL);
	
	if (!data->schema || !data->result) {
		kfree(data->schema);
		kfree(data->result);
		kfree(data);
		return -ENOMEM;
	}

	data->result_ready = false;
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
 * ks_proc_read - Return query result to userspace
 * 
 * User reads struct ks_result containing TLV-encoded field values
 */
static ssize_t ks_proc_read(struct file *file, char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct ks_proc_data *data = file->private_data;
	size_t result_size;
	size_t to_copy;

	if (!data->result_ready)
		return -EAGAIN;

	/* Calculate total result size */
	result_size = sizeof(data->result->total_len) + data->result->total_len;

	if (*ppos >= result_size)
		return 0; /* EOF */

	to_copy = min(count, result_size - (size_t)*ppos);

	if (copy_to_user(buf, (char *)data->result + *ppos, to_copy))
		return -EFAULT;

	*ppos += to_copy;
	return to_copy;
}

/**
 * ks_proc_release - Cleanup per-file state
 */
static int ks_proc_release(struct inode *inode, struct file *file)
{
	struct ks_proc_data *data = file->private_data;

	if (data) {
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
MODULE_AUTHOR("Linux Kernel Community");
MODULE_DESCRIPTION("k-serial procfs interface for cgroup field queries");

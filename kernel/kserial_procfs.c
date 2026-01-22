// SPDX-License-Identifier: GPL-2.0
/*
 * k-serial procfs interface
 * 
 * Provides /proc/self/cgroup_query for querying current task's cgroup fields
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
#include <linux/sched.h>
#include <linux/uaccess.h>

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
 * ks_proc_write - Accept schema from userspace
 * 
 * User writes struct ks_schema to specify which fields to query
 */
static ssize_t ks_proc_write(struct file *file, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	struct ks_proc_data *data = file->private_data;
	struct cgroup *cgrp;
	int ret;

	if (count != sizeof(struct ks_schema))
		return -EINVAL;

	/* Copy schema from userspace */
	if (copy_from_user(data->schema, buf, sizeof(*data->schema)))
		return -EFAULT;

	/* Get current task's cgroup */
	rcu_read_lock();
	cgrp = task_dfl_cgroup(current);
	if (!cgrp) {
		rcu_read_unlock();
		return -ENOENT;
	}

	/* Query the cgroup using k-serial */
	ret = ks_query_cgroup(cgrp, data->schema, data->result);
	rcu_read_unlock();

	if (ret) {
		pr_warn("k-serial: query failed: %d\n", ret);
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

	entry = proc_create("cgroup_query", 0600, NULL, &ks_proc_ops);
	if (!entry) {
		pr_err("k-serial: failed to create /proc/cgroup_query\n");
		return -ENOMEM;
	}

	pr_info("k-serial: initialized /proc/cgroup_query interface\n");
	return 0;
}

/**
 * ks_procfs_exit - Cleanup procfs interface
 */
static void __exit ks_procfs_exit(void)
{
	remove_proc_entry("cgroup_query", NULL);
	pr_info("k-serial: removed /proc/cgroup_query interface\n");
}

module_init(ks_procfs_init);
module_exit(ks_procfs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Linux Kernel Community");
MODULE_DESCRIPTION("k-serial procfs interface for cgroup field queries");

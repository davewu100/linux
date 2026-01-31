// SPDX-License-Identifier: GPL-2.0
/*
 * kserial debugfs interface for statistics and monitoring
 */

#include <linux/kserial.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/module.h>

static struct dentry *ks_debug_dir;
static struct dentry *ks_stats_file;
static struct dentry *ks_cache_clear_file;

/* Show cache statistics */
static int ks_stats_show(struct seq_file *m, void *v)
{
	seq_printf(m, "kserial BTF Query Cache Statistics\n");
	seq_printf(m, "==================================\n\n");
	
	ks_cache_print_stats(m);
	
	return 0;
}

static int ks_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, ks_stats_show, NULL);
}

static const struct file_operations ks_stats_fops = {
	.owner = THIS_MODULE,
	.open = ks_stats_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/* Clear cache */
static ssize_t ks_cache_clear_write(struct file *file, const char __user *ubuf,
				     size_t count, loff_t *ppos)
{
	char buf[2];
	
	if (count > sizeof(buf) - 1)
		return -EINVAL;
	
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	
	buf[count] = '\0';
	
	if (buf[0] == '1') {
		ks_cache_invalidate();
		pr_info("kserial: cache cleared\n");
	}
	
	return count;
}

static const struct file_operations ks_cache_clear_fops = {
	.owner = THIS_MODULE,
	.write = ks_cache_clear_write,
};

/**
 * ks_debug_init - Initialize debugfs interface
 */
int __init ks_debug_init(void)
{
	ks_debug_dir = debugfs_create_dir("kserial", NULL);
	if (IS_ERR(ks_debug_dir)) {
		pr_warn("kserial: failed to create debugfs directory\n");
		return PTR_ERR(ks_debug_dir);
	}
	
	ks_stats_file = debugfs_create_file("stats", 0444, ks_debug_dir,
					     NULL, &ks_stats_fops);
	if (IS_ERR(ks_stats_file)) {
		pr_warn("kserial: failed to create stats file\n");
		debugfs_remove_recursive(ks_debug_dir);
		return PTR_ERR(ks_stats_file);
	}
	
	ks_cache_clear_file = debugfs_create_file("cache_clear", 0200,
						   ks_debug_dir, NULL,
						   &ks_cache_clear_fops);
	if (IS_ERR(ks_cache_clear_file)) {
		pr_warn("kserial: failed to create cache_clear file\n");
		debugfs_remove_recursive(ks_debug_dir);
		return PTR_ERR(ks_cache_clear_file);
	}
	
	pr_info("kserial: debugfs interface created at /sys/kernel/debug/kserial/\n");
	return 0;
}
EXPORT_SYMBOL_GPL(ks_debug_init);

/**
 * ks_debug_cleanup - Clean up debugfs interface
 */
void ks_debug_cleanup(void)
{
	debugfs_remove_recursive(ks_debug_dir);
}
EXPORT_SYMBOL_GPL(ks_debug_cleanup);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jianyue Wu <wujianyue000@gmail.com>");
MODULE_DESCRIPTION("kserial debugfs interface");

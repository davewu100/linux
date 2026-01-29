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

extern int ks_query_struct(void *struct_addr, const char *struct_name,
			   const struct ks_schema *schema, struct ks_result *result);

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
 * ks_chrdev_open - Allocate per-file context
 */
static int ks_chrdev_open(struct inode *inode, struct file *file)
{
	struct ks_context *ctx;
	
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	
	spin_lock_init(&ctx->buffer_lock);
	file->private_data = ctx;
	
	return 0;
}

/**
 * ks_chrdev_ioctl - Handle subscribe/unsubscribe/refresh
 */
static long ks_chrdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct ks_context *ctx = file->private_data;
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

static int __init ks_chrdev_init(void)
{
	int ret;
	
	ret = misc_register(&ks_miscdev);
	if (ret) {
		pr_err("kserial: failed to register /dev/kserial: %d\n", ret);
		return ret;
	}
	
	pr_info("kserial: /dev/kserial registered (io_uring support)\n");
	return 0;
}

static void __exit ks_chrdev_exit(void)
{
	misc_deregister(&ks_miscdev);
	pr_info("kserial: /dev/kserial unregistered\n");
}

module_init(ks_chrdev_init);
module_exit(ks_chrdev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("k-serial developers");
MODULE_DESCRIPTION("k-serial character device with io_uring support");

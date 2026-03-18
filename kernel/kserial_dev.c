// SPDX-License-Identifier: GPL-2.0
/*
 * kserial_dev: generic char device for BTF-based kernel struct access.
 * Supports read/write, mmap, and io_uring. Base from kallsyms symbol or pid.
 */

#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/kserial-internal.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/pid.h>
#include <linux/security.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <uapi/linux/kserial.h>

#define KSERIAL_ECHO_BUF_MAX 2048

#if defined(CONFIG_IO_URING)
#include <linux/io_uring/cmd.h>
#endif

#define KSERIAL_DEV_NAME "kserial"

#define KSERIAL_DISPLAY_NAME_LEN 96

struct kserial_slot {
	void *base;
	u32   offset;
	u32   size;
	bool  is_task;   /* base is task_struct*, need put_task_struct on close */
	bool  configured;
	char  display_name[KSERIAL_DISPLAY_NAME_LEN]; /* for cat "name value" */
};

struct kserial_file {
	struct kserial_slot slots[KSERIAL_MAX_SLOTS];
	int nr_slots;       /* max(slot_id+1) over SET_TARGET calls */
	bool output_text;   /* after echo config: cat returns decimal lines */
	size_t cat_len;     /* total length of text output (for partial read) */
};

static int kserial_open(struct inode *inode, struct file *filp)
{
	struct kserial_file *kf;
	int err;

	/* Intentional: same privilege as /dev/mem; see Documentation/admin-guide/kserial.rst */
	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	err = security_locked_down(LOCKDOWN_DEV_MEM);
	if (err)
		return err;

	kf = kzalloc_obj(struct kserial_file, GFP_KERNEL);
	if (!kf)
		return -ENOMEM;
	filp->private_data = kf;
	return 0;
}

static void kserial_clear_slot(struct kserial_slot *slot)
{
	if (slot->configured && slot->is_task && slot->base)
		put_task_struct((struct task_struct *)slot->base);
	slot->base = NULL;
	slot->offset = 0;
	slot->size = 0;
	slot->is_task = false;
	slot->configured = false;
	slot->display_name[0] = '\0';
}

static int kserial_release(struct inode *inode, struct file *filp)
{
	struct kserial_file *kf = filp->private_data;
	int i;

	if (kf) {
		for (i = 0; i < KSERIAL_MAX_SLOTS; i++)
			kserial_clear_slot(&kf->slots[i]);
		kfree(kf);
	}
	return 0;
}

/* Internal: set one slot from kernel copy of kserial_target (no user copy). */
static int kserial_set_target_parsed(struct kserial_file *kf, const struct kserial_target *t)
{
	struct kserial_slot *slot;
	void *base = NULL;
	u32 offset, size;
	int ret;
	struct pid *pid;
	struct task_struct *task;

	if (t->slot_id >= KSERIAL_MAX_SLOTS)
		return -EINVAL;
	slot = &kf->slots[t->slot_id];

	kserial_clear_slot(slot);

	switch (t->type) {
	case KSERIAL_OBJ_SYMBOL:
		base = kserial_base_from_symbol(t->symbol_name);
		if (!base)
			return -ENOENT;
		break;
	case KSERIAL_OBJ_PID:
		if (t->pid == 0)
			return -EINVAL;
		pid = find_get_pid(t->pid);
		if (!pid)
			return -ESRCH;
		task = pid_task(pid, PIDTYPE_PID);
		put_pid(pid);
		if (!task)
			return -ESRCH;
		get_task_struct(task);
		base = task;
		slot->is_task = true;
		break;
	default:
		return -EINVAL;
	}

	ret = kserial_btf_resolve(t->struct_name, t->field_path, &offset, &size);
	if (ret) {
		if (slot->is_task && base)
			put_task_struct((struct task_struct *)base);
		return ret;
	}

	slot->base = base;
	slot->offset = offset;
	slot->size = size;
	slot->configured = true;
	if (t->type == KSERIAL_OBJ_SYMBOL)
		snprintf(slot->display_name, KSERIAL_DISPLAY_NAME_LEN, "%s.%s.%s",
			 t->symbol_name, t->struct_name, t->field_path);
	else
		snprintf(slot->display_name, KSERIAL_DISPLAY_NAME_LEN, "pid%u.%s.%s",
			 t->pid, t->struct_name, t->field_path);
	if (kf->nr_slots <= (int)t->slot_id)
		kf->nr_slots = t->slot_id + 1;
	return 0;
}

static int kserial_set_target(struct kserial_file *kf, struct kserial_target __user *u)
{
	struct kserial_target t;

	if (copy_from_user(&t, u, sizeof(t)))
		return -EFAULT;
	t.symbol_name[KSERIAL_SYMBOL_LEN - 1] = '\0';
	t.struct_name[KSERIAL_STRUCT_LEN - 1] = '\0';
	t.field_path[KSERIAL_PATH_LEN - 1] = '\0';
	return kserial_set_target_parsed(kf, &t);
}

/* Advance *pp past optional spaces; return pointer to first token, null-term it and set *pp past it. */
static char *kserial_next_token(char **pp)
{
	char *p = (char *)skip_spaces(*pp);

	if (!p || !*p)
		return NULL;
	*pp = p;
	while (**pp && !isspace((unsigned char)**pp))
		(*pp)++;
	if (**pp)
		*(*pp)++ = '\0';
	return p;
}

/*
 * Parse one echo line: "[slot_id] symbol <sym> <struct> <field_path>" or
 * "[slot_id] pid <pid> <struct> <field_path>". Field path is rest of line (may contain spaces).
 * Returns 0 on success, -EINVAL on parse error.
 */
static int kserial_parse_echo_line(char *line, struct kserial_target *t)
{
	char *p = line;
	char *tok;
	unsigned int slot_id = 0;
	int is_symbol = -1;

	p = (char *)skip_spaces(p);
	if (!p || !*p)
		return -EINVAL;

	tok = kserial_next_token(&p);
	if (!tok)
		return -EINVAL;

	if (isdigit((unsigned char)*tok)) {
		if (kstrtou32(tok, 0, &slot_id) != 0 || slot_id >= KSERIAL_MAX_SLOTS)
			return -EINVAL;
		tok = kserial_next_token(&p);
		if (!tok)
			return -EINVAL;
	}

	if (strcmp(tok, "symbol") == 0)
		is_symbol = 1;
	else if (strcmp(tok, "pid") == 0)
		is_symbol = 0;
	else
		return -EINVAL;

	memset(t, 0, sizeof(*t));
	t->slot_id = slot_id;

	if (is_symbol) {
		t->type = KSERIAL_OBJ_SYMBOL;
		tok = kserial_next_token(&p);
		if (!tok)
			return -EINVAL;
		strscpy(t->symbol_name, tok, KSERIAL_SYMBOL_LEN);
		tok = kserial_next_token(&p);
		if (!tok)
			return -EINVAL;
		strscpy(t->struct_name, tok, KSERIAL_STRUCT_LEN);
		p = (char *)skip_spaces(p);
		if (p && *p)
			strscpy(t->field_path, p, KSERIAL_PATH_LEN);
		else
			return -EINVAL;
	} else {
		t->type = KSERIAL_OBJ_PID;
		tok = kserial_next_token(&p);
		if (!tok || kstrtou32(tok, 0, &t->pid) != 0 || t->pid == 0)
			return -EINVAL;
		tok = kserial_next_token(&p);
		if (!tok)
			return -EINVAL;
		strscpy(t->struct_name, tok, KSERIAL_STRUCT_LEN);
		p = (char *)skip_spaces(p);
		if (p && *p)
			strscpy(t->field_path, p, KSERIAL_PATH_LEN);
		else
			return -EINVAL;
	}
	return 0;
}

/*
 * Heuristic: treat write as "echo config" (text lines) when buffer contains
 * "symbol" or "pid" and no embedded NUL. Otherwise write goes to current slot
 * as raw binary.
 */
static bool kserial_looks_like_echo(const char *buf, size_t count)
{
	size_t i;

	if (count < 8)
		return false;
	for (i = 0; i < count && buf[i] != '\0'; i++)
		;
	if (i < count)
		return false; /* has embedded null, likely binary */
	return strstr(buf, "symbol") || strstr(buf, "pid");
}

static ssize_t kserial_read(struct file *filp, char __user *buf, size_t count, loff_t *ppos)
{
	struct kserial_file *kf = filp->private_data;
	struct kserial_slot *slot;
	size_t n;
	loff_t slot_idx = *ppos;

	/* Text mode (after echo): cat returns one decimal line per slot */
	if (kf->output_text && kf->nr_slots > 0) {
		char *tmp;
		size_t len = 0;
		size_t off = (size_t)slot_idx; /* f_pos used as byte offset for partial read */
		int i;

		if (off > 0 && off >= kf->cat_len)
			return 0; /* EOF */
		tmp = kmalloc(KSERIAL_ECHO_BUF_MAX, GFP_KERNEL);
		if (!tmp)
			return -ENOMEM;
		for (i = 0; i < kf->nr_slots && len < KSERIAL_ECHO_BUF_MAX - 24; i++) {
			slot = &kf->slots[i];
			if (!slot->configured)
				continue;
			if (slot->size <= 8 && slot->size != 0) {
				u64 val = kserial_read_field(slot->base, slot->offset, slot->size);

				len += snprintf(tmp + len, KSERIAL_ECHO_BUF_MAX - len, "%s %llu\n",
						slot->display_name[0] ? slot->display_name : "?", val);
			}
		}
		if (len == 0) {
			kfree(tmp);
			return 0;
		}
		if (off == 0)
			kf->cat_len = len;
		n = min_t(size_t, count, len - off);
		if (copy_to_user(buf, tmp + off, n))
			n = -EFAULT;
		kfree(tmp);
		if (n < 0)
			return n;
		*ppos = off + n;
		return n;
	}

	if (kf->nr_slots == 0)
		return -ENOTTY;
	if (slot_idx < 0 || slot_idx >= kf->nr_slots)
		return -EINVAL;
	slot = &kf->slots[slot_idx];
	if (!slot->configured)
		return -ENOTTY;
	n = min_t(size_t, count, slot->size);
	if (n == 0)
		return 0;
	if (copy_to_user(buf, (char *)slot->base + slot->offset, n))
		return -EFAULT;
	return n;
}

static ssize_t kserial_write(struct file *filp, const char __user *buf, size_t count, loff_t *ppos)
{
	struct kserial_file *kf = filp->private_data;
	struct kserial_slot *slot;
	size_t n;
	loff_t slot_idx = *ppos;
	char *kbuf = NULL;
	char *line;
	struct kserial_target t;
	int nr_parsed = 0;

	/* Echo config path: only when size fits and we copy first (cannot dereference user buf). */
	if (count > 0 && count <= KSERIAL_ECHO_BUF_MAX) {
		kbuf = kmalloc(count + 1, GFP_KERNEL);
		if (!kbuf)
			return -ENOMEM;
		if (copy_from_user(kbuf, buf, count)) {
			kfree(kbuf);
			return -EFAULT;
		}
		kbuf[count] = '\0';

		if (kserial_looks_like_echo(kbuf, count)) {
			for (line = kbuf; line && *line; ) {
				char *eol = strchr(line, '\n');

				if (eol)
					*eol = '\0';
				line = skip_spaces(line);
				if (*line && kserial_parse_echo_line(line, &t) == 0) {
					if (kserial_set_target_parsed(kf, &t) == 0)
						nr_parsed++;
				}
				if (eol)
					line = eol + 1;
				else
					break;
			}
			kfree(kbuf);
			if (nr_parsed > 0) {
				kf->output_text = true;
				kf->cat_len = 0; /* next read builds fresh */
			}
			return count;
		}
	}

	/* Binary write to current slot (use kbuf if we already copied). */
	if (kf->nr_slots == 0) {
		kfree(kbuf);
		return -ENOTTY;
	}
	if (slot_idx < 0 || slot_idx >= kf->nr_slots) {
		kfree(kbuf);
		return -EINVAL;
	}
	slot = &kf->slots[slot_idx];
	if (!slot->configured) {
		kfree(kbuf);
		return -ENOTTY;
	}
	n = min_t(size_t, count, slot->size);
	if (n == 0) {
		kfree(kbuf);
		return 0;
	}
	if (kbuf) {
		memcpy((char *)slot->base + slot->offset, kbuf, n);
		kfree(kbuf);
	} else {
		if (copy_from_user((char *)slot->base + slot->offset, buf, n))
			return -EFAULT;
	}
	return n;
}

static loff_t kserial_llseek(struct file *filp, loff_t offset, int whence)
{
	struct kserial_file *kf = filp->private_data;
	loff_t npos;

	switch (whence) {
	case SEEK_SET:
		npos = offset;
		break;
	case SEEK_CUR:
		npos = filp->f_pos + offset;
		break;
	case SEEK_END:
		npos = (kf->nr_slots > 0 ? kf->nr_slots - 1 : 0) + offset;
		break;
	default:
		return -EINVAL;
	}
	if (npos < 0 || (kf->nr_slots > 0 && npos >= kf->nr_slots))
		return -EINVAL;
	filp->f_pos = npos;
	return npos;
}

static int kserial_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct kserial_file *kf = filp->private_data;
	struct kserial_slot *slot;
	unsigned long size;
	void *kva;
	loff_t slot_idx = filp->f_pos;

	if (kf->nr_slots == 0)
		return -ENOTTY;
	if (slot_idx < 0 || slot_idx >= kf->nr_slots)
		return -EINVAL;
	slot = &kf->slots[slot_idx];
	if (!slot->configured)
		return -ENOTTY;

	kva = (char *)slot->base + slot->offset;
	if (!virt_addr_valid(kva))
		return -EINVAL;

	size = min_t(unsigned long, vma->vm_end - vma->vm_start, (unsigned long)slot->size);
	if (size == 0)
		return -EINVAL;

	return remap_pfn_range(vma, vma->vm_start,
			       page_to_pfn(virt_to_page(kva)), size, vma->vm_page_prot);
}

static long kserial_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct kserial_file *kf = filp->private_data;
	__u32 __user *u = (__u32 __user *)arg;
	__u32 n;

	switch (cmd) {
	case KSERIAL_IOC_SET_TARGET:
		return kserial_set_target(kf, (struct kserial_target __user *)arg);
	case KSERIAL_IOC_GET_NSLOTS:
		n = (__u32)kf->nr_slots;
		return put_user(n, u) ? -EFAULT : 0;
	default:
		return -ENOTTY;
	}
}

#if defined(CONFIG_IO_URING)
static int kserial_uring_cmd(struct io_uring_cmd *ioucmd, unsigned int issue_flags)
{
	struct kserial_file *kf = ioucmd->file->private_data;
	struct kserial_slot *slot;
	const struct io_uring_sqe *sqe = ioucmd->sqe;
	u32 op = sqe->cmd_op;
	u32 slot_id = (u32)sqe->off;  /* slot index for this command */
	unsigned long ubuf = sqe->addr;
	u32 len = sqe->len;
	size_t n;
	int ret = 0;

	if (kf->nr_slots == 0)
		return -ENOTTY;
	if (slot_id >= (u32)kf->nr_slots)
		return -EINVAL;
	slot = &kf->slots[slot_id];
	if (!slot->configured)
		return -ENOTTY;
	n = min_t(size_t, len, slot->size);
	if (n == 0) {
		io_uring_cmd_done(ioucmd, 0, issue_flags);
		return 0;
	}
	switch (op) {
	case KSERIAL_URING_OP_READ:
		if (copy_to_user((void __user *)ubuf, (char *)slot->base + slot->offset, n))
			ret = -EFAULT;
		break;
	case KSERIAL_URING_OP_WRITE:
		if (copy_from_user((char *)slot->base + slot->offset, (void __user *)ubuf, n))
			ret = -EFAULT;
		break;
	default:
		ret = -EOPNOTSUPP;
	}
	io_uring_cmd_done(ioucmd, ret < 0 ? ret : (s32)n, issue_flags);
	return 0;
}
#endif

static const struct file_operations kserial_fops = {
	.owner          = THIS_MODULE,
	.open           = kserial_open,
	.release        = kserial_release,
	.read           = kserial_read,
	.write          = kserial_write,
	.llseek         = kserial_llseek,
	.mmap           = kserial_mmap,
	.unlocked_ioctl = kserial_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
#if defined(CONFIG_IO_URING)
	.uring_cmd      = kserial_uring_cmd,
#endif
	.fop_flags      = FOP_UNSIGNED_OFFSET,
};

static struct miscdevice kserial_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = KSERIAL_DEV_NAME,
	.fops  = &kserial_fops,
};

static int __init kserial_dev_init(void)
{
	return misc_register(&kserial_misc);
}
device_initcall(kserial_dev_init);

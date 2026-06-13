// SPDX-License-Identifier: GPL-2.0-only
/*
 * kserial - BTF-aware kernel serial protocol
 *
 * /dev/kserial lets userspace pass raw kernel struct bytes together with a
 * type name.  The kernel looks up the type in vmlinux BTF, walks the struct
 * members, and serialises each field into a protobuf-compatible binary
 * message that is placed in a per-fd ring buffer.  Userspace consumes the
 * encoded messages via read() or by mmap'ing the ring buffer directly.
 *
 * Wire format is documented in <uapi/linux/kserial.h>.
 *
 * Requires CONFIG_DEBUG_INFO_BTF=y.
 */

#include <linux/btf.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <uapi/linux/btf.h>
#include <uapi/linux/kserial.h>

/* Data ring buffer size: 64 pages = 256 KB */
#define KSERIAL_RB_DATA_ORDER	6
#define KSERIAL_RB_DATA_SIZE	(PAGE_SIZE << KSERIAL_RB_DATA_ORDER)
/*
 * Maximum write() request size.  Capped at the ring buffer data size so
 * that a single request can never be larger than what the buffer can hold.
 */
#define KSERIAL_MAX_REQ_SIZE	KSERIAL_RB_DATA_SIZE
#define KSERIAL_RB_TOTAL_SIZE	(PAGE_SIZE + KSERIAL_RB_DATA_SIZE)
/*
 * Maximum encoded message body size.  Sized so that the largest possible
 * on-wire message — kserial_msg_hdr (16 bytes) + type name (up to
 * KSERIAL_MAX_NAME bytes) + encoded body — always fits in a single empty
 * ring-buffer slot:
 *
 *   sizeof(kserial_msg_hdr) + KSERIAL_MAX_NAME + KSERIAL_MAX_ENC_SIZE
 *       <= KSERIAL_RB_DATA_SIZE
 */
#define KSERIAL_MAX_ENC_SIZE	\
	(KSERIAL_RB_DATA_SIZE - sizeof(struct kserial_msg_hdr) - KSERIAL_MAX_NAME)
/* Maximum struct nesting depth for recursive encoding */
#define KSERIAL_MAX_DEPTH	8

/* =========================================================================
 * LEB128 / varint helpers
 * =======================================================================
 */

/**
 * ks_varint_encode() - encode @value as an unsigned LEB128 varint
 * @buf:   output buffer
 * @size:  available bytes
 * @value: value to encode
 *
 * Returns the number of bytes written, or -ENOSPC if @buf is too small.
 */
static int ks_varint_encode(u8 *buf, size_t size, u64 value)
{
	int i = 0;

	do {
		if (i >= size)
			return -ENOSPC;
		buf[i] = value & 0x7f;
		value >>= 7;
		if (value)
			buf[i] |= 0x80;
		i++;
	} while (value);

	return i;
}

/* Zigzag-encode a signed integer so that small negatives remain small. */
static inline u64 ks_zigzag(s64 v)
{
	return ((u64)v << 1) ^ (u64)(v >> 63);
}

/* =========================================================================
 * Per-fd ring buffer
 *
 * Layout of the vmalloc'd region:
 *   [0, PAGE_SIZE)            struct kserial_rb_ctrl  (control page)
 *   [PAGE_SIZE, total_size)   circular data buffer
 *
 * The region is mapped read-write into userspace via mmap().
 * All kernel-side producer/consumer updates hold rb->lock.
 * =======================================================================
 */

struct ks_rb {
	spinlock_t		lock;
	void		       *base;		/* vmalloc_user base address     */
	struct kserial_rb_ctrl *ctrl;		/* points to base                */
	u8		       *data;		/* points to base + PAGE_SIZE    */
	u64			producer;	/* kernel-internal producer idx  */
	wait_queue_head_t	waitq;
};

static int ks_rb_alloc(struct ks_rb *rb)
{
	rb->base = vmalloc_user(KSERIAL_RB_TOTAL_SIZE);
	if (!rb->base)
		return -ENOMEM;

	rb->ctrl = rb->base;
	rb->data = (u8 *)rb->base + PAGE_SIZE;

	rb->ctrl->magic     = KSERIAL_MAGIC;
	rb->ctrl->version   = KSERIAL_VERSION;
	rb->ctrl->data_size = KSERIAL_RB_DATA_SIZE;
	rb->ctrl->producer  = 0;
	rb->ctrl->consumer  = 0;
	rb->producer        = 0;

	spin_lock_init(&rb->lock);
	init_waitqueue_head(&rb->waitq);
	return 0;
}

static void ks_rb_free(struct ks_rb *rb)
{
	vfree(rb->base);
	rb->base = NULL;
}

/**
 * ks_rb_write() - append @len bytes from @buf to the ring buffer
 *
 * Returns 0 on success, -ENOSPC if the buffer is full.
 */
static int ks_rb_write(struct ks_rb *rb, const u8 *buf, size_t len)
{
	u64 cons, avail;
	size_t pos, first;

	spin_lock(&rb->lock);

	/*
	 * Use the kernel-internal producer index for capacity calculations.
	 * ctrl->producer is a published copy visible to mmap readers; reading
	 * it for kernel-side logic would allow a mmap user to corrupt the
	 * available-space accounting by writing to ctrl->producer.
	 */
	cons  = READ_ONCE(rb->ctrl->consumer);
	/*
	 * Clamp consumer to the valid window [oldest_live, producer].
	 * A mmap user may set ctrl->consumer to any value; two cases need
	 * bounding to keep the avail calculation correct:
	 *   upper: cons > producer  ->  avail wraps to near UINT64_MAX (false
	 *          space), so clamp to producer (empty ring).
	 *   lower: producer - cons > KSERIAL_RB_DATA_SIZE  ->  avail wraps to
	 *          near UINT64_MAX (false space), bypassing the ENOSPC check;
	 *          clamp to the oldest byte still in the ring.
	 */
	if (cons > rb->producer)
		cons = rb->producer;
	else if (rb->producer - cons > KSERIAL_RB_DATA_SIZE)
		cons = rb->producer - KSERIAL_RB_DATA_SIZE;
	avail = KSERIAL_RB_DATA_SIZE - (rb->producer - cons);

	if (avail < len) {
		spin_unlock(&rb->lock);
		return -ENOSPC;
	}

	pos = rb->producer % KSERIAL_RB_DATA_SIZE;
	if (pos + len <= KSERIAL_RB_DATA_SIZE) {
		memcpy(rb->data + pos, buf, len);
	} else {
		first = KSERIAL_RB_DATA_SIZE - pos;
		memcpy(rb->data + pos, buf, first);
		memcpy(rb->data, buf + first, len - first);
	}

	rb->producer += len;
	/* Publish the new producer index to userspace via a store-release. */
	smp_store_release(&rb->ctrl->producer, rb->producer);
	spin_unlock(&rb->lock);

	wake_up_interruptible(&rb->waitq);
	return 0;
}

/**
 * ks_rb_read() - copy up to @count bytes from the ring buffer into @buf
 *
 * Returns the number of bytes copied (0 if empty).
 */
static ssize_t ks_rb_read(struct ks_rb *rb, u8 *buf, size_t count)
{
	u64 cons;
	size_t avail, to_read, pos, first;

	spin_lock(&rb->lock);

	cons  = READ_ONCE(rb->ctrl->consumer);
	if (cons > rb->producer)
		cons = rb->producer;
	avail = rb->producer - cons;

	if (!avail) {
		spin_unlock(&rb->lock);
		return 0;
	}

	to_read = min(count, avail);
	pos = cons % KSERIAL_RB_DATA_SIZE;

	if (pos + to_read <= KSERIAL_RB_DATA_SIZE) {
		memcpy(buf, rb->data + pos, to_read);
	} else {
		first = KSERIAL_RB_DATA_SIZE - pos;
		memcpy(buf, rb->data + pos, first);
		memcpy(buf + first, rb->data, to_read - first);
	}

	WRITE_ONCE(rb->ctrl->consumer, cons + to_read);
	spin_unlock(&rb->lock);

	return to_read;
}

/* =========================================================================
 * BTF-based struct encoder
 *
 * Given a type_id (BTF_KIND_STRUCT) and a pointer to raw struct bytes, this
 * code walks every non-bitfield member and emits a protobuf-compatible
 * tag+value record for each one.
 *
 * Encoding choices:
 *   BTF_KIND_INT       ->  WIRE_VARINT (signed: zigzag; unsigned: plain)
 *   BTF_KIND_FLOAT     ->  WIRE_I32 (4-byte float), WIRE_I64 (8-byte double),
 *                          or WIRE_LEN (other widths, e.g. 10-byte x87 long
 *                          double or 2-byte half-precision)
 *   BTF_KIND_PTR       ->  WIRE_I64 (always 8 bytes, zero-padded on 32-bit)
 *   BTF_KIND_ENUM      ->  WIRE_VARINT (signed: zigzag; unsigned: plain u32)
 *   BTF_KIND_ENUM64    ->  WIRE_VARINT (signed: zigzag; unsigned: plain u64)
 *   BTF_KIND_ARRAY     ->  WIRE_LEN  (char[] as length-prefixed string;
 *                                     other arrays as raw bytes)
 *   BTF_KIND_STRUCT/
 *   BTF_KIND_UNION     ->  WIRE_LEN  (recursively encoded sub-message, up to
 *                                     KSERIAL_MAX_DEPTH levels deep; beyond
 *                                     that: raw bytes)
 *   modifiers          ->  stripped via btf_type_skip_modifiers()
 *   bitfields          ->  skipped (not yet supported)
 * =======================================================================
 */

struct ks_enc {
	u8    *buf;
	size_t size;
	size_t pos;
	int    depth;
};

static int ks_enc_write(struct ks_enc *e, const void *data, size_t len)
{
	if (e->pos + len > e->size)
		return -ENOSPC;
	memcpy(e->buf + e->pos, data, len);
	e->pos += len;
	return 0;
}

static int ks_enc_varint(struct ks_enc *e, u64 v)
{
	u8 tmp[10];
	int n = ks_varint_encode(tmp, sizeof(tmp), v);

	if (n < 0)
		return n;
	return ks_enc_write(e, tmp, n);
}

static int ks_enc_tag(struct ks_enc *e, u32 field_num, u32 wire_type)
{
	return ks_enc_varint(e, KSERIAL_TAG(field_num, wire_type));
}

/* Forward declaration for recursive struct encoding. */
static int ks_encode_struct(const struct btf *btf, u32 type_id,
			     const u8 *data, size_t data_size,
			     struct ks_enc *e);

/**
 * ks_encode_member() - encode one struct member
 * @btf:     vmlinux BTF descriptor
 * @type_id: BTF type ID of the member (before modifier resolution)
 * @fn:      1-based field number (BTF member index + 1)
 * @data:    pointer to the member's bytes within the parent struct
 * @e:       output encoding context
 */
static int ks_encode_member(const struct btf *btf, u32 type_id,
			     u32 fn, const u8 *data, struct ks_enc *e)
{
	const struct btf_type *t;
	u32 res_id;
	int ret;

	t = btf_type_skip_modifiers(btf, type_id, &res_id);
	if (!t)
		return -EINVAL;

	switch (BTF_INFO_KIND(t->info)) {

	case BTF_KIND_INT: {
		u32 int_data = *(const u32 *)(t + 1);
		u32 bits     = BTF_INT_BITS(int_data);
		bool is_signed = !!(BTF_INT_ENCODING(int_data) & BTF_INT_SIGNED);
		u64 val;

		switch (bits) {
		case 8:
			val = is_signed ? ks_zigzag(*(const s8  *)data)
					: *(const u8  *)data;
			break;
		case 16:
			val = is_signed ? ks_zigzag(*(const s16 *)data)
					: *(const u16 *)data;
			break;
		case 32:
			val = is_signed ? ks_zigzag(*(const s32 *)data)
					: *(const u32 *)data;
			break;
		case 64:
			val = is_signed ? ks_zigzag(*(const s64 *)data)
					: *(const u64 *)data;
			break;
		default:
			return 0; /* unsupported width — skip */
		}

		ret = ks_enc_tag(e, fn, KSERIAL_WIRE_VARINT);
		if (ret)
			return ret;
		return ks_enc_varint(e, val);
	}

	case BTF_KIND_FLOAT:
		switch (t->size) {
		case 4:
			ret = ks_enc_tag(e, fn, KSERIAL_WIRE_I32);
			if (ret)
				return ret;
			return ks_enc_write(e, data, 4);
		case 8:
			ret = ks_enc_tag(e, fn, KSERIAL_WIRE_I64);
			if (ret)
				return ret;
			return ks_enc_write(e, data, 8);
		default:
			/*
			 * Unusual float width (e.g. 2-byte half, 10-byte x87
			 * long double).  Neither WIRE_I32 nor WIRE_I64 can
			 * represent an arbitrary number of bytes, so fall back
			 * to a length-delimited raw payload.
			 */
			ret = ks_enc_tag(e, fn, KSERIAL_WIRE_LEN);
			if (ret)
				return ret;
			ret = ks_enc_varint(e, t->size);
			if (ret)
				return ret;
			return ks_enc_write(e, data, t->size);
		}

	case BTF_KIND_PTR: {
		/*
		 * Always encode as 8 bytes to keep the format architecture-
		 * independent.  On 32-bit kernels the upper 4 bytes are zero.
		 */
		u64 pval = 0;

		memcpy(&pval, data, sizeof(void *));
		ret = ks_enc_tag(e, fn, KSERIAL_WIRE_I64);
		if (ret)
			return ret;
		return ks_enc_write(e, &pval, 8);
	}

	case BTF_KIND_ENUM: {
		/*
		 * Since Linux 6.0, BTF_INFO_KFLAG(t->info) == 1 marks a
		 * signed enum.  Encode signed enums with zigzag (consistent
		 * with BTF_KIND_INT) so negative values stay compact.
		 */
		bool is_signed = !!BTF_INFO_KFLAG(t->info);
		u64  val = is_signed ? ks_zigzag(*(const s32 *)data)
				     : *(const u32 *)data;

		ret = ks_enc_tag(e, fn, KSERIAL_WIRE_VARINT);
		if (ret)
			return ret;
		return ks_enc_varint(e, val);
	}

	case BTF_KIND_ENUM64: {
		/*
		 * Like BTF_KIND_ENUM, BTF_INFO_KFLAG(t->info) == 1 marks a
		 * signed 64-bit enum (Linux 6.0+).  Use zigzag for signed
		 * values so negative entries stay compact.
		 */
		bool is_signed = !!BTF_INFO_KFLAG(t->info);
		u64  val = is_signed ? ks_zigzag(*(const s64 *)data)
				     : *(const u64 *)data;

		ret = ks_enc_tag(e, fn, KSERIAL_WIRE_VARINT);
		if (ret)
			return ret;
		return ks_enc_varint(e, val);
	}

	case BTF_KIND_ARRAY: {
		const struct btf_array *arr = btf_array(t);
		const struct btf_type  *elem_t;
		u32 elem_res_id;
		u32 total = t->size;

		elem_t = btf_type_skip_modifiers(btf, arr->type, &elem_res_id);

		/*
		 * Treat an array of 8-bit integers (char, u8, s8) as a
		 * length-delimited byte string.  For char arrays we stop at
		 * the first NUL so the consumer gets a proper C string.
		 */
		if (elem_t &&
		    BTF_INFO_KIND(elem_t->info) == BTF_KIND_INT) {
			u32 edata = *(const u32 *)(elem_t + 1);

			if (BTF_INT_BITS(edata) == 8) {
				u32 slen = strnlen((const char *)data, total);

				ret = ks_enc_tag(e, fn, KSERIAL_WIRE_LEN);
				if (ret)
					return ret;
				ret = ks_enc_varint(e, slen);
				if (ret)
					return ret;
				return ks_enc_write(e, data, slen);
			}
		}

		/* All other arrays: raw bytes. */
		ret = ks_enc_tag(e, fn, KSERIAL_WIRE_LEN);
		if (ret)
			return ret;
		ret = ks_enc_varint(e, total);
		if (ret)
			return ret;
		return ks_enc_write(e, data, total);
	}

	case BTF_KIND_STRUCT:
	case BTF_KIND_UNION:
		if (e->depth >= KSERIAL_MAX_DEPTH) {
			/*
			 * Too deep to recurse safely; fall back to raw bytes so
			 * the message is still complete.
			 */
			ret = ks_enc_tag(e, fn, KSERIAL_WIRE_LEN);
			if (ret)
				return ret;
			ret = ks_enc_varint(e, t->size);
			if (ret)
				return ret;
			return ks_enc_write(e, data, t->size);
		}

		/*
		 * Recursively encode the nested struct into a temporary
		 * buffer, then emit it as a length-delimited sub-message.
		 * Allocate on the heap to avoid blowing the kernel stack.
		 */
		{
			size_t sub_size = max_t(size_t, (size_t)t->size * 8, 4096U);
			u8 *sub_buf;
			struct ks_enc sub;

			sub_buf = kmalloc(sub_size, GFP_KERNEL);
			if (!sub_buf)
				return -ENOMEM;

			sub.buf   = sub_buf;
			sub.size  = sub_size;
			sub.pos   = 0;
			sub.depth = e->depth + 1;

			ret = ks_encode_struct(btf, res_id, data, t->size, &sub);
			if (!ret) {
				ret = ks_enc_tag(e, fn, KSERIAL_WIRE_LEN);
				if (!ret)
					ret = ks_enc_varint(e, sub.pos);
				if (!ret)
					ret = ks_enc_write(e, sub_buf, sub.pos);
			}

			kfree(sub_buf);
			return ret;
		}

	default:
		return 0; /* skip unsupported type kinds */
	}
}

/**
 * ks_encode_struct() - encode a BTF struct into the @e output context
 * @btf:       vmlinux BTF descriptor
 * @type_id:   BTF type ID; must refer to a BTF_KIND_STRUCT or BTF_KIND_UNION
 * @data:      pointer to the start of the struct in memory
 * @data_size: available bytes at @data (must be >= BTF type size)
 * @e:         output encoding context
 *
 * Emits one protobuf-compatible tag+value record per non-bitfield member.
 * Bitfield members are currently silently skipped.
 */
static int ks_encode_struct(const struct btf *btf, u32 type_id,
			     const u8 *data, size_t data_size,
			     struct ks_enc *e)
{
	const struct btf_type   *t = btf_type_by_id(btf, type_id);
	const struct btf_member *member;
	int i, ret;

	if (!t)
		return -EINVAL;

	if (BTF_INFO_KIND(t->info) != BTF_KIND_STRUCT &&
	    BTF_INFO_KIND(t->info) != BTF_KIND_UNION)
		return -EINVAL;

	if (data_size < t->size)
		return -EINVAL;

	for_each_member(i, t, member) {
		u32 bit_off  = __btf_member_bit_offset(t, member);
		u32 byte_off = bit_off / 8;
		u32 bf_size  = __btf_member_bitfield_size(t, member);

		/* Bitfield encoding is not supported; skip silently. */
		if (bf_size)
			continue;

		if (byte_off >= data_size)
			break;

		/* field_number is 1-based to match protobuf conventions */
		ret = ks_encode_member(btf, member->type,
				       (u32)i + 1,
				       data + byte_off, e);
		if (ret < 0)
			return ret;
	}

	return 0;
}

/* =========================================================================
 * Per-fd context
 * =======================================================================
 */

struct ks_ctx {
	struct mutex	lock;	/* serialises write() calls per fd */
	struct ks_rb	rb;
};

/* =========================================================================
 * File operations
 * =======================================================================
 */

static int ks_open(struct inode *inode, struct file *file)
{
	struct ks_ctx *ctx;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mutex_init(&ctx->lock);

	ret = ks_rb_alloc(&ctx->rb);
	if (ret) {
		kfree(ctx);
		return ret;
	}

	file->private_data = ctx;
	return 0;
}

static int ks_release(struct inode *inode, struct file *file)
{
	struct ks_ctx *ctx = file->private_data;

	ks_rb_free(&ctx->rb);
	mutex_destroy(&ctx->lock);
	kfree(ctx);
	return 0;
}

/**
 * ks_write() - accept a kserial_req from userspace and encode it
 *
 * Expected write() layout:
 *   [struct kserial_req]
 *   [char type_name[req.name_len]]
 *   [u8   data[req.data_len]]
 */
static ssize_t ks_write(struct file *file, const char __user *ubuf,
			 size_t count, loff_t *ppos)
{
	struct ks_ctx		*ctx = file->private_data;
	struct kserial_req	 req;
	struct btf		*btf;
	char			*type_name = NULL;
	u8			*sdata = NULL;
	u8			*enc_buf = NULL;
	struct kserial_msg_hdr	*hdr;
	struct ks_enc		 enc;
	size_t			 name_off, data_off;
	size_t			 hdr_size;
	s32			 type_id;
	int			 ret;

	if (count < sizeof(req))
		return -EINVAL;
	if (count > KSERIAL_MAX_REQ_SIZE)
		return -E2BIG;

	if (copy_from_user(&req, ubuf, sizeof(req)))
		return -EFAULT;

	if (req.magic != KSERIAL_MAGIC)
		return -EINVAL;
	if (req.version != KSERIAL_VERSION)
		return -EINVAL;
	if (!req.name_len || req.name_len > KSERIAL_MAX_NAME)
		return -EINVAL;
	if (!req.data_len)
		return -EINVAL;

	name_off = sizeof(req);
	data_off = name_off + req.name_len;

	if (data_off > count || data_off + req.data_len != count)
		return -EINVAL;

	/* Get the vmlinux BTF — requires CONFIG_DEBUG_INFO_BTF=y. */
	btf = btf_get_vmlinux();
	if (IS_ERR_OR_NULL(btf))
		return IS_ERR(btf) ? PTR_ERR(btf) : -ENODATA;

	/* Copy the type name from userspace (adds a NUL terminator). */
	type_name = memdup_user_nul(ubuf + name_off, req.name_len);
	if (IS_ERR(type_name))
		return PTR_ERR(type_name);

	/* Copy the raw struct bytes from userspace. */
	sdata = kvmalloc(req.data_len, GFP_KERNEL);
	if (!sdata) {
		ret = -ENOMEM;
		goto out_name;
	}
	if (copy_from_user(sdata, ubuf + data_off, req.data_len)) {
		ret = -EFAULT;
		goto out_sdata;
	}

	/* Look up the struct type in vmlinux BTF. */
	type_id = btf_find_by_name_kind(btf, type_name, BTF_KIND_STRUCT);
	if (type_id < 0) {
		ret = type_id;
		goto out_sdata;
	}

	/*
	 * Allocate the encoding buffer:
	 *   kserial_msg_hdr  +  type_name  +  encoded fields
	 */
	hdr_size = sizeof(*hdr) + req.name_len;
	enc_buf  = kvmalloc(hdr_size + KSERIAL_MAX_ENC_SIZE, GFP_KERNEL);
	if (!enc_buf) {
		ret = -ENOMEM;
		goto out_sdata;
	}

	enc.buf   = enc_buf + hdr_size;
	enc.size  = KSERIAL_MAX_ENC_SIZE;
	enc.pos   = 0;
	enc.depth = 0;

	mutex_lock(&ctx->lock);

	ret = ks_encode_struct(btf, (u32)type_id, sdata, req.data_len, &enc);
	if (ret)
		goto out_unlock;

	/* Fill in the message header. */
	hdr           = (struct kserial_msg_hdr *)enc_buf;
	hdr->magic    = KSERIAL_MAGIC;
	hdr->version  = KSERIAL_VERSION;
	hdr->flags    = KSERIAL_FLAG_ZIGZAG; /* signed ints use zigzag */
	hdr->name_len = req.name_len;
	hdr->msg_len  = (u32)(hdr_size + enc.pos);
	hdr->reserved = 0;
	memcpy(enc_buf + sizeof(*hdr), type_name, req.name_len);

	ret = ks_rb_write(&ctx->rb, enc_buf, hdr->msg_len);
	if (!ret)
		ret = (int)count;

out_unlock:
	mutex_unlock(&ctx->lock);
	kvfree(enc_buf);
out_sdata:
	kvfree(sdata);
out_name:
	kfree(type_name);
	return ret;
}

/**
 * ks_read() - copy encoded messages from the ring buffer to userspace
 *
 * Blocks (unless O_NONBLOCK) until at least one byte is available.
 * Advances the ring-buffer consumer automatically.
 */
static ssize_t ks_read(struct file *file, char __user *ubuf,
		       size_t count, loff_t *ppos)
{
	struct ks_ctx *ctx = file->private_data;
	u8    *tmp;
	ssize_t n;
	int     ret;

	if (!count)
		return 0;

	count = min_t(size_t, count, KSERIAL_RB_DATA_SIZE);
	tmp = kvmalloc(count, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	if (file->f_flags & O_NONBLOCK) {
		n = ks_rb_read(&ctx->rb, tmp, count);
		if (!n)
			n = -EAGAIN;
	} else {
		/*
		 * Loop in case a concurrent KSERIAL_IOC_FLUSH or
		 * KSERIAL_IOC_RESET drains the buffer between
		 * wait_event_interruptible() returning and ks_rb_read()
		 * acquiring rb->lock.  Without the loop ks_rb_read() could
		 * return 0, which callers interpret as EOF.
		 *
		 * Use '>' rather than '!=' so that a mmap user who sets
		 * ctrl->consumer beyond the current producer does not cause
		 * a permanent busy-spin: when consumer > producer the condition
		 * is false and the task sleeps correctly.
		 *
		 * Pairs with smp_store_release() in ks_rb_write().
		 */
		do {
			ret = wait_event_interruptible(ctx->rb.waitq,
				smp_load_acquire(&ctx->rb.ctrl->producer) >
				READ_ONCE(ctx->rb.ctrl->consumer));
			if (ret) {
				kvfree(tmp);
				return ret;
			}
			n = ks_rb_read(&ctx->rb, tmp, count);
		} while (n == 0);
	}

	if (n > 0 && copy_to_user(ubuf, tmp, n))
		n = -EFAULT;

	kvfree(tmp);
	return n;
}

/**
 * ks_mmap() - map the per-fd ring buffer into userspace
 *
 * The mapping size must equal KSERIAL_RB_TOTAL_SIZE exactly:
 *   [0,         PAGE_SIZE)  ->  struct kserial_rb_ctrl  (ctrl page)
 *   [PAGE_SIZE, total_size) ->  circular data buffer
 *
 * The mapping is read-write.  Userspace mmap consumers may write @consumer
 * directly to advance past consumed bytes.  @producer is published by the
 * kernel via smp_store_release() for userspace to read, but writes to
 * ctrl->producer from userspace are ignored: all kernel-side capacity
 * calculations use the internal rb->producer field, which is only updated
 * under rb->lock and is not accessible through the mapping.
 *
 * Userspace mmap consumers must use appropriate load-acquire barriers
 * when reading @producer and store-release barriers when writing @consumer.
 */
static int ks_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct ks_ctx *ctx = file->private_data;

	if (vma->vm_end - vma->vm_start != KSERIAL_RB_TOTAL_SIZE)
		return -EINVAL;
	if (vma->vm_pgoff != 0)
		return -EINVAL;

	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
	return remap_vmalloc_range(vma, ctx->rb.base, 0);
}

static long ks_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct ks_ctx *ctx = file->private_data;

	switch (cmd) {
	case KSERIAL_IOC_FLUSH:
		spin_lock(&ctx->rb.lock);
		WRITE_ONCE(ctx->rb.ctrl->consumer, ctx->rb.producer);
		spin_unlock(&ctx->rb.lock);
		return 0;

	case KSERIAL_IOC_RESET:
		spin_lock(&ctx->rb.lock);
		ctx->rb.producer = 0;
		WRITE_ONCE(ctx->rb.ctrl->producer, 0);
		WRITE_ONCE(ctx->rb.ctrl->consumer, 0);
		spin_unlock(&ctx->rb.lock);
		return 0;

	case KSERIAL_IOC_GETINFO: {
		struct kserial_rb_ctrl snap;

		spin_lock(&ctx->rb.lock);
		memcpy(&snap, ctx->rb.ctrl, sizeof(snap));
		spin_unlock(&ctx->rb.lock);

		if (copy_to_user((void __user *)arg, &snap, sizeof(snap)))
			return -EFAULT;
		return 0;
	}

	case KSERIAL_IOC_CONSUME: {
		u64 n;

		if (copy_from_user(&n, (const void __user *)arg, sizeof(n)))
			return -EFAULT;

		spin_lock(&ctx->rb.lock);
		{
			u64 cons  = READ_ONCE(ctx->rb.ctrl->consumer);
			u64 avail;

			if (cons > ctx->rb.producer)
				cons = ctx->rb.producer;
			avail = ctx->rb.producer - cons;
			WRITE_ONCE(ctx->rb.ctrl->consumer,
				   cons + min(n, avail));
		}
		spin_unlock(&ctx->rb.lock);
		return 0;
	}

	default:
		return -ENOTTY;
	}
}

static __poll_t ks_poll(struct file *file, struct poll_table_struct *wait)
{
	struct ks_ctx *ctx = file->private_data;
	__poll_t mask = EPOLLOUT | EPOLLWRNORM; /* writes never block */

	poll_wait(file, &ctx->rb.waitq, wait);

	/*
	 * Use '>' rather than '!=' for the same reason as ks_read(): a mmap
	 * user who sets ctrl->consumer beyond the current producer must not
	 * cause a level-triggered epoll loop to spin indefinitely.
	 * Pairs with smp_store_release() in ks_rb_write().
	 */
	if (smp_load_acquire(&ctx->rb.ctrl->producer) >
	    READ_ONCE(ctx->rb.ctrl->consumer))
		mask |= EPOLLIN | EPOLLRDNORM;

	return mask;
}

static const struct file_operations ks_fops = {
	.owner          = THIS_MODULE,
	.open           = ks_open,
	.release        = ks_release,
	.write          = ks_write,
	.read           = ks_read,
	.mmap           = ks_mmap,
	.unlocked_ioctl = ks_ioctl,
	.compat_ioctl   = ks_ioctl,
	.poll           = ks_poll,
	.llseek         = no_llseek,
};

static struct miscdevice ks_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "kserial",
	.fops  = &ks_fops,
	.mode  = 0600,
};

/* =========================================================================
 * Module init / exit
 * =======================================================================
 */

static int __init kserial_init(void)
{
	int ret = misc_register(&ks_miscdev);

	if (!ret)
		pr_debug("kserial: registered /dev/%s\n", ks_miscdev.name);
	return ret;
}

static void __exit kserial_exit(void)
{
	misc_deregister(&ks_miscdev);
}

module_init(kserial_init);
module_exit(kserial_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("BTF-aware binary serialization for user<->kernel IPC");
MODULE_AUTHOR("Jianyue Wu <wujianyue000@gmail.com>");

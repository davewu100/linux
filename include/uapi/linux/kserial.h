/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * kserial - Kernel Serial Protocol
 *
 * A BTF-aware binary serialization format for user<->kernel communication.
 * The wire format is protobuf-compatible: same varint encoding, same wire
 * types.  Field numbers map 1:1 to BTF struct member indices (1-based).
 *
 * Protocol:
 *   1. User writes a kserial_req to /dev/kserial, followed by the type name
 *      and the raw struct bytes to serialize.
 *   2. Kernel looks up the type in vmlinux BTF, walks the struct members, and
 *      encodes each field as a protobuf-compatible tag+value record.
 *   3. The encoded kserial message is placed into the per-fd ring buffer.
 *   4. User consumes the encoded message via read() or the mmap'd ring buffer.
 *
 * Wire format of one encoded message:
 *
 *   [kserial_msg_hdr]            -- fixed-size header (magic, msg_len, ...)
 *   [char type_name[name_len]]   -- null-terminated struct name
 *   [field records ...]          -- protobuf-compatible tag+value pairs
 *
 * Field record encoding (identical to protobuf):
 *
 *   tag     = varint( (field_number << 3) | wire_type )
 *   value   = depends on wire_type:
 *               WIRE_VARINT  ->  LEB128 unsigned varint
 *                                (signed integers use zigzag encoding when
 *                                 KSERIAL_FLAG_ZIGZAG is set in msg_hdr.flags)
 *               WIRE_I64     ->  8 bytes little-endian
 *               WIRE_LEN     ->  varint length + <length> bytes
 *               WIRE_I32     ->  4 bytes little-endian
 *
 * field_number is the 1-based BTF member index within the struct.
 */

#ifndef _UAPI_LINUX_KSERIAL_H
#define _UAPI_LINUX_KSERIAL_H

#include <linux/types.h>
#include <linux/ioctl.h>

/* -----------------------------------------------------------------------
 * Protocol constants
 * ---------------------------------------------------------------------
 */

#define KSERIAL_MAGIC    0x4B534552U   /* "KSER" */
#define KSERIAL_VERSION  1
#define KSERIAL_MAX_NAME 128           /* max type name length incl. NUL */

/*
 * Wire types — identical to protobuf wire types.
 * Encoded as the low 3 bits of the tag varint.
 */
#define KSERIAL_WIRE_VARINT  0  /* LEB128: int, uint (any width), bool, enum */
#define KSERIAL_WIRE_I64     1  /* 8-byte LE: double, pointer */
#define KSERIAL_WIRE_LEN     2  /* varint length + bytes: arrays, sub-structs */
/* wire types 3 and 4 are reserved (deprecated in protobuf, unused here) */
#define KSERIAL_WIRE_I32     5  /* 4-byte LE: float */

/*
 * Encode a (field_number, wire_type) pair into a tag.
 * Matches the protobuf tag encoding exactly.
 */
#define KSERIAL_TAG(field_num, wire_type)  \
	(((__u32)(field_num) << 3) | ((__u32)(wire_type) & 0x7))

/*
 * Message / request flags.
 */
/* Signed integers in this message use zigzag encoding (set by kernel). */
#define KSERIAL_FLAG_ZIGZAG  (1 << 0)

/* -----------------------------------------------------------------------
 * Structures
 * ---------------------------------------------------------------------
 */

/**
 * struct kserial_req - request header written to /dev/kserial
 *
 * The write() buffer must be laid out as:
 *
 *   [struct kserial_req]           -- this header
 *   [char  type_name[name_len]]    -- BTF struct name, null-terminated
 *   [__u8  data[data_len]]         -- raw struct bytes to serialize
 *
 * The total write() size must equal sizeof(kserial_req) + name_len + data_len.
 */
struct kserial_req {
	__u32 magic;       /* KSERIAL_MAGIC */
	__u8  version;     /* KSERIAL_VERSION */
	__u8  flags;       /* KSERIAL_FLAG_* (reserved for future use) */
	__u16 name_len;    /* length of type_name including NUL terminator */
	__u32 data_len;    /* length of raw struct data in bytes */
	__u32 reserved;
};

/**
 * struct kserial_msg_hdr - header of an encoded message in the ring buffer
 *
 * Every message in the ring buffer starts with this header, followed by:
 *
 *   [char  type_name[name_len]]    -- struct name (null-terminated)
 *   [field records ...]            -- protobuf-compatible tag+value pairs
 *
 * msg_len covers the full message including this header.
 */
struct kserial_msg_hdr {
	__u32 magic;       /* KSERIAL_MAGIC */
	__u8  version;     /* KSERIAL_VERSION */
	__u8  flags;       /* KSERIAL_FLAG_* */
	__u16 name_len;    /* length of type_name including NUL terminator */
	__u32 msg_len;     /* total message length in bytes */
	__u32 reserved;
};

/**
 * struct kserial_rb_ctrl - ring buffer control page (at mmap offset 0)
 *
 * The ring buffer data region follows immediately at mmap offset PAGE_SIZE.
 *
 * Consuming via mmap (zero-copy):
 *   1. Read @producer to determine how many bytes the kernel has written.
 *   2. Read bytes from data[consumer % data_size .. consumer % data_size + avail)
 *      wrapping at data_size if necessary.
 *   3. After consuming N bytes, issue KSERIAL_IOC_CONSUME(N) or update
 *      @consumer directly, with an appropriate store-release barrier.
 *
 * Consuming via read():
 *   The kernel copies data out and advances @consumer automatically.
 *
 * Do not mix mmap-based and read()-based consumption on the same fd.
 */
struct kserial_rb_ctrl {
	__u32 magic;           /* KSERIAL_MAGIC */
	__u32 version;         /* KSERIAL_VERSION */
	__u64 data_size;       /* size of data ring buffer in bytes */
	__u64 producer;        /* kernel writes here (monotonically increasing) */
	__u64 consumer;        /* reader writes here after consuming bytes */
	__u32 flags;
	__u32 reserved[3];
};

/* -----------------------------------------------------------------------
 * ioctl commands
 * ---------------------------------------------------------------------
 */

#define KSERIAL_IOC_MAGIC  'K'

/** KSERIAL_IOC_FLUSH - discard all unread data (advance consumer to producer) */
#define KSERIAL_IOC_FLUSH    _IO(KSERIAL_IOC_MAGIC,  1)

/** KSERIAL_IOC_RESET - reset both producer and consumer to 0 */
#define KSERIAL_IOC_RESET    _IO(KSERIAL_IOC_MAGIC,  2)

/** KSERIAL_IOC_GETINFO - snapshot of kserial_rb_ctrl */
#define KSERIAL_IOC_GETINFO  _IOR(KSERIAL_IOC_MAGIC, 3, struct kserial_rb_ctrl)

/** KSERIAL_IOC_CONSUME - advance consumer by N bytes (for mmap users) */
#define KSERIAL_IOC_CONSUME  _IOW(KSERIAL_IOC_MAGIC, 4, __u64)

#endif /* _UAPI_LINUX_KSERIAL_H */

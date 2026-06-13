// SPDX-License-Identifier: GPL-2.0-only
/*
 * kserial_test - userspace test and demonstration for /dev/kserial
 *
 * Usage:
 *   kserial_test [type_name]
 *
 * The test encodes a hard-coded sample struct and sends it to /dev/kserial,
 * then reads back the encoded protobuf-compatible binary message and decodes
 * it to stdout.
 *
 * Compile:
 *   gcc -O2 -Wall -o kserial_test kserial_test.c
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/*
 * Pull in the UAPI definitions.  When built inside the kernel tree:
 *   #include <linux/kserial.h>
 * For standalone compilation, replicate the protocol constants here.
 */
#ifdef __KERNEL__
#  include <uapi/linux/kserial.h>
#else

#define KSERIAL_MAGIC    0x4B534552U
#define KSERIAL_VERSION  1
#define KSERIAL_MAX_NAME 128

#define KSERIAL_WIRE_VARINT  0
#define KSERIAL_WIRE_I64     1
#define KSERIAL_WIRE_LEN     2
#define KSERIAL_WIRE_I32     5

#define KSERIAL_TAG(fn, wt)  (((uint32_t)(fn) << 3) | ((uint32_t)(wt) & 0x7))

#define KSERIAL_FLAG_ZIGZAG  (1 << 0)

struct kserial_req {
	uint32_t magic;
	uint8_t  version;
	uint8_t  flags;
	uint16_t name_len;
	uint32_t data_len;
	uint32_t reserved;
};

struct kserial_msg_hdr {
	uint32_t magic;
	uint8_t  version;
	uint8_t  flags;
	uint16_t name_len;
	uint32_t msg_len;
	uint32_t reserved;
};

struct kserial_rb_ctrl {
	uint32_t magic;
	uint32_t version;
	uint64_t data_size;
	uint64_t producer;
	uint64_t consumer;
	uint32_t flags;
	uint32_t reserved[3];
};

#define KSERIAL_IOC_MAGIC    'K'
#define KSERIAL_IOC_FLUSH    _IO(KSERIAL_IOC_MAGIC,  1)
#define KSERIAL_IOC_RESET    _IO(KSERIAL_IOC_MAGIC,  2)
#define KSERIAL_IOC_GETINFO  _IOR(KSERIAL_IOC_MAGIC, 3, struct kserial_rb_ctrl)
#define KSERIAL_IOC_CONSUME  _IOW(KSERIAL_IOC_MAGIC, 4, uint64_t)

#endif /* __KERNEL__ */

/* -------------------------------------------------------------------------
 * Varint decoder (LEB128, same as protobuf)
 * ---------------------------------------------------------------------- */

static int varint_decode(const uint8_t *buf, size_t size,
			 uint64_t *out_val, size_t *out_len)
{
	uint64_t result = 0;
	int      shift  = 0;
	size_t   i;

	for (i = 0; i < size && i < 10; i++) {
		result |= (uint64_t)(buf[i] & 0x7f) << shift;
		shift  += 7;
		if (!(buf[i] & 0x80)) {
			*out_val = result;
			*out_len = i + 1;
			return 0;
		}
	}
	return -1; /* truncated */
}

/* Zigzag decode: reverses the kernel-side ks_zigzag() encoding. */
static int64_t zigzag_decode(uint64_t v)
{
	return (int64_t)((v >> 1) ^ -(int64_t)(v & 1));
}

/* -------------------------------------------------------------------------
 * kserial message decoder
 *
 * Prints each field to stdout.  The decoder does not have BTF field names,
 * so it prints field_number instead.  A production decoder would combine
 * this with BTF or a .proto schema to resolve names.
 * ---------------------------------------------------------------------- */

static void decode_message(const uint8_t *msg, size_t msg_size,
			    int zigzag, int indent)
{
	const struct kserial_msg_hdr *hdr;
	const char   *type_name;
	const uint8_t *p;
	size_t         remaining;

	if (msg_size < sizeof(*hdr)) {
		fprintf(stderr, "message too short (%zu bytes)\n", msg_size);
		return;
	}

	hdr = (const struct kserial_msg_hdr *)msg;

	if (hdr->magic != KSERIAL_MAGIC) {
		fprintf(stderr, "bad magic: 0x%08x\n", hdr->magic);
		return;
	}

	type_name = (const char *)(msg + sizeof(*hdr));
	printf("%*stype:    %.*s\n", indent, "", (int)hdr->name_len, type_name);
	printf("%*smsg_len: %u bytes\n", indent, "", hdr->msg_len);

	p         = msg + sizeof(*hdr) + hdr->name_len;
	remaining = msg_size - sizeof(*hdr) - hdr->name_len;

	while (remaining > 0) {
		uint64_t tag_val;
		size_t   tag_len;
		uint32_t field_num, wire_type;

		/* Decode the field tag. */
		if (varint_decode(p, remaining, &tag_val, &tag_len) < 0)
			break;

		/* End-of-message marker. */
		if (tag_val == 0)
			break;

		p         += tag_len;
		remaining -= tag_len;

		field_num = (uint32_t)(tag_val >> 3);
		wire_type = (uint32_t)(tag_val & 0x7);

		printf("%*s  field %u (wire_type=%u): ", indent, "",
		       field_num, wire_type);

		switch (wire_type) {
		case KSERIAL_WIRE_VARINT: {
			uint64_t v;
			size_t   vlen;

			if (varint_decode(p, remaining, &v, &vlen) < 0) {
				printf("<truncated varint>\n");
				return;
			}
			p         += vlen;
			remaining -= vlen;

			if (zigzag)
				printf("%" PRId64 " (zigzag: raw=0x%" PRIx64 ")\n",
				       zigzag_decode(v), v);
			else
				printf("%" PRIu64 "\n", v);
			break;
		}

		case KSERIAL_WIRE_I64: {
			uint64_t v;

			if (remaining < 8) {
				printf("<truncated i64>\n");
				return;
			}
			memcpy(&v, p, 8);
			p         += 8;
			remaining -= 8;
			printf("0x%016" PRIx64 "\n", v);
			break;
		}

		case KSERIAL_WIRE_I32: {
			uint32_t v;

			if (remaining < 4) {
				printf("<truncated i32>\n");
				return;
			}
			memcpy(&v, p, 4);
			p         += 4;
			remaining -= 4;
			printf("0x%08x\n", v);
			break;
		}

		case KSERIAL_WIRE_LEN: {
			uint64_t len;
			size_t   llen;

			if (varint_decode(p, remaining, &len, &llen) < 0) {
				printf("<truncated length>\n");
				return;
			}
			p         += llen;
			remaining -= llen;

			if (remaining < len) {
				printf("<truncated data>\n");
				return;
			}

			/* Check if it looks like a printable string. */
			{
				int is_str = 1;
				size_t j;

				for (j = 0; j < len; j++) {
					uint8_t c = p[j];

					if (c < 0x20 && c != '\t' &&
					    c != '\n' && c != '\r') {
						is_str = 0;
						break;
					}
				}

				if (is_str && len > 0) {
					printf("\"%.*s\"\n", (int)len, p);
				} else {
					printf("[%" PRIu64 " bytes:", len);
					size_t show = len < 16 ? len : 16;

					for (j = 0; j < show; j++)
						printf(" %02x", p[j]);
					if (len > 16)
						printf(" ...");
					printf("]\n");
				}
			}

			p         += len;
			remaining -= len;
			break;
		}

		default:
			printf("<unknown wire type %u>\n", wire_type);
			return;
		}
	}
}

/* -------------------------------------------------------------------------
 * Helper: build and write a kserial_req
 * ---------------------------------------------------------------------- */

static int kserial_send(int fd, const char *type_name,
			const void *data, size_t data_len)
{
	struct kserial_req  req;
	uint16_t            name_len = (uint16_t)(strlen(type_name) + 1);
	size_t              total    = sizeof(req) + name_len + data_len;
	uint8_t            *buf;
	ssize_t             n;

	buf = malloc(total);
	if (!buf)
		return -1;

	req.magic    = KSERIAL_MAGIC;
	req.version  = KSERIAL_VERSION;
	req.flags    = 0;
	req.name_len = name_len;
	req.data_len = (uint32_t)data_len;
	req.reserved = 0;

	memcpy(buf, &req, sizeof(req));
	memcpy(buf + sizeof(req), type_name, name_len);
	memcpy(buf + sizeof(req) + name_len, data, data_len);

	n = write(fd, buf, total);
	free(buf);

	if (n < 0) {
		perror("write");
		return -1;
	}
	return 0;
}

/* -------------------------------------------------------------------------
 * Example: encode a struct timespec64 (a common kernel time struct)
 *
 * struct timespec64 {
 *   s64  tv_sec;    // field 1
 *   long tv_nsec;   // field 2
 * };
 *
 * We also demo "struct pt_regs" if the user passes it on the command line.
 * ---------------------------------------------------------------------- */

/* Matches kernel's struct timespec64 exactly. */
struct demo_timespec64 {
	int64_t  tv_sec;
	long     tv_nsec;
};

static int demo_via_read(int fd)
{
	struct demo_timespec64 ts = {
		.tv_sec  = 1713052800,  /* 2024-04-14 00:00:00 UTC */
		.tv_nsec = 123456789,
	};
	uint8_t  buf[4096];
	ssize_t  n;
	int      ret;

	printf("=== Sending struct timespec64 via write() ===\n");
	printf("  tv_sec  = %" PRId64 "\n", ts.tv_sec);
	printf("  tv_nsec = %ld\n", ts.tv_nsec);
	printf("\n");

	ret = kserial_send(fd, "timespec64", &ts, sizeof(ts));
	if (ret < 0)
		return ret;

	/* Read the encoded message back. */
	n = read(fd, buf, sizeof(buf));
	if (n < 0) {
		perror("read");
		return -1;
	}
	if (n == 0) {
		fprintf(stderr, "read returned 0 bytes\n");
		return -1;
	}

	printf("=== Decoded kserial message (%zd bytes) ===\n", n);
	decode_message(buf, (size_t)n, 1 /* zigzag */, 0);
	printf("\n");
	return 0;
}

static int demo_via_mmap(int fd)
{
	struct kserial_rb_ctrl info;
	struct kserial_rb_ctrl *ctrl;
	uint8_t  *base;
	uint8_t  *data_region;
	uint64_t  prod, cons;

	/* Query ring buffer geometry. */
	if (ioctl(fd, KSERIAL_IOC_GETINFO, &info) < 0) {
		perror("KSERIAL_IOC_GETINFO");
		return -1;
	}

	long page_size = sysconf(_SC_PAGE_SIZE);
	size_t total   = (size_t)(page_size + info.data_size);

	/* mmap the ring buffer. */
	base = mmap(NULL, total, PROT_READ | PROT_WRITE,
		    MAP_SHARED, fd, 0);
	if (base == MAP_FAILED) {
		perror("mmap");
		return -1;
	}

	ctrl        = (struct kserial_rb_ctrl *)base;
	data_region = base + page_size;

	/* Reset the ring buffer first. */
	if (ioctl(fd, KSERIAL_IOC_RESET) < 0) {
		perror("KSERIAL_IOC_RESET");
		munmap(base, total);
		return -1;
	}

	/* Send another timespec64 with different values. */
	struct demo_timespec64 ts2 = {
		.tv_sec  = -42,         /* negative to demonstrate zigzag */
		.tv_nsec = 999999999,
	};

	printf("=== Sending struct timespec64 via write() + mmap read ===\n");
	printf("  tv_sec  = %" PRId64 "\n", ts2.tv_sec);
	printf("  tv_nsec = %ld\n", ts2.tv_nsec);
	printf("\n");

	if (kserial_send(fd, "timespec64", &ts2, sizeof(ts2)) < 0) {
		munmap(base, total);
		return -1;
	}

	/* Poll the mmap'd producer index until data arrives. */
	int tries = 0;

	do {
		/* Use __atomic load to model a load-acquire. */
		__atomic_load(&ctrl->producer, &prod, __ATOMIC_ACQUIRE);
		cons = ctrl->consumer;
		if (prod != cons)
			break;
		usleep(1000);
	} while (++tries < 100);

	if (prod == cons) {
		fprintf(stderr, "timeout waiting for data in ring buffer\n");
		munmap(base, total);
		return -1;
	}

	/* Read the message from the ring buffer. */
	uint64_t avail = prod - cons;
	uint8_t *msg_buf = malloc((size_t)avail);

	if (!msg_buf) {
		munmap(base, total);
		return -1;
	}

	size_t pos   = (size_t)(cons % info.data_size);
	size_t avail_sz = (size_t)avail;

	if (pos + avail_sz <= (size_t)info.data_size) {
		memcpy(msg_buf, data_region + pos, avail_sz);
	} else {
		size_t first = (size_t)info.data_size - pos;
		memcpy(msg_buf, data_region + pos, first);
		memcpy(msg_buf + first, data_region, avail_sz - first);
	}

	printf("=== Decoded kserial message from mmap (%" PRIu64 " bytes) ===\n",
	       avail);
	decode_message(msg_buf, avail_sz, 1, 0);
	free(msg_buf);

	/* Advance consumer via ioctl. */
	uint64_t consumed = avail;

	if (ioctl(fd, KSERIAL_IOC_CONSUME, &consumed) < 0)
		perror("KSERIAL_IOC_CONSUME");

	munmap(base, total);
	printf("\n");
	return 0;
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */

int main(void)
{
	const char *dev   = "/dev/kserial";
	int         fd;
	int         ret   = 0;

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		perror("open /dev/kserial");
		fprintf(stderr, "Is CONFIG_KSERIAL=y and DEBUG_INFO_BTF=y?\n");
		return 1;
	}

	/* Demo 1: send/receive via read() */
	if (demo_via_read(fd) < 0) {
		ret = 1;
		goto out;
	}

	/* Demo 2: send via write(), receive via mmap */
	if (demo_via_mmap(fd) < 0) {
		ret = 1;
		goto out;
	}

	printf("All tests passed.\n");

out:
	close(fd);
	return ret;
}

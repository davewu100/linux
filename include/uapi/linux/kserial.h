/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UAPI_LINUX_KSERIAL_H
#define _UAPI_LINUX_KSERIAL_H

#include <linux/types.h>

/*
 * kserial: BTF-based field query for memory.stat.ks and memory.numa_stat.ks.
 * Supports nested paths (e.g. "vmstats.state[14]") and caches lookups.
 */
#define KS_MAX_FIELDS 128
#define KS_FIELD_NAME_LEN 64
#define KS_MAX_OUTPUT_SIZE 4096
#define KS_MAX_PATH_DEPTH 4

#define KS_FLAG_ALLOW_NULL 0x01
#define KS_FLAG_FLUSH      0x02  /* memcg: flush stats before read (fresh data) */

struct ks_schema {
	__u32 nr_fields;
	__u32 flags;
	char struct_name[KS_FIELD_NAME_LEN];
	__u32 target_pid;
	__u32 reserved[3];
	__u32 block_offset;
	__u32 block_size;
	__u32 array_start;
	__u32 array_count;
	char field_names[KS_MAX_FIELDS][KS_FIELD_NAME_LEN];
};

struct ks_tlv {
	__u16 field_id;
	__u16 len;
	__u8  data[];
} __attribute__((packed));

struct ks_result {
	__u32 total_len;
	__u8  data[KS_MAX_OUTPUT_SIZE];
};

/* Optional output: resolved (offset, size) per field for direct read cache */
struct ks_resolved_field {
	__u32 offset;
	__u32 size;
};

#endif /* _UAPI_LINUX_KSERIAL_H */

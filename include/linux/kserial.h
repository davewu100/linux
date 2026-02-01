/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_KSERIAL_H
#define _LINUX_KSERIAL_H

#include <linux/types.h>

/*
 * kserial: BTF-based field query for memory.stat.ks and memory.numa_stat.ks.
 * Supports nested paths (e.g. "vmstats.state[14]") and caches lookups.
 */

#define KS_MAX_FIELDS 16
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

#ifdef __KERNEL__

#include <linux/btf.h>
#include <linux/cgroup.h>
#include <linux/rhashtable.h>

int ks_query_struct(void *struct_addr, const char *struct_name,
		    const struct ks_schema *schema, struct ks_result *result);

struct ks_cache_entry {
	struct rhash_head node;
	char struct_name[KS_FIELD_NAME_LEN];
	char field_path[KS_FIELD_NAME_LEN];
	u32 offset;
	u32 size;
	u32 type_id;
	u8 flags;
	u64 created_ns;
	u64 hits;
	u64 last_access_ns;
};

struct ks_cache_entry *ks_cache_lookup(const char *struct_name,
				       const char *field_path);
int ks_cache_insert(const char *struct_name, const char *field_path,
		    u32 offset, u32 size, u32 type_id, u8 flags);

#endif /* __KERNEL__ */

#endif /* _LINUX_KSERIAL_H */

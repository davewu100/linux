/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_KSERIAL_H
#define _LINUX_KSERIAL_H

#ifdef __KERNEL__

#include <linux/types.h>
#include <linux/btf.h>
#include <linux/cgroup.h>
#include <linux/rhashtable.h>

/*
 * kserial: BTF-based field query support for memory.stat.ks
 * and memory.numa_stat.ks. This is a kernel-internal API.
 */
#define KS_MAX_FIELDS 128
#define KS_FIELD_NAME_LEN 64
#define KS_MAX_OUTPUT_SIZE 4096
#define KS_MAX_PATH_DEPTH 4

#define KS_FLAG_ALLOW_NULL 0x01
#define KS_FLAG_FLUSH      0x02  /* memcg: flush stats before read */

struct ks_schema {
	u32 nr_fields;
	u32 flags;
	char struct_name[KS_FIELD_NAME_LEN];
	u32 target_pid;
	u32 reserved[3];
	u32 block_offset;
	u32 block_size;
	u32 array_start;
	u32 array_count;
	char field_names[KS_MAX_FIELDS][KS_FIELD_NAME_LEN];
};

struct ks_tlv {
	u16 field_id;
	u16 len;
	u8  data[];
} __packed;

struct ks_result {
	u32 total_len;
	u8  data[KS_MAX_OUTPUT_SIZE];
};

/* Optional output: resolved (offset, size) per field for direct read cache */
struct ks_resolved_field {
	u32 offset;
	u32 size;
};

int ks_query_struct(void *struct_addr, const char *struct_name,
		    const struct ks_schema *schema, struct ks_result *result,
		    struct ks_resolved_field *resolved_out);

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

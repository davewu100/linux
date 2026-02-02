/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_KSERIAL_H
#define _LINUX_KSERIAL_H

#include <uapi/linux/kserial.h>

#ifdef __KERNEL__

#include <linux/btf.h>
#include <linux/cgroup.h>
#include <linux/rhashtable.h>

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

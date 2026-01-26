/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_KSERIAL_H
#define _LINUX_KSERIAL_H

#include <linux/types.h>

/*
 * k-serial: Dynamic field subscription for kernel structs
 * Phase 2: Supports nested struct fields (e.g., "css_set.dfl_cgrp.level")
 * Phase 3: Supports array indexing (e.g., "subsys[0]", "nr_dying_subsys[2]")
 */

#define KS_MAX_FIELDS 16
#define KS_FIELD_NAME_LEN 64  /* Increased for nested paths */
#define KS_MAX_OUTPUT_SIZE 4096
#define KS_MAX_PATH_DEPTH 4   /* Maximum nesting depth */

/* Schema flags */
#define KS_FLAG_ALLOW_NULL 0x01  /* Return 0 for NULL pointers instead of error */

/* User space schema: list of field names/paths to query */
struct ks_schema {
	__u32 nr_fields;
	__u32 flags;
	char field_names[KS_MAX_FIELDS][KS_FIELD_NAME_LEN];
};

/* Output format: TLV (Type-Length-Value) */
struct ks_tlv {
	__u16 field_id;   /* Index in schema */
	__u16 len;        /* Length of data */
	__u8  data[];     /* Field value */
} __attribute__((packed));

/* Result buffer */
struct ks_result {
	__u32 total_len;  /* Total bytes written */
	__u8  data[KS_MAX_OUTPUT_SIZE];
};

#ifdef __KERNEL__

#include <linux/btf.h>
#include <linux/cgroup.h>

/*
 * No whitelist - k-serial allows querying any field that BTF can resolve
 * 
 * Security model:
 * - BTF type checking ensures only valid struct fields are accessed
 * - Only scalar types (int, u64, pointers) are returned
 * - Complex types (strings, nested structs) are rejected
 * - Array bounds are checked automatically
 * 
 * This is safe because:
 * 1. BTF prevents access to non-existent fields
 * 2. Type validation prevents reading arbitrary memory
 * 3. Users can only query their own cgroup (via current task)
 */

/**
 * ks_query_cgroup - Query fields from a cgroup using BTF
 * @cgrp: Target cgroup
 * @schema: User-provided field list
 * @result: Output buffer for TLV-encoded data
 * 
 * Returns: 0 on success, negative error code on failure
 */
int ks_query_cgroup(struct cgroup *cgrp, const struct ks_schema *schema,
		    struct ks_result *result);

#endif /* __KERNEL__ */

#endif /* _LINUX_KSERIAL_H */

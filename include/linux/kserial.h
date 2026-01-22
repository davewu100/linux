/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_KSERIAL_H
#define _LINUX_KSERIAL_H

#include <linux/types.h>

/*
 * k-serial: Dynamic field subscription for kernel structs
 * MVP: Only supports struct cgroup, scalar fields, read-only access
 */

#define KS_MAX_FIELDS 16
#define KS_FIELD_NAME_LEN 32
#define KS_MAX_OUTPUT_SIZE 4096

/* User space schema: list of field names to query */
struct ks_schema {
	__u32 nr_fields;
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

/* Whitelist of allowed fields for struct cgroup */
static const char *ks_cgroup_whitelist[] = {
	"level",
	"max_depth",
	"nr_descendants",
	"nr_dying_descendants",
	"max_descendants",
	NULL
};

/**
 * ks_validate_field - Check if field name is in whitelist
 * @field_name: Field name to validate
 * 
 * Returns: true if allowed, false otherwise
 */
static inline bool ks_validate_field(const char *field_name)
{
	int i;
	for (i = 0; ks_cgroup_whitelist[i]; i++) {
		if (!strcmp(field_name, ks_cgroup_whitelist[i]))
			return true;
	}
	return false;
}

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

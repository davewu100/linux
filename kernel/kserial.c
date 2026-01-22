// SPDX-License-Identifier: GPL-2.0
/*
 * k-serial: Dynamic field subscription for kernel structs
 * 
 * This MVP implementation allows userspace to query specific fields
 * from struct cgroup using BTF-based reflection. Fields are specified
 * by name, validated against a whitelist, and returned in TLV format.
 */

#include <linux/kserial.h>
#include <linux/btf.h>
#include <linux/btf_ids.h>
#include <linux/cgroup.h>
#include <linux/errno.h>
#include <linux/string.h>

/**
 * ks_find_field_by_name - Find a struct member by name using BTF
 * @btf: BTF object
 * @type_id: BTF type ID of the struct
 * @field_name: Name of the field to find
 * @member: Output pointer to btf_member
 * 
 * Returns: 0 on success, -ENOENT if not found
 */
static int ks_find_field_by_name(const struct btf *btf, s32 type_id,
				  const char *field_name,
				  const struct btf_member **member)
{
	const struct btf_type *t;
	const struct btf_member *m;
	int i;

	t = btf_type_by_id(btf, type_id);
	if (!t || !btf_type_is_struct(t))
		return -EINVAL;

	for (i = 0; i < btf_vlen(t); i++) {
		m = btf_members(t) + i;
		const char *name = btf_name_by_offset(btf, m->name_off);
		
		if (name && !strcmp(name, field_name)) {
			*member = m;
			return 0;
		}
	}

	return -ENOENT;
}

/**
 * ks_get_field_size - Get size of a BTF type (MVP: only scalars)
 * @btf: BTF object
 * @type_id: BTF type ID
 * 
 * Returns: Size in bytes, or -EINVAL for unsupported types
 */
static int ks_get_field_size(const struct btf *btf, u32 type_id)
{
	const struct btf_type *t;

	t = btf_type_by_id(btf, type_id);
	if (!t)
		return -EINVAL;

	/* Follow modifiers (const, volatile, etc.) */
	while (btf_type_is_modifier(t)) {
		type_id = t->type;
		t = btf_type_by_id(btf, type_id);
		if (!t)
			return -EINVAL;
	}

	/* MVP: Only support integer types */
	if (btf_type_is_int(t)) {
		return t->size;
	}

	/* Reject pointers, arrays, etc. for MVP */
	return -EINVAL;
}

/**
 * ks_write_tlv - Write a TLV entry to output buffer
 * @result: Output buffer
 * @field_id: Field index in schema
 * @data: Field value
 * @len: Length of value
 * 
 * Returns: 0 on success, -ENOSPC if buffer full
 */
static int ks_write_tlv(struct ks_result *result, u16 field_id,
			const void *data, u16 len)
{
	struct ks_tlv *tlv;
	u32 tlv_size = sizeof(struct ks_tlv) + len;

	if (result->total_len + tlv_size > KS_MAX_OUTPUT_SIZE)
		return -ENOSPC;

	tlv = (struct ks_tlv *)(result->data + result->total_len);
	tlv->field_id = field_id;
	tlv->len = len;
	memcpy(tlv->data, data, len);

	result->total_len += tlv_size;
	return 0;
}

/**
 * ks_query_cgroup - Query fields from a cgroup using BTF
 * @cgrp: Target cgroup
 * @schema: User-provided field list
 * @result: Output buffer for TLV-encoded data
 * 
 * This function:
 * 1. Validates each field name against whitelist
 * 2. Uses BTF to find field by name in struct cgroup
 * 3. Extracts the value from the cgroup instance
 * 4. Encodes the result in TLV format
 * 
 * Returns: 0 on success, negative error code on failure
 */
int ks_query_cgroup(struct cgroup *cgrp, const struct ks_schema *schema,
		    struct ks_result *result)
{
	const struct btf *btf;
	s32 cgroup_type_id;
	u32 i;
	int ret;

	if (!cgrp || !schema || !result)
		return -EINVAL;

	if (schema->nr_fields == 0 || schema->nr_fields > KS_MAX_FIELDS)
		return -EINVAL;

	/* Initialize result buffer */
	result->total_len = 0;

	/* Get BTF for vmlinux */
	btf = btf_get_module_btf(NULL);
	if (!btf)
		return -ENOENT;

	/* Find struct cgroup in BTF */
	cgroup_type_id = btf_find_by_name_kind(btf, "cgroup", BTF_KIND_STRUCT);
	if (cgroup_type_id < 0)
		return cgroup_type_id;

	/* Process each requested field */
	for (i = 0; i < schema->nr_fields; i++) {
		const char *field_name = schema->field_names[i];
		const struct btf_member *member;
		void *field_addr;
		u64 value;
		int field_size;

		/* Validate field name against whitelist */
		if (!ks_validate_field(field_name)) {
			pr_warn("k-serial: field '%s' not in whitelist\n",
				field_name);
			return -EPERM;
		}

		/* Find field in BTF */
		ret = ks_find_field_by_name(btf, cgroup_type_id, field_name,
					     &member);
		if (ret) {
			pr_warn("k-serial: field '%s' not found in struct cgroup\n",
				field_name);
			return ret;
		}

		/* Get field size and validate type */
		field_size = ks_get_field_size(btf, member->type);
		if (field_size < 0) {
			pr_warn("k-serial: field '%s' has unsupported type\n",
				field_name);
			return field_size;
		}

		/* Calculate field address (assume byte-aligned for MVP) */
		field_addr = (void *)cgrp + (member->offset / 8);

		/* Read value (zero-extend for smaller types) */
		value = 0;
		memcpy(&value, field_addr, field_size);

		/* Write TLV entry */
		ret = ks_write_tlv(result, i, &value, field_size);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(ks_query_cgroup);

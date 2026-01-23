// SPDX-License-Identifier: GPL-2.0
/*
 * k-serial: Dynamic field subscription for kernel structs
 * Phase 2: Supports nested struct fields
 * 
 * This implementation allows userspace to query fields using paths like:
 * - Simple: "level"
 * - Nested: "self.id"
 * - Deep: "css_set.dfl_cgrp.level"
 * 
 * Fields are validated against a whitelist and returned in TLV format.
 */

#include <linux/kserial.h>
#include <linux/btf.h>
#include <linux/btf_ids.h>
#include <linux/cgroup.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>

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
 * ks_get_field_size - Get size of a BTF type
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

	/* Support integer types */
	if (btf_type_is_int(t)) {
		return t->size;
	}

	/* Phase 2: Support pointer types (size = sizeof(void*)) */
	if (btf_type_is_ptr(t)) {
		return sizeof(void *);
	}

	/* Reject arrays and other complex types */
	return -EINVAL;
}

/**
 * ks_parse_field_path - Parse field path into components
 * @path: Field path (e.g., "css_set.dfl_cgrp.level")
 * @components: Output array of path components
 * @max_depth: Maximum number of components
 * 
 * Returns: Number of components, or negative error code
 */
static int ks_parse_field_path(const char *path, char components[][32], int max_depth)
{
	const char *p = path;
	int depth = 0;
	int len;

	while (*p && depth < max_depth) {
		const char *dot = strchr(p, '.');
		
		if (dot) {
			len = dot - p;
			if (len >= 32)
				return -ENAMETOOLONG;
			strncpy(components[depth], p, len);
			components[depth][len] = '\0';
			p = dot + 1;
		} else {
			len = strlen(p);
			if (len >= 32)
				return -ENAMETOOLONG;
			strcpy(components[depth], p);
			break;
		}
		depth++;
	}

	if (*p)
		depth++;

	return depth;
}

/**
 * ks_resolve_field_path - Resolve nested field path using BTF
 * @btf: BTF object
 * @start_type_id: Starting struct type ID
 * @base_addr: Base address of starting struct
 * @path: Field path (e.g., "self.id")
 * @flags: Schema flags
 * @final_addr: Output pointer to final field address
 * @final_type_id: Output pointer to final field type ID
 * 
 * Returns: 0 on success, negative error code on failure
 */
static int ks_resolve_field_path(const struct btf *btf, s32 start_type_id,
				  void *base_addr, const char *path,
				  u32 flags, void **final_addr,
				  u32 *final_type_id)
{
	char components[KS_MAX_PATH_DEPTH][32];
	int depth, i;
	s32 current_type_id = start_type_id;
	void *current_addr = base_addr;

	/* Parse path into components */
	depth = ks_parse_field_path(path, components, KS_MAX_PATH_DEPTH);
	if (depth < 0)
		return depth;

	if (depth > KS_MAX_PATH_DEPTH)
		return -E2BIG;

	/* Traverse each component */
	for (i = 0; i < depth; i++) {
		const struct btf_type *t;
		const struct btf_member *m;
		u32 offset;
		int ret;

		/* Find field in current struct */
		ret = ks_find_field_by_name(btf, current_type_id,
					     components[i], &m);
		if (ret) {
			pr_warn("k-serial: field '%s' not found in path '%s'\n",
				components[i], path);
			return ret;
		}

		offset = m->offset / 8;
		current_addr = (char *)current_addr + offset;
		current_type_id = m->type;

		/* Get the actual type (follow modifiers) */
		t = btf_type_by_id(btf, current_type_id);
		if (!t)
			return -EINVAL;

		while (btf_type_is_modifier(t)) {
			current_type_id = t->type;
			t = btf_type_by_id(btf, current_type_id);
			if (!t)
				return -EINVAL;
		}

		/* If not the last component, it must be a pointer or struct */
		if (i < depth - 1) {
			if (btf_type_is_ptr(t)) {
				/* Dereference pointer */
				void *ptr = *(void **)current_addr;
				
				if (!ptr) {
					if (flags & KS_FLAG_ALLOW_NULL) {
						*final_addr = NULL;
						*final_type_id = 0;
						return 0;
					}
					pr_warn("k-serial: NULL pointer in path '%s' at '%s'\n",
						path, components[i]);
					return -EFAULT;
				}

				current_addr = ptr;
				
				/* Get the pointed-to type */
				current_type_id = t->type;
				t = btf_type_by_id(btf, current_type_id);
				if (!t)
					return -EINVAL;

				/* Follow modifiers again */
				while (btf_type_is_modifier(t)) {
					current_type_id = t->type;
					t = btf_type_by_id(btf, current_type_id);
					if (!t)
						return -EINVAL;
				}

				/* Must be a struct after dereferencing */
				if (!btf_type_is_struct(t))
					return -EINVAL;
			} else if (btf_type_is_struct(t)) {
				/* Embedded struct, continue with current address */
				continue;
			} else {
				pr_warn("k-serial: invalid intermediate type in path '%s'\n",
					path);
				return -EINVAL;
			}
		}
	}

	*final_addr = current_addr;
	*final_type_id = current_type_id;
	return 0;
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
 * @schema: User-provided field list (may contain nested paths)
 * @result: Output buffer for TLV-encoded data
 * 
 * This function supports both simple fields and nested paths:
 * - Simple: "level" 
 * - Nested: "self.id"
 * - Deep: "dom_cgrp.level" (with pointer dereferencing)
 * 
 * Steps:
 * 1. Validates each field path against whitelist
 * 2. Uses BTF to resolve the path (handles nesting and pointers)
 * 3. Extracts the value from the final field
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

	/* Process each requested field/path */
	for (i = 0; i < schema->nr_fields; i++) {
		const char *field_path = schema->field_names[i];
		void *field_addr;
		u32 field_type_id;
		u64 value;
		int field_size;

		/* Validate field path against whitelist */
		if (!ks_validate_field(field_path)) {
			pr_warn("k-serial: field '%s' not in whitelist\n",
				field_path);
			return -EPERM;
		}

		/* Resolve field path (handles nesting and pointers) */
		ret = ks_resolve_field_path(btf, cgroup_type_id, cgrp,
					     field_path, schema->flags,
					     &field_addr, &field_type_id);
		if (ret) {
			pr_warn("k-serial: failed to resolve path '%s'\n",
				field_path);
			return ret;
		}

		/* Handle NULL pointer case (if flag set) */
		if (!field_addr) {
			value = 0;
			field_size = sizeof(u64);
			ret = ks_write_tlv(result, i, &value, field_size);
			if (ret)
				return ret;
			continue;
		}

		/* Get field size and validate type */
		field_size = ks_get_field_size(btf, field_type_id);
		if (field_size < 0) {
			pr_warn("k-serial: field '%s' has unsupported type\n",
				field_path);
			return field_size;
		}

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

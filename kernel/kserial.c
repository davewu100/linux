// SPDX-License-Identifier: GPL-2.0
/*
 * kserial: BTF-based field query for kernel structs
 *
 * Used by memory.stat.ks and memory.numa_stat.ks to query mem_cgroup
 * fields by name via BTF, allowing selective reads with better performance.
 */

#include <linux/kserial.h>
#include <linux/btf.h>
#include <linux/btf_ids.h>
#include <linux/bpf.h>
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
	u32 resolved_id;

	/* Skip modifiers (const, volatile, etc.) */
	t = btf_type_skip_modifiers(btf, type_id, &resolved_id);
	if (!t)
		return -EINVAL;

	/* Support integer types */
	if (btf_type_is_int(t)) {
		return t->size;
	}

	/* Phase 2: Support pointer types (size = sizeof(void*)) */
	if (btf_type_is_ptr(t)) {
		return sizeof(void *);
	}

	/* Phase 3: For arrays, return element size */
	if (btf_type_is_array(t)) {
		const struct btf_array *arr = btf_array(t);
		return ks_get_field_size(btf, arr->type);
	}

	/* Reject other complex types */
	return -EINVAL;
}

/**
 * ks_parse_array_syntax - Parse array syntax from field name
 * @field: Field name (e.g., "subsys[0]" or "level")
 * @base_name: Output buffer for base field name
 * @index: Output pointer for array index (-1 if not an array)
 *
 * Returns: 0 on success, negative error code on failure
 */
static int ks_parse_array_syntax(const char *field, char *base_name, int *index)
{
	const char *bracket = strchr(field, '[');
	int len;
	int digit_count = 0;

	if (!bracket) {
		/* No array syntax */
		strcpy(base_name, field);
		*index = -1;
		return 0;
	}

	/* Extract base name */
	len = bracket - field;
	if (len >= 32 || len == 0)
		return -EINVAL;

	strncpy(base_name, field, len);
	base_name[len] = '\0';

	/* Parse index */
	bracket++; /* Skip '[' */
	if (*bracket == '\0' || *bracket == ']')
		return -EINVAL;

	*index = 0;
	while (*bracket >= '0' && *bracket <= '9') {
		int digit = *bracket - '0';

		/* Prevent integer overflow: check before multiplication
		 * Max safe value: INT_MAX / 10 = 214748364
		 * If index > INT_MAX/10, next multiplication will overflow
		 * If index == INT_MAX/10, check if digit would cause overflow
		 */
		if (*index > INT_MAX / 10 ||
		    (*index == INT_MAX / 10 && digit > INT_MAX % 10)) {
			pr_warn("k-serial: array index too large in '%s'\n", field);
			return -ERANGE;
		}

		*index = (*index) * 10 + digit;
		bracket++;
		digit_count++;

		/* Sanity check: reject unreasonably long numbers (>10 digits)
		 * This prevents DoS by parsing extremely long number strings
		 */
		if (digit_count > 10) {
			pr_warn("k-serial: array index too many digits in '%s'\n", field);
			return -EINVAL;
		}
	}

	if (*bracket != ']')
		return -EINVAL;

	/* Additional sanity check: reject negative results from overflow */
	if (*index < 0) {
		pr_warn("k-serial: array index invalid in '%s'\n", field);
		return -ERANGE;
	}

	return 0;
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

		/* Get the struct type to calculate proper offset */
		t = btf_type_by_id(btf, current_type_id);
		if (!t)
			return -EINVAL;

		/* Calculate byte offset using proper BTF API */
		offset = __btf_member_bit_offset(t, m) / 8;
		pr_debug("k-serial: field '%s' offset=%u bytes (bit_offset=%u)\n",
			 components[i], offset, __btf_member_bit_offset(t, m));
		current_addr = (char *)current_addr + offset;
		current_type_id = m->type;

		/* Get the actual type (follow modifiers) */
		t = btf_type_skip_modifiers(btf, current_type_id, &current_type_id);
		if (!t)
			return -EINVAL;

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

				/* Get the pointed-to type and skip modifiers */
				current_type_id = t->type;
				t = btf_type_skip_modifiers(btf, current_type_id, &current_type_id);
				if (!t)
					return -EINVAL;

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
 * ks_resolve_array_element - Resolve array element address and type
 * @btf: BTF object
 * @array_type_id: BTF type ID of the array field
 * @array_addr: Base address of the array
 * @index: Array index
 * @elem_addr: Output pointer to element address
 * @elem_type_id: Output pointer to element type ID
 *
 * Returns: 0 on success, negative error code on failure
 */
static int ks_resolve_array_element(const struct btf *btf, u32 array_type_id,
				     void *array_addr, int index,
				     void **elem_addr, u32 *elem_type_id)
{
	const struct btf_type *t;
	const struct btf_array *arr;
	u32 elem_size;
	int ret;

	/* Skip modifiers */
	t = btf_type_skip_modifiers(btf, array_type_id, &array_type_id);
	if (!t)
		return -EINVAL;

	/* Must be an array type */
	if (!btf_type_is_array(t)) {
		pr_warn("k-serial: field is not an array\n");
		return -EINVAL;
	}

	arr = btf_array(t);

	/* Bounds check */
	if (index < 0 || (u32)index >= arr->nelems) {
		pr_warn("k-serial: array index %d out of bounds (0-%u)\n",
			index, arr->nelems - 1);
		return -ERANGE;
	}

	/* Get element size */
	ret = ks_get_field_size(btf, arr->type);
	if (ret < 0)
		return ret;
	elem_size = ret;

	/* Prevent address calculation overflow
	 * Check if (index * elem_size) would overflow before calculating
	 */
	if (elem_size > 0 && index > (INT_MAX / elem_size)) {
		pr_warn("k-serial: array offset calculation would overflow\n");
		return -ERANGE;
	}

	/* Calculate element address (now safe from overflow) */
	*elem_addr = (char *)array_addr + (index * elem_size);
	*elem_type_id = arr->type;

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
 * ks_query_struct - Query fields from any kernel struct using BTF
 * @struct_addr: Address of target struct
 * @struct_name: Name of struct type (e.g., "cgroup", "mem_cgroup", "task_struct")
 * @schema: User-provided field list (may contain nested paths)
 * @result: Output buffer for TLV-encoded data
 *
 * This function supports querying any BTF-visible kernel struct:
 * - Simple fields: "level", "nr_descendants"
 * - Nested fields: "self.id", "dom_cgrp.level"
 * - Array elements: "subsys[0]", "nr_dying_subsys[1]"
 *
 * Security:
 * - BTF type checking ensures field exists
 * - Only scalar types (int, u64, pointer) are returned
 * - Array bounds checked automatically
 *
 * Returns: 0 on success, negative error code on failure
 */
int ks_query_struct(void *struct_addr, const char *struct_name,
		    const struct ks_schema *schema, struct ks_result *result)
{
	const struct btf *btf;
	s32 struct_type_id;
	u32 i;
	int ret;

	if (!struct_addr || !struct_name || !schema || !result)
		return -EINVAL;

	if (schema->nr_fields == 0 || schema->nr_fields > KS_MAX_FIELDS)
		return -EINVAL;

	/* Initialize result buffer */
	result->total_len = 0;

	/* Get BTF for vmlinux */
	/* Use bpf_get_btf_vmlinux() instead of direct access to btf_vmlinux */
	extern struct btf *bpf_get_btf_vmlinux(void);
	btf = bpf_get_btf_vmlinux();
	if (IS_ERR(btf) || !btf)
		return -ENOENT;

	/* Find target struct in BTF */
	struct_type_id = btf_find_by_name_kind(btf, struct_name, BTF_KIND_STRUCT);
	if (struct_type_id < 0) {
		pr_warn("k-serial: struct '%s' not found in BTF\n", struct_name);
		return struct_type_id;
	}

	/* Process each requested field/path */
	for (i = 0; i < schema->nr_fields; i++) {
		const char *field_path = schema->field_names[i];
		char base_name[64];
		int array_index;
		void *field_addr;
		u32 field_type_id;
		u32 field_offset = 0;
		u64 value;
		int field_size;
		struct ks_cache_entry *cache_entry = NULL;

		/* Parse array syntax (e.g., "vmstats.state[14]") */
		ret = ks_parse_array_syntax(field_path, base_name, &array_index);
		if (ret) {
			pr_warn("kserial: invalid array syntax in '%s'\n",
				field_path);
			return ret;
		}

		/* Try cache first (non-array paths only) for performance */
		if (array_index < 0) {
			cache_entry = ks_cache_lookup(struct_name, base_name);
			if (cache_entry) {
				field_offset = cache_entry->offset;
				field_type_id = cache_entry->type_id;
				field_size = cache_entry->size;
				field_addr = (char *)struct_addr + field_offset;
				goto read_value;
			}
		}

		/* Cache miss or array: resolve field path via BTF */
		ret = ks_resolve_field_path(btf, struct_type_id, struct_addr,
					     base_name, schema->flags,
					     &field_addr, &field_type_id);
		if (ret) {
			pr_warn("kserial: failed to resolve path '%s': %d\n",
				base_name, ret);
			continue;
		}
		field_offset = (u32)((char *)field_addr - (char *)struct_addr);

		/* If array access, resolve the element */
		if (array_index >= 0) {
			ret = ks_resolve_array_element(btf, field_type_id,
						        field_addr, array_index,
						        &field_addr, &field_type_id);
			if (ret) {
				pr_warn("kserial: failed to access array element '%s'\n",
					field_path);
				return ret;
			}
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
			pr_warn("kserial: field '%s' has unsupported type\n",
				field_path);
			continue;
		}

		/* Cache resolved field for next time (non-array only) */
		if (array_index < 0 && !cache_entry)
			ks_cache_insert(struct_name, base_name, field_offset,
					field_size, field_type_id, 0);

read_value:
		/* Read value (zero-extend for smaller types, sign-extend for signed types) */
		value = 0;
		if (field_size > sizeof(value)) {
			pr_warn("kserial: field '%s' size %d too large\n",
				field_path, field_size);
			continue;
		}
		memcpy(&value, field_addr, field_size);

		/* Sign-extend if the field is a signed integer type */
		{
			const struct btf_type *t = btf_type_by_id(btf, field_type_id);
			if (t && btf_type_is_int(t)) {
				u8 encoding = btf_int_encoding(t);
				if (encoding & BTF_INT_SIGNED) {
					/* Sign-extend based on field size */
					if (field_size == 1 && (value & 0x80))
						value |= 0xFFFFFFFFFFFFFF00ULL;
					else if (field_size == 2 && (value & 0x8000))
						value |= 0xFFFFFFFFFFFF0000ULL;
					else if (field_size == 4 && (value & 0x80000000))
						value |= 0xFFFFFFFF00000000ULL;
				}
			}
		}

		/* Write TLV entry */
		ret = ks_write_tlv(result, i, &value, field_size);
		if (ret)
			return ret;
	}

	return 0;
}

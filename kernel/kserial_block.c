// SPDX-License-Identifier: GPL-2.0
/*
 * kserial_block.c - Block read implementation for k-serial
 * 
 * Supports reading contiguous memory blocks:
 * - Array ranges: field[start..end]
 * - Entire arrays: field[*]
 * - Raw offsets: @offset,size
 */

#include <linux/kserial.h>
#include <linux/btf.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/slab.h>

#define KS_MAX_BLOCK_SIZE (KS_MAX_OUTPUT_SIZE - sizeof(struct ks_tlv))

/**
 * ks_parse_array_range - Parse array range syntax like "state[0..49]"
 * @field_name: Field name with potential range
 * @base_name: Output buffer for base field name
 * @start: Output for start index
 * @count: Output for element count
 * 
 * Returns: 0 on success, -EINVAL if not a valid range
 * 
 * Supported formats:
 *   "state[0..49]"  -> base="state", start=0, count=50
 *   "state[*]"      -> base="state", start=0, count=-1 (special: all)
 *   "state[5]"      -> base="state", start=5, count=1  (single element)
 *   "state"         -> not a range, return -EINVAL
 */
static int ks_parse_array_range(const char *field_name, char *base_name,
				 u32 *start, u32 *count)
{
	const char *bracket = strchr(field_name, '[');
	const char *dots, *close;
	u32 end_idx;
	
	if (!bracket)
		return -EINVAL;  /* Not an array syntax */
	
	/* Extract base name */
	size_t base_len = bracket - field_name;
	if (base_len >= KS_FIELD_NAME_LEN)
		return -EINVAL;
	memcpy(base_name, field_name, base_len);
	base_name[base_len] = '\0';
	
	bracket++;  /* Skip '[' */
	
	/* Check for wildcard: field[*] */
	if (*bracket == '*') {
		*start = 0;
		*count = (u32)-1;  /* Special value: read all */
		return 0;
	}
	
	/* Check for range: field[start..end] */
	dots = strstr(bracket, "..");
	if (dots) {
		/* Parse start index */
		if (kstrtou32(bracket, 10, start) < 0)
			return -EINVAL;
		
		/* Parse end index */
		if (kstrtou32(dots + 2, 10, &end_idx) < 0)
			return -EINVAL;
		
		if (end_idx < *start)
			return -EINVAL;
		
		*count = end_idx - *start + 1;
		return 0;
	}
	
	/* Single element: field[N] */
	close = strchr(bracket, ']');
	if (!close)
		return -EINVAL;
	
	if (kstrtou32(bracket, 10, start) < 0)
		return -EINVAL;
	
	*count = 1;
	return 0;
}

/**
 * ks_query_block - Read a contiguous block of array elements
 * @struct_addr: Base address of struct
 * @struct_name: Struct type name
 * @schema: Contains field name with range syntax
 * @result: Output buffer
 * 
 * This function handles array range reads like:
 *   vmstats.state[0..49]  - Read 50 elements
 *   vmstats.state[*]      - Read entire array
 *   vmstats.state[5]      - Read single element (legacy compat)
 */
/* Forward declaration */
static int ks_find_field_by_name(const struct btf *btf, s32 type_id,
				 const char *field_name,
				 const struct btf_member **member_out);

int ks_query_block(void *struct_addr, const char *struct_name,
		   const struct ks_schema *schema,
		   struct ks_result *result)
{
	extern struct btf *btf_vmlinux;
	const struct btf *btf = btf_vmlinux;
	const struct btf_type *t, *struct_type;
	const struct btf_member *m;
	const struct btf_array *arr;
	char base_field[KS_FIELD_NAME_LEN];
	u32 struct_id, field_type_id, elem_type_id;
	u32 offset, elem_size, array_nelems;
	u32 start_idx, read_count;
	size_t total_size;
	void *field_addr;
	struct ks_tlv *tlv;
	int ret;
	
	if (!btf || !struct_addr || !struct_name || !schema || !result)
		return -EINVAL;
	
	/* Find struct type */
	struct_id = btf_find_by_name_kind(btf, struct_name, BTF_KIND_STRUCT);
	if (struct_id <= 0) {
		pr_warn("k-serial: struct '%s' not found in BTF\n", struct_name);
		return -ENOENT;
	}
	
	struct_type = btf_type_by_id(btf, struct_id);
	
	/* Parse array range from field name */
	ret = ks_parse_array_range(schema->field_names[0], base_field,
				    &start_idx, &read_count);
	if (ret < 0) {
		/* Fallback: use explicit array_start/array_count */
		strncpy(base_field, schema->field_names[0], KS_FIELD_NAME_LEN - 1);
		start_idx = schema->array_start;
		read_count = schema->array_count;
		if (read_count == 0)
			return -EINVAL;
	}
	
	/* Find the field in struct */
	ret = ks_find_field_by_name(btf, struct_id, base_field, &m);
	if (ret < 0) {
		pr_warn("k-serial: field '%s' not found in struct '%s'\n",
			base_field, struct_name);
		return ret;
	}
	
	/* Calculate member offset */
	{
		const struct btf_member *members = btf_type_member(struct_type);
		u32 member_idx = m - members;
		offset = btf_member_bit_offset(struct_type, member_idx) / 8;
	}
	field_type_id = m->type;
	
	/* Resolve to array type */
	t = btf_type_skip_modifiers(btf, field_type_id, &field_type_id);
	if (!t || !btf_type_is_array(t)) {
		pr_warn("k-serial: field '%s' is not an array\n", base_field);
		return -EINVAL;
	}
	
	arr = btf_array(t);
	elem_type_id = arr->type;
	array_nelems = arr->nelems;
	
	/* Get element size */
	t = btf_type_by_id(btf, elem_type_id);
	if (!t) {
		return -EINVAL;
	}
	elem_size = t->size;
	
	/* Handle wildcard: read entire array */
	if (read_count == (u32)-1) {
		read_count = array_nelems;
	}
	
	/* Bounds check */
	if (start_idx >= array_nelems) {
		pr_warn("k-serial: array index %u out of bounds (size %u)\n",
			start_idx, array_nelems);
		return -ERANGE;
	}
	
	if (start_idx + read_count > array_nelems) {
		pr_warn("k-serial: array range [%u..%u] exceeds size %u\n",
			start_idx, start_idx + read_count - 1, array_nelems);
		return -ERANGE;
	}
	
	/* Calculate total size */
	total_size = elem_size * read_count;
	
	/* Size check */
	if (total_size > KS_MAX_BLOCK_SIZE) {
		pr_warn("k-serial: block size %zu exceeds limit %zu\n",
			total_size, (size_t)KS_MAX_BLOCK_SIZE);
		return -E2BIG;
	}
	
	/* Calculate field address */
	field_addr = struct_addr + offset + (start_idx * elem_size);
	
	/* Safety check: ensure address is valid */
	if (!virt_addr_valid(field_addr)) {
		pr_warn("k-serial: invalid address for block read\n");
		return -EFAULT;
	}
	
	/* Construct TLV output */
	tlv = (struct ks_tlv *)result->data;
	tlv->field_id = 0;
	tlv->len = total_size;
	
	/* Copy block data */
	memcpy(tlv->data, field_addr, total_size);
	
	result->total_len = sizeof(*tlv) + total_size;
	
	pr_debug("k-serial: block read %s[%u..%u] = %zu bytes\n",
		 base_field, start_idx, start_idx + read_count - 1, total_size);
	
	return 0;
}

/* Helper function to find field by name (forward declaration from kserial.c) */
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

	m = btf_type_member(t);
	for (i = 0; i < btf_type_vlen(t); i++, m++) {
		const char *name = btf_name_by_offset(btf, m->name_off);
		
		if (name && !strcmp(name, field_name)) {
			*member = m;
			return 0;
		}
	}

	return -ENOENT;
}

EXPORT_SYMBOL_GPL(ks_query_block);

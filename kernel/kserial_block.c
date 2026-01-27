// SPDX-License-Identifier: GPL-2.0
/*
 * k-serial block read implementation
 * 
 * Provides block read mode for reading array ranges or raw memory blocks
 */

#include <linux/kserial.h>
#include <linux/btf.h>
#include <linux/bpf.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>

/**
 * ks_query_block - Block read mode for array ranges or raw memory
 * @struct_addr: Address of target struct
 * @struct_name: Name of struct type
 * @schema: User-provided schema with block read parameters
 * @result: Output buffer for TLV-encoded data
 * 
 * Returns: 0 on success, negative error code on failure
 */
int ks_query_block(void *struct_addr, const char *struct_name,
		   const struct ks_schema *schema, struct ks_result *result)
{
	const struct btf *btf;
	s32 struct_type_id;
	u32 i;
	void *read_addr;
	size_t read_size;
	u32 offset = 0;

	if (!struct_addr || !struct_name || !schema || !result)
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

	/* Handle raw offset mode */
	if (schema->flags & KS_FLAG_RAW_OFFSET) {
		if (schema->block_size == 0 || schema->block_size > KS_MAX_OUTPUT_SIZE)
			return -EINVAL;

		read_addr = (char *)struct_addr + schema->block_offset;
		read_size = schema->block_size;

		/* Write TLV entry */
		if (offset + sizeof(struct ks_tlv) + read_size > KS_MAX_OUTPUT_SIZE)
			return -ENOSPC;

		{
			struct ks_tlv *tlv = (struct ks_tlv *)(result->data + offset);
			tlv->field_id = 0;
			tlv->len = read_size;
			memcpy(tlv->data, read_addr, read_size);
			offset += sizeof(*tlv) + read_size;
		}

		result->total_len = offset;
		return 0;
	}

	/* Handle array range read mode */
	if (schema->nr_fields == 0)
		return -EINVAL;

	/* Process each field */
	for (i = 0; i < schema->nr_fields; i++) {
		const char *field_name = schema->field_names[i];
		const struct btf_member *member = NULL;
		const struct btf_type *t;
		u32 field_type_id;
		size_t field_size;
		u32 array_start, array_count;
		u32 j;
		int found = 0;

		/* Find field in struct using BTF */
		t = btf_type_by_id(btf, struct_type_id);
		if (!t || !btf_type_is_struct(t))
			continue;

		for (j = 0; j < btf_vlen(t); j++) {
			const struct btf_member *m = btf_members(t) + j;
			const char *name = btf_name_by_offset(btf, m->name_off);
			
			if (name && !strcmp(name, field_name)) {
				member = m;
				found = 1;
				break;
			}
		}

		if (!found) {
			pr_warn("k-serial: field '%s' not found in struct '%s'\n",
				field_name, struct_name);
			continue;
		}

		field_type_id = member->type;
		t = btf_type_by_id(btf, field_type_id);
		if (!t)
			continue;

		/* Skip modifiers */
		t = btf_type_skip_modifiers(btf, field_type_id, &field_type_id);
		if (!t)
			continue;

		/* Check if it's an array */
		if (!btf_type_is_array(t)) {
			pr_warn("k-serial: field '%s' is not an array\n", field_name);
			continue;
		}

		/* Get array element type and size */
		{
			const struct btf_array *arr = btf_array(t);
			const struct btf_type *elem_t;
			u32 elem_type_id = arr->type;

			elem_t = btf_type_skip_modifiers(btf, elem_type_id, &elem_type_id);
			if (!elem_t)
				continue;

			/* Get element size from BTF type */
			field_size = elem_t->size;
			if (field_size == 0) {
				/* For pointer types, use pointer size */
				if (btf_type_is_ptr(elem_t))
					field_size = sizeof(void *);
				else
					continue;
			}
		}

		/* Get array range parameters */
		array_start = (i == 0 && schema->array_start > 0) ? schema->array_start : 0;
		array_count = (i == 0 && schema->array_count > 0) ? schema->array_count : 1;

		/* Calculate field offset */
		{
			u32 field_offset = member->offset / 8;
			void *array_base = (char *)struct_addr + field_offset;

			/* Read array elements */
			for (j = 0; j < array_count; j++) {
				void *elem_addr = (char *)array_base + (array_start + j) * field_size;
				size_t elem_size = field_size;

				/* Write TLV entry */
				if (offset + sizeof(struct ks_tlv) + elem_size > KS_MAX_OUTPUT_SIZE) {
					pr_warn("k-serial: output buffer full\n");
					break;
				}

				{
					struct ks_tlv *tlv = (struct ks_tlv *)(result->data + offset);
					tlv->field_id = i;
					tlv->len = elem_size;
					memcpy(tlv->data, elem_addr, elem_size);
					offset += sizeof(*tlv) + elem_size;
				}
			}
		}
	}

	result->total_len = offset;
	return 0;
}

/* Export for use by kserial_procfs module */
EXPORT_SYMBOL_GPL(ks_query_block);

/* Module metadata - only when built as module */
#ifdef MODULE
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jianyue Wu <wujianyue000@gmail.com>");
MODULE_DESCRIPTION("k-serial block read implementation");
#endif

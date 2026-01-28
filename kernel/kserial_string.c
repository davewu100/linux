// SPDX-License-Identifier: GPL-2.0
/*
 * kserial_string.c - String field support for k-serial
 */

#include <linux/kserial.h>
#include <linux/btf.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/module.h>

#define KS_MAX_STRING_LEN 256

/* Check if BTF type is char */
static bool ks_is_char_type(const struct btf *btf, u32 type_id)
{
	const struct btf_type *t;
	const char *name;

	t = btf_type_skip_modifiers(btf, type_id, &type_id);
	if (!t || !btf_type_is_int(t))
		return false;

	name = btf_name_by_offset(btf, t->name_off);
	return name && (!strcmp(name, "char") ||
			!strcmp(name, "signed char") ||
	                !strcmp(name, "unsigned char"));
}

/* Check if field is a string */
bool ks_is_string_field(const struct btf *btf, u32 field_type_id)
{
	const struct btf_type *t;
	const struct btf_array *arr;
	u32 elem_type_id;

	t = btf_type_skip_modifiers(btf, field_type_id, &field_type_id);
	if (!t)
		return false;

	/* Case 1: char array */
	if (btf_type_is_array(t)) {
		arr = btf_array(t);
		elem_type_id = arr->type;
		return ks_is_char_type(btf, elem_type_id);
	}

	/* Case 2: char pointer */
	if (btf_type_is_ptr(t)) {
		elem_type_id = t->type;
		return ks_is_char_type(btf, elem_type_id);
	}

	return false;
}

/* Query a string field */
int ks_query_string_field(const struct btf *btf, void *field_addr,
			   u32 field_type_id, char *out_buf, u32 buf_size)
{
	const struct btf_type *t;
	const struct btf_array *arr;
	u32 array_len, copy_len;
	const char *ptr;

	if (!btf || !field_addr || !out_buf || buf_size == 0)
		return -EINVAL;

	t = btf_type_skip_modifiers(btf, field_type_id, &field_type_id);
	if (!t)
		return -EINVAL;

	/* Case 1: char array */
	if (btf_type_is_array(t)) {
		arr = btf_array(t);
		array_len = arr->nelems;

		copy_len = strnlen(field_addr, min(array_len, buf_size - 1));
		memcpy(out_buf, field_addr, copy_len);
		out_buf[copy_len] = '\0';

		return copy_len + 1;
	}

	/* Case 2: char pointer */
	if (btf_type_is_ptr(t)) {
		ptr = *(const char **)field_addr;

		if (!ptr || !virt_addr_valid(ptr))
			return -EFAULT;

		copy_len = strnlen(ptr, min((u32)KS_MAX_STRING_LEN, buf_size - 1));
		memcpy(out_buf, ptr, copy_len);
		out_buf[copy_len] = '\0';

		return copy_len + 1;
	}

	return -EINVAL;
}

EXPORT_SYMBOL_GPL(ks_is_string_field);
EXPORT_SYMBOL_GPL(ks_query_string_field);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jianyue Wu <wujianyue000@gmail.com>");
MODULE_DESCRIPTION("k-serial: String field support for char arrays and pointers");

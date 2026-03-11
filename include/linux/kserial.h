/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_KSERIAL_H
#define _LINUX_KSERIAL_H

#include <linux/types.h>

#define KS_MAX_FIELDS 128
#define KS_FIELD_NAME_LEN 64

struct ks_schema {
	u32 nr_fields;
	u32 flags;
	char struct_name[KS_FIELD_NAME_LEN];
	char field_names[KS_MAX_FIELDS][KS_FIELD_NAME_LEN];
};

/**
 * kserial_btf_resolve - resolve struct field path to offset and size via BTF
 * @struct_name: BTF struct name (e.g. "mem_cgroup")
 * @field_path: dot-separated path, optional array index (e.g. "vmstats.state[17]")
 * @out_offset: output byte offset of the field in the struct
 * @out_size: output size in bytes
 *
 * Uses vmlinux BTF; result is cached. Return 0 on success, negative on error.
 */
int kserial_btf_resolve(const char *struct_name, const char *field_path,
			u32 *out_offset, u32 *out_size);

/**
 * kserial_read_field - read one field from base + offset as u64
 * @base: pointer to the struct (e.g. struct mem_cgroup *)
 * @offset: byte offset from kserial_btf_resolve
 * @size: size in bytes (1, 2, 4, or 8)
 *
 * Return: field value zero-extended to u64, or 0 if size is not 1/2/4/8.
 */
u64 kserial_read_field(void *base, u32 offset, u32 size);

#endif /* _LINUX_KSERIAL_H */

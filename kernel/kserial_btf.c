// SPDX-License-Identifier: GPL-2.0
/*
 * kserial_btf: BTF-based struct field path resolution (memcg-agnostic).
 * Resolves (struct_name, field_path) -> (offset, size) with caching.
 */

#include <linux/btf.h>
#include <linux/bpf.h>
#include <linux/kserial.h>
#include <linux/slab.h>
#include <linux/rhashtable.h>
#include <linux/errno.h>
#include <linux/string.h>

#define KSERIAL_BTF_PATH_MAX 256
#define KSERIAL_CACHE_SIZE 256

struct kserial_btf_entry {
	struct rhash_head node;
	char struct_name[KS_FIELD_NAME_LEN];
	char field_path[KSERIAL_BTF_PATH_MAX];
	u32 offset;
	u32 size;
};

static struct rhashtable_params kserial_btf_params = {
	.key_len = KS_FIELD_NAME_LEN + KSERIAL_BTF_PATH_MAX,
	.key_offset = offsetof(struct kserial_btf_entry, struct_name),
	.head_offset = offsetof(struct kserial_btf_entry, node),
	.automatic_shrinking = true,
};

static DEFINE_MUTEX(kserial_btf_cache_mutex);

static struct rhashtable kserial_btf_cache;
static bool kserial_btf_cache_ready;

/* Cache for "mem_cgroup" BTF so we only do O(n) bpf_find_btf_id once per boot. */
#define KSERIAL_MEMCG_STRUCT_NAME "mem_cgroup"
static struct btf *kserial_mem_cgroup_btf;
static u32 kserial_mem_cgroup_type_id;
static bool kserial_mem_cgroup_cached;

static u32 member_bit_offset(const struct btf_type *t, const struct btf_member *m)
{
	return btf_type_kflag(t) ? BTF_MEMBER_BIT_OFFSET(m->offset) : m->offset;
}

/* Parse one path segment: "name" or "name[index]". Return 0 and set *name_end, *idx (or -1). */
static int parse_segment(const char *seg, const char **name_end, int *idx)
{
	const char *p;
	char *end;

	*idx = -1;
	for (p = seg; *p && *p != '['; p++)
		;
	*name_end = p;
	if (*p == '[') {
		p++;
		*idx = (int)simple_strtol(p, &end, 10);
		if (end == p || *end != ']')
			return -EINVAL;
	}
	return 0;
}

/* Resolve one path segment (e.g. "state[17]") in a struct; update *type_id, *offset_bits, *size. */
static int resolve_path_segment(const struct btf *btf, u32 *type_id,
				const char *seg_buf, u32 *offset_bits, u32 *size)
{
	const struct btf_type *t, *mt;
	const struct btf_member *member;
	const char *name_end, *mem_name;
	u32 mem_off_bits, elem_size;
	const struct btf_array *arr;
	u32 id = *type_id;
	u32 elem_type_id;
	int i, idx, err;

	err = parse_segment(seg_buf, &name_end, &idx);
	if (err)
		return err;

	t = btf_type_skip_modifiers(btf, id, &id);
	if (!t || !btf_type_is_struct(t))
		return -EINVAL;

	for_each_member(i, t, member) {
		mem_name = btf_name_by_offset(btf, member->name_off);
		if (!mem_name || strncmp(mem_name, seg_buf, name_end - seg_buf) != 0)
			continue;
		if (mem_name[name_end - seg_buf] != '\0')
			continue;

		mem_off_bits = member_bit_offset(t, member);
		if (mem_off_bits % 8)
			return -EINVAL;
		*offset_bits += mem_off_bits;

		id = member->type;
		mt = btf_type_skip_modifiers(btf, id, &id);
		if (!mt)
			return -EINVAL;

		if (idx >= 0) {
			if (!btf_type_is_array(mt))
				return -EINVAL;
			arr = btf_array(mt);
			elem_type_id = arr->type;
			if ((u32)idx >= arr->nelems)
				return -EINVAL;
			if (!btf_type_id_size(btf, &elem_type_id, &elem_size) || !elem_size)
				return -EINVAL;
			*offset_bits += (u32)idx * elem_size * 8;
			*size = elem_size;
			id = arr->type;
			btf_type_skip_modifiers(btf, id, &id);
		} else {
			if (!btf_type_id_size(btf, &id, size))
				*size = 0;
		}
		*type_id = id;
		return 0;
	}
	return -ENOENT;
}

/* Resolve path (e.g. "vmstats.state[17]") from struct type_id. *out_offset in bytes, *out_size in bytes. */
static int resolve_path(const struct btf *btf, u32 type_id, const char *path,
			u32 *out_offset, u32 *out_size)
{
	u32 offset_bits = 0, size = 0;
	u32 id = type_id;
	char seg_buf[KS_FIELD_NAME_LEN];
	const char *p = path;

	*out_offset = 0;
	*out_size = 0;

	while (*p) {
		const char *dot = strchrnul(p, '.');
		size_t seg_len = dot - p;

		if (seg_len >= sizeof(seg_buf))
			return -EINVAL;
		memcpy(seg_buf, p, seg_len);
		seg_buf[seg_len] = '\0';

		if (resolve_path_segment(btf, &id, seg_buf, &offset_bits, &size))
			return -ENOENT;

		if (!*dot)
			break;
		p = dot + 1;
	}

	*out_offset = offset_bits / 8;
	*out_size = size ? size : 8;
	return 0;
}

u64 kserial_read_field(void *base, u32 offset, u32 size)
{
	u64 val = 0;

	if (size == 0 || size > sizeof(val)) {
		WARN_ONCE(1, "kserial_read_field: invalid size %u\n", size);
		return 0;
	}
	memcpy(&val, (char *)base + offset, size);
	return val;
}

int kserial_btf_resolve(const char *struct_name, const char *field_path,
			u32 *out_offset, u32 *out_size)
{
	struct btf *btf = NULL;
	s32 type_id;
	struct kserial_btf_entry *e;
	struct kserial_btf_entry key;
	int ret;

	if (!out_offset || !out_size)
		return -EINVAL;

	/* Cache key is (struct_name, field_path); lookup uses full key. */
	memset(&key.struct_name, 0, sizeof(key.struct_name));
	memset(&key.field_path, 0, sizeof(key.field_path));
	strscpy(key.struct_name, struct_name, KS_FIELD_NAME_LEN);
	strscpy(key.field_path, field_path, KSERIAL_BTF_PATH_MAX);

	mutex_lock(&kserial_btf_cache_mutex);
	if (kserial_btf_cache_ready) {
		e = rhashtable_lookup_fast(&kserial_btf_cache, &key.struct_name, kserial_btf_params);
		if (e) {
			*out_offset = e->offset;
			*out_size = e->size;
			mutex_unlock(&kserial_btf_cache_mutex);
			return 0;
		}
	}
	mutex_unlock(&kserial_btf_cache_mutex);

	/* For "mem_cgroup", use cached BTF+type_id so we do O(n) vmlinux scan only once. */
	if (strcmp(struct_name, KSERIAL_MEMCG_STRUCT_NAME) == 0) {
		mutex_lock(&kserial_btf_cache_mutex);
		if (kserial_mem_cgroup_cached) {
			btf = kserial_mem_cgroup_btf;
			type_id = (s32)kserial_mem_cgroup_type_id;
			mutex_unlock(&kserial_btf_cache_mutex);
			ret = resolve_path(btf, (u32)type_id, field_path, out_offset, out_size);
			if (ret)
				return ret;
			goto insert_path_cache;
		}
		mutex_unlock(&kserial_btf_cache_mutex);
	}

	type_id = bpf_find_btf_id(struct_name, BTF_KIND_STRUCT, &btf);
	if (type_id < 0 || !btf)
		return type_id < 0 ? type_id : -EINVAL;

	ret = resolve_path(btf, (u32)type_id, field_path, out_offset, out_size);
	if (strcmp(struct_name, KSERIAL_MEMCG_STRUCT_NAME) == 0) {
		mutex_lock(&kserial_btf_cache_mutex);
		if (!kserial_mem_cgroup_cached) {
			kserial_mem_cgroup_btf = btf;
			kserial_mem_cgroup_type_id = (u32)type_id;
			kserial_mem_cgroup_cached = true;
		} else {
			btf_put(btf);
		}
		mutex_unlock(&kserial_btf_cache_mutex);
	} else {
		btf_put(btf);
	}
	if (ret)
		return ret;

insert_path_cache:

	e = kmalloc(sizeof(*e), GFP_KERNEL);
	if (e) {
		memcpy(e->struct_name, key.struct_name, sizeof(key.struct_name));
		memcpy(e->field_path, key.field_path, sizeof(key.field_path));
		e->offset = *out_offset;
		e->size = *out_size;
		mutex_lock(&kserial_btf_cache_mutex);
		if (kserial_btf_cache_ready &&
		    rhashtable_lookup_insert_fast(&kserial_btf_cache, &e->node, kserial_btf_params) != 0)
			kfree(e);
		mutex_unlock(&kserial_btf_cache_mutex);
	}
	return 0;
}

static int __init kserial_btf_init(void)
{
	int err = rhashtable_init(&kserial_btf_cache, &kserial_btf_params);

	if (!err)
		kserial_btf_cache_ready = true;
	return err;
}

subsys_initcall(kserial_btf_init);

// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/cpuhotplug.h>
#include <linux/vmalloc.h>
#include <linux/sysfs.h>
#include <linux/init.h>

#include "zcomp.h"

#include "backend_lzo.h"
#include "backend_lzorle.h"
#include "backend_lz4.h"
#include "backend_lz4hc.h"
#include "backend_zstd.h"
#include "backend_deflate.h"
#include "backend_842.h"

/*
 * Registry of available compression backends, terminated by a NULL sentinel
 * that lookup_backend_ops() relies on.  Backends are matched by ->name and
 * exposed to users (zswap, zram) purely through this table, so no caller
 * hardcodes an algorithm name.
 *
 * To add a backend "foo":
 *   1. implement lib/zcomp/backend_foo.c (a struct zcomp_ops backend_foo)
 *      and declare it in lib/zcomp/backend_foo.h;
 *   2. #include "backend_foo.h" above;
 *   3. add a guarded &backend_foo entry to this array;
 *   4. add CONFIG_ZCOMP_BACKEND_FOO in lib/zcomp/Kconfig and the object in
 *      lib/zcomp/Makefile.
 * Nothing in zswap or zram needs to change for the new name to be usable.
 */
static const struct zcomp_ops *backends[] = {
#if IS_ENABLED(CONFIG_ZCOMP_BACKEND_LZO)
	&backend_lzorle,
	&backend_lzo,
#endif
#if IS_ENABLED(CONFIG_ZCOMP_BACKEND_LZ4)
	&backend_lz4,
#endif
#if IS_ENABLED(CONFIG_ZCOMP_BACKEND_LZ4HC)
	&backend_lz4hc,
#endif
#if IS_ENABLED(CONFIG_ZCOMP_BACKEND_ZSTD)
	&backend_zstd,
#endif
#if IS_ENABLED(CONFIG_ZCOMP_BACKEND_DEFLATE)
	&backend_deflate,
#endif
#if IS_ENABLED(CONFIG_ZCOMP_BACKEND_842)
	&backend_842,
#endif
	NULL
};

static void zcomp_strm_free(struct zcomp *comp, struct zcomp_strm *zstrm)
{
	comp->ops->destroy_ctx(&zstrm->ctx);
	vfree(zstrm->local_copy);
	vfree(zstrm->buffer);
	zstrm->buffer = NULL;
}

static int zcomp_strm_init(struct zcomp *comp, struct zcomp_strm *zstrm)
{
	int ret;

	ret = comp->ops->create_ctx(comp->params, &zstrm->ctx);
	if (ret)
		return ret;

	zstrm->local_copy = vzalloc(PAGE_SIZE);
	/*
	 * allocate 2 pages. 1 for compressed data, plus 1 extra for the
	 * case when compressed size is larger than the original one
	 */
	zstrm->buffer = vzalloc(2 * PAGE_SIZE);
	if (!zstrm->buffer || !zstrm->local_copy) {
		zcomp_strm_free(comp, zstrm);
		return -ENOMEM;
	}
	return 0;
}

static const struct zcomp_ops *lookup_backend_ops(const char *comp)
{
	int i = 0;

	while (backends[i]) {
		if (sysfs_streq(comp, backends[i]->name))
			break;
		i++;
	}
	return backends[i];
}

const char *zcomp_lookup_backend_name(const char *comp)
{
	const struct zcomp_ops *backend = lookup_backend_ops(comp);

	if (backend)
		return backend->name;

	return NULL;
}
EXPORT_SYMBOL_GPL(zcomp_lookup_backend_name);

/* show available compressors */
ssize_t zcomp_available_show(const char *comp, char *buf, ssize_t at)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(backends) - 1; i++) {
		if (!strcmp(comp, backends[i]->name)) {
			at += sysfs_emit_at(buf, at, "[%s] ",
					    backends[i]->name);
		} else {
			at += sysfs_emit_at(buf, at, "%s ", backends[i]->name);
		}
	}

	at += sysfs_emit_at(buf, at, "\n");
	return at;
}
EXPORT_SYMBOL_GPL(zcomp_available_show);

int zcomp_cpu_up_prepare(unsigned int cpu, struct hlist_node *node)
{
	struct zcomp *comp = hlist_entry(node, struct zcomp, node);
	struct zcomp_strm *zstrm = per_cpu_ptr(comp->stream, cpu);
	int ret;

	ret = zcomp_strm_init(comp, zstrm);
	if (ret)
		pr_err("Can't allocate a compression stream\n");
	return ret;
}
EXPORT_SYMBOL_GPL(zcomp_cpu_up_prepare);

int zcomp_cpu_dead(unsigned int cpu, struct hlist_node *node)
{
	struct zcomp *comp = hlist_entry(node, struct zcomp, node);
	struct zcomp_strm *zstrm = per_cpu_ptr(comp->stream, cpu);

	mutex_lock(&zstrm->lock);
	zcomp_strm_free(comp, zstrm);
	mutex_unlock(&zstrm->lock);
	return 0;
}
EXPORT_SYMBOL_GPL(zcomp_cpu_dead);

static int zcomp_init(struct zcomp *comp, struct zcomp_params *params)
{
	int ret, cpu;

	comp->stream = alloc_percpu(struct zcomp_strm);
	if (!comp->stream)
		return -ENOMEM;

	comp->params = params;
	ret = comp->ops->setup_params(comp->params);
	if (ret)
		goto cleanup;

	for_each_possible_cpu(cpu)
		mutex_init(&per_cpu_ptr(comp->stream, cpu)->lock);

	ret = cpuhp_state_add_instance(CPUHP_ZCOMP_PREPARE, &comp->node);
	if (ret < 0)
		goto cleanup;

	return 0;

cleanup:
	comp->ops->release_params(comp->params);
	free_percpu(comp->stream);
	return ret;
}

void zcomp_destroy(struct zcomp *comp)
{
	cpuhp_state_remove_instance(CPUHP_ZCOMP_PREPARE, &comp->node);
	comp->ops->release_params(comp->params);
	free_percpu(comp->stream);
	kfree(comp);
}
EXPORT_SYMBOL_GPL(zcomp_destroy);

struct zcomp *zcomp_create(const char *alg, struct zcomp_params *params)
{
	struct zcomp *comp;
	int error;

	/*
	 * The backends array has a sentinel NULL value, so the minimum
	 * size is 1. In order to be valid the array, apart from the
	 * sentinel NULL element, should have at least one compression
	 * backend selected.
	 */
	BUILD_BUG_ON(ARRAY_SIZE(backends) <= 1);

	comp = kzalloc_obj(struct zcomp);
	if (!comp)
		return ERR_PTR(-ENOMEM);

	comp->ops = lookup_backend_ops(alg);
	if (!comp->ops) {
		kfree(comp);
		return ERR_PTR(-EINVAL);
	}

	error = zcomp_init(comp, params);
	if (error) {
		kfree(comp);
		return ERR_PTR(error);
	}
	return comp;
}
EXPORT_SYMBOL_GPL(zcomp_create);

/*
 * Register the shared CPU-hotplug state used by every zcomp instance.
 *
 * The per-CPU compression streams are (de)allocated through this state, so it
 * must be set up before any zcomp user (zram, zswap, ...) calls
 * zcomp_create().  It used to be registered from zram's module init, which
 * left zcomp unusable for zswap (and other users) unless zram happened to be
 * loaded first.  Do it here in the library with a core_initcall so it is
 * always available regardless of which user comes first.
 *
 * If the registration fails there is no silent fallback: the error is
 * logged, the initcall return value is otherwise ignored by the boot code,
 * and every later zcomp_create() fails at cpuhp_state_add_instance() because
 * CPUHP_ZCOMP_PREPARE was never set up.  Users therefore see zcomp become
 * unavailable rather than operating on unallocated per-CPU streams.
 */
static int __init zcomp_core_init(void)
{
	int ret;

	ret = cpuhp_setup_state_multi(CPUHP_ZCOMP_PREPARE, "lib/zcomp:prepare",
				      zcomp_cpu_up_prepare, zcomp_cpu_dead);
	if (ret < 0)
		pr_err("zcomp: failed to register cpuhp state: %d\n", ret);
	return ret;
}
core_initcall(zcomp_core_init);

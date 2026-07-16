// SPDX-License-Identifier: GPL-2.0
/*
 * Swap page compression owned by the mm core.
 *
 * Backends such as zram store opaque blobs; compress/decompress policy
 * and execution live here rather than in block drivers.
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/swap_compress.h>

#include "zcomp.h"

/*
 * Primary swap compressor handle.
 *
 * One instance per backing device rather than a single global, so several zram
 * devices do not share one set of per-CPU streams (no cross-device compression
 * contention) and may safely use different algorithms.  The mm core still owns
 * the compression code; the device just holds an opaque handle to an instance.
 */
struct swap_comp {
	struct zcomp *zcomp;
};

/*
 * Create a primary swap compressor instance for @alg with @params (compression
 * level, dictionary, deflate.winbits).  Each backing device gets its own, so
 * devices do not share per-CPU streams and may use different algorithms and
 * parameters independently.
 */
struct swap_comp *swap_comp_create(const char *alg, struct zcomp_params *params)
{
	struct swap_comp *sc;
	struct zcomp *zcomp;

	if (!alg || !*alg || !params)
		return ERR_PTR(-EINVAL);

	sc = kzalloc(sizeof(*sc), GFP_KERNEL);
	if (!sc)
		return ERR_PTR(-ENOMEM);

	zcomp = zcomp_create(alg, params);
	if (IS_ERR(zcomp)) {
		kfree(sc);
		return ERR_CAST(zcomp);
	}

	sc->zcomp = zcomp;
	return sc;
}
EXPORT_SYMBOL_GPL(swap_comp_create);

void swap_comp_destroy(struct swap_comp *sc)
{
	if (!sc)
		return;
	zcomp_destroy(sc->zcomp);
	kfree(sc);
}
EXPORT_SYMBOL_GPL(swap_comp_destroy);

struct zcomp_strm *swap_compress_stream_get(struct swap_comp *sc)
{
	if (!sc)
		return NULL;
	return zcomp_stream_get(sc->zcomp);
}
EXPORT_SYMBOL_GPL(swap_compress_stream_get);

void swap_compress_stream_put(struct zcomp_strm *zstrm)
{
	zcomp_stream_put(zstrm);
}
EXPORT_SYMBOL_GPL(swap_compress_stream_put);

int swap_compress(struct swap_comp *sc, struct zcomp_strm *zstrm,
		  const void *src, unsigned int *dst_len)
{
	if (!sc)
		return -ENODEV;
	return zcomp_compress(sc->zcomp, zstrm, src, dst_len);
}
EXPORT_SYMBOL_GPL(swap_compress);

int swap_decompress(struct swap_comp *sc, struct zcomp_strm *zstrm,
		    const void *src, unsigned int src_len, void *dst)
{
	if (!sc)
		return -ENODEV;
	return zcomp_decompress(sc->zcomp, zstrm, src, src_len, dst);
}
EXPORT_SYMBOL_GPL(swap_decompress);

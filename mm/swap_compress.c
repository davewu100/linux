// SPDX-License-Identifier: GPL-2.0
/*
 * Swap page compression owned by the mm core.
 *
 * Backends such as zram store opaque blobs; compress/decompress policy
 * and execution live here rather than in block drivers.
 */
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/crypto.h>
#include <linux/swap_compress.h>

#include "zcomp.h"

static DEFINE_MUTEX(swap_comp_mutex);
static struct zcomp *swap_comp;
static char swap_comp_alg[CRYPTO_MAX_ALG_NAME];

static int swap_comp_create_locked(const char *alg)
{
	struct zcomp *comp;
	struct zcomp_params params = {
		.level = ZCOMP_PARAM_NOT_SET,
		.deflate.winbits = ZCOMP_PARAM_NOT_SET,
	};

	comp = zcomp_create(alg, &params);
	if (IS_ERR(comp))
		return PTR_ERR(comp);

	strscpy(swap_comp_alg, alg, sizeof(swap_comp_alg));
	swap_comp = comp;
	return 0;
}

static int swap_comp_ensure(void)
{
	int ret = 0;

	if (swap_comp)
		return 0;

	mutex_lock(&swap_comp_mutex);
	if (!swap_comp) {
		const char *alg;

#ifdef CONFIG_ZRAM_DEF_COMP
		alg = CONFIG_ZRAM_DEF_COMP;
#else
		alg = "lzo-rle";
#endif
		ret = swap_comp_create_locked(alg);
	}
	mutex_unlock(&swap_comp_mutex);
	return ret;
}

int swap_compress_set_algorithm(const char *alg)
{
	struct zcomp *comp, *old;

	if (!alg || !*alg)
		return -EINVAL;

	comp = zcomp_create(alg, &(struct zcomp_params){
		.level = ZCOMP_PARAM_NOT_SET,
		.deflate.winbits = ZCOMP_PARAM_NOT_SET,
	});
	if (IS_ERR(comp))
		return PTR_ERR(comp);

	mutex_lock(&swap_comp_mutex);
	old = swap_comp;
	swap_comp = comp;
	strscpy(swap_comp_alg, alg, sizeof(swap_comp_alg));
	mutex_unlock(&swap_comp_mutex);

	if (old)
		zcomp_destroy(old);
	return 0;
}
EXPORT_SYMBOL_GPL(swap_compress_set_algorithm);

struct zcomp_strm *swap_compress_stream_get(void)
{
	if (swap_comp_ensure())
		return NULL;
	return zcomp_stream_get(swap_comp);
}
EXPORT_SYMBOL_GPL(swap_compress_stream_get);

void swap_compress_stream_put(struct zcomp_strm *zstrm)
{
	zcomp_stream_put(zstrm);
}
EXPORT_SYMBOL_GPL(swap_compress_stream_put);

int swap_compress(struct zcomp_strm *zstrm, const void *src, unsigned int *dst_len)
{
	if (!swap_comp)
		return -ENODEV;
	return zcomp_compress(swap_comp, zstrm, src, dst_len);
}
EXPORT_SYMBOL_GPL(swap_compress);

int swap_decompress(struct zcomp_strm *zstrm, const void *src, unsigned int src_len,
		    void *dst)
{
	if (!swap_comp)
		return -ENODEV;
	return zcomp_decompress(swap_comp, zstrm, src, src_len, dst);
}
EXPORT_SYMBOL_GPL(swap_decompress);

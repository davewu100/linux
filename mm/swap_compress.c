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
#include <linux/sched.h>
#include <linux/mm.h>
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

/*
 * Change the shared core-swap compressor.
 *
 * WARNING: compressed-blob passthrough (see
 * Documentation/mm/core-swap-compressed-passthrough.md) stores blobs on the
 * backing device that can only be decoded by the codec that produced them.
 * Changing the codec while such blobs are live on disk would make them
 * undecodable.  Callers must ensure no passthrough blobs outlive the old codec
 * (e.g. only allow a change when no swap area with passthrough slots is
 * active).  This is not yet enforced here and is a prerequisite for enabling
 * passthrough on a system that reconfigures the compressor at runtime.
 */
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

/*
 * Compressed-blob passthrough length transport.
 *
 * A precompressed writeback issues a synchronous bio and blocks on it in the
 * submitting task's context (zram runs ->submit_bio synchronously in the same
 * task, with no worker handoff).  The compressed length is therefore recorded
 * on the task, which keeps it valid across the submit_bio_wait() sleep and
 * unambiguous even when several CPUs write back concurrently -- without ever
 * becoming a struct bio property or a block-layer flag.
 *
 * current->swap_precompressed_len == 0 means "no passthrough in flight" (an
 * ordinary raw-page write).  Nesting is not expected on the writeback path, so
 * begin() over an already-open region is a bug.
 */
void swap_precompressed_write_begin(unsigned int comp_len)
{
	WARN_ON_ONCE(current->swap_precompressed_len);
	current->swap_precompressed_len = comp_len;
}
EXPORT_SYMBOL_GPL(swap_precompressed_write_begin);

void swap_precompressed_write_end(void)
{
	current->swap_precompressed_len = 0;
}
EXPORT_SYMBOL_GPL(swap_precompressed_write_end);

bool swap_precompressed_write_len(unsigned int *comp_len)
{
	unsigned int len = current->swap_precompressed_len;

	if (!len || len >= PAGE_SIZE)
		return false;
	*comp_len = len;
	return true;
}
EXPORT_SYMBOL_GPL(swap_precompressed_write_len);

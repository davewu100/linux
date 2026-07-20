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

/*
 * Compressed-blob passthrough with heterogeneous codecs.
 *
 * A passthrough blob carries the identity of the codec that produced it (a
 * stable zcomp backend id, see zcomp_lookup_backend_id()).  zswap and the zram
 * primary slot may use different algorithms, so on swapin the backing store
 * must decode a blob with the codec that produced it, not with its own primary
 * codec.  Rather than share a single global compressor, we keep a small
 * id-indexed cache of zcomp instances, created lazily the first time a codec is
 * needed and reused afterwards.  The cache is bounded by the number of
 * compiled-in backends.
 *
 * swap_codec_cache[id] == NULL means "not yet created".
 */
#define SWAP_CODEC_CACHE_MAX	8
static struct zcomp *swap_codec_cache[SWAP_CODEC_CACHE_MAX];
static DEFINE_MUTEX(swap_codec_cache_mutex);

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
 * Change the primary core-swap compressor (the one the backing store uses for
 * its own, non-passthrough, primary slots).
 *
 * Note: compressed-blob passthrough no longer depends on this primary codec.
 * A passthrough blob records the id of the codec that produced it and is decoded
 * through the id-indexed cache (swap_decompress_by_id()), so changing the
 * primary codec here does not affect already-stored passthrough blobs.  The
 * id->codec mapping is stable for a kernel image (a backend's index in
 * lib/zcomp's backends[]), so passthrough blobs remain decodable across a
 * primary-codec change.
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
 * Return a cached zcomp instance for backend @id, creating it on first use.
 * The passthrough read path needs the exact codec that produced a stored blob,
 * which may differ from the primary codec, so instances are keyed by the stable
 * backend id rather than sharing one global compressor.
 */
static struct zcomp *swap_codec_get(int id)
{
	struct zcomp *comp;
	const char *name;

	if (id < 0 || id >= SWAP_CODEC_CACHE_MAX)
		return ERR_PTR(-EINVAL);

	comp = READ_ONCE(swap_codec_cache[id]);
	if (comp)
		return comp;

	name = zcomp_backend_name_by_id(id);
	if (!name)
		return ERR_PTR(-EINVAL);

	mutex_lock(&swap_codec_cache_mutex);
	comp = swap_codec_cache[id];
	if (!comp) {
		comp = zcomp_create(name, &(struct zcomp_params){
			.level = ZCOMP_PARAM_NOT_SET,
			.deflate.winbits = ZCOMP_PARAM_NOT_SET,
		});
		if (!IS_ERR(comp))
			WRITE_ONCE(swap_codec_cache[id], comp);
	}
	mutex_unlock(&swap_codec_cache_mutex);
	return comp;
}

/*
 * Backend id for an algorithm name (for the write path to stamp onto a stored
 * blob).  Returns a negative errno for an unknown/unsupported name.
 */
int swap_compress_alg_id(const char *alg)
{
	if (!alg || !*alg)
		return -EINVAL;
	return zcomp_lookup_backend_id(alg);
}
EXPORT_SYMBOL_GPL(swap_compress_alg_id);

/*
 * Acquire the per-CPU stream of the codec identified by @alg_id (the codec that
 * produced a passthrough blob).  The returned stream is held with its lock, so
 * it must be released with swap_compress_stream_put().  Its ->local_copy can be
 * used by the backing store as a bounce buffer for a zsmalloc object that spans
 * two physical pages, avoiding a second, separately-locked stream.  Returns NULL
 * if the codec is unavailable.
 */
struct zcomp_strm *swap_decompress_stream_by_id_get(int alg_id)
{
	struct zcomp *comp = swap_codec_get(alg_id);

	if (IS_ERR(comp))
		return NULL;
	return zcomp_stream_get(comp);
}
EXPORT_SYMBOL_GPL(swap_decompress_stream_by_id_get);

/*
 * Decompress a passthrough blob with the codec identified by @alg_id (the codec
 * that produced it), independent of any primary codec, using a stream already
 * acquired via swap_decompress_stream_by_id_get().  @src_len bytes at @src
 * decompress into a full page at @dst.
 */
int swap_decompress_by_id(int alg_id, struct zcomp_strm *zstrm,
			  const void *src, unsigned int src_len, void *dst)
{
	struct zcomp *comp = swap_codec_get(alg_id);

	if (IS_ERR(comp))
		return PTR_ERR(comp);
	return zcomp_decompress(comp, zstrm, src, src_len, dst);
}
EXPORT_SYMBOL_GPL(swap_decompress_by_id);

/*
 * Compressed-blob passthrough transport.
 *
 * A precompressed writeback issues a synchronous bio and blocks on it in the
 * submitting task's context (zram runs ->submit_bio synchronously in the same
 * task, with no worker handoff).  The compressed length and the id of the codec
 * that produced the blob are therefore recorded on the task, which keeps them
 * valid across the submit_bio_wait() sleep and unambiguous even when several
 * CPUs write back concurrently -- without ever becoming a struct bio property
 * or a block-layer flag.
 *
 * The codec id makes the passthrough codec-agnostic: zswap and the backing zram
 * primary slot may use different algorithms, and the backing store decodes a
 * blob with the codec that produced it (swap_decompress_by_id()), not with its
 * own primary codec.
 *
 * current->swap_precompressed_len == 0 means "no passthrough in flight" (an
 * ordinary raw-page write).  Nesting is not expected on the writeback path, so
 * begin() over an already-open region is a bug.
 */
void swap_precompressed_write_begin(unsigned int comp_len, int alg_id)
{
	WARN_ON_ONCE(current->swap_precompressed_len);
	current->swap_precompressed_len = comp_len;
	current->swap_precompressed_alg_id = alg_id;
}
EXPORT_SYMBOL_GPL(swap_precompressed_write_begin);

void swap_precompressed_write_end(void)
{
	current->swap_precompressed_len = 0;
	current->swap_precompressed_alg_id = 0;
}
EXPORT_SYMBOL_GPL(swap_precompressed_write_end);

bool swap_precompressed_write_len(unsigned int *comp_len, int *alg_id)
{
	unsigned int len = current->swap_precompressed_len;

	if (!len || len >= PAGE_SIZE)
		return false;
	*comp_len = len;
	*alg_id = current->swap_precompressed_alg_id;
	return true;
}
EXPORT_SYMBOL_GPL(swap_precompressed_write_len);

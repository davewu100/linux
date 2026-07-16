/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SWAP_COMPRESS_H
#define _LINUX_SWAP_COMPRESS_H

#include <linux/types.h>

struct zcomp_strm;
struct folio;

#ifdef CONFIG_SWAP_COMPRESS

struct zcomp_strm *swap_compress_stream_get(void);
void swap_compress_stream_put(struct zcomp_strm *zstrm);
int swap_compress(struct zcomp_strm *zstrm, const void *src, unsigned int *dst_len);
int swap_decompress(struct zcomp_strm *zstrm, const void *src, unsigned int src_len,
		    void *dst);
int swap_compress_set_algorithm(const char *alg);

/*
 * Compressed-blob passthrough (see
 * Documentation/mm/core-swap-compressed-passthrough.md).
 *
 * On writeback, zswap holds a blob that was produced by the shared core-swap
 * codec (swap_compress()).  Because the backing device (zram primary slot)
 * decompresses through the same codec, the blob can be stored verbatim instead
 * of decompressing it into a page and letting the backing device recompress it.
 *
 * The only datum the backing store needs that it cannot recover on its own is
 * the compressed length.  It is transported through a per-task descriptor
 * (current->swap_precompressed_len), scoped to a single synchronous write that
 * the backing store runs in this same task, so it never becomes a struct bio
 * property or a block-layer flag.
 */

/*
 * Begin a precompressed write on the current task: record @comp_len for the
 * synchronous bio the caller is about to submit.  Must be paired with
 * swap_precompressed_write_end().  Not expected to nest.
 */
void swap_precompressed_write_begin(unsigned int comp_len);

/* End the precompressed write region opened by swap_precompressed_write_begin(). */
void swap_precompressed_write_end(void);

/*
 * Backing-store query: if a precompressed write is in flight on this task,
 * store its length in @comp_len and return true.  @comp_len is guaranteed
 * 0 < *comp_len < PAGE_SIZE.  Returns false for an ordinary raw-page write.
 */
bool swap_precompressed_write_len(unsigned int *comp_len);

#else /* CONFIG_SWAP_COMPRESS */

static inline struct zcomp_strm *swap_compress_stream_get(void)
{
	return NULL;
}

static inline void swap_compress_stream_put(struct zcomp_strm *zstrm)
{
}

static inline int swap_compress(struct zcomp_strm *zstrm, const void *src,
				unsigned int *dst_len)
{
	return -EOPNOTSUPP;
}

static inline int swap_decompress(struct zcomp_strm *zstrm, const void *src,
				  unsigned int src_len, void *dst)
{
	return -EOPNOTSUPP;
}

static inline int swap_compress_set_algorithm(const char *alg)
{
	return -EOPNOTSUPP;
}

static inline void swap_precompressed_write_begin(unsigned int comp_len)
{
}

static inline void swap_precompressed_write_end(void)
{
}

static inline bool swap_precompressed_write_len(unsigned int *comp_len)
{
	return false;
}

#endif /* CONFIG_SWAP_COMPRESS */

#endif /* _LINUX_SWAP_COMPRESS_H */

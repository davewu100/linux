/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SWAP_COMPRESS_H
#define _LINUX_SWAP_COMPRESS_H

struct zcomp_strm;
struct swap_comp;

#ifdef CONFIG_SWAP_COMPRESS

/*
 * Primary swap compressor handle.
 *
 * The mm core owns the compression code, but an instance is per-caller rather
 * than a single global: each backing device (e.g. each zram device) creates its
 * own handle, so devices do not share one set of per-CPU streams and do not
 * contend on each other's compression, and different devices may use different
 * algorithms safely.
 */
struct swap_comp *swap_comp_create(const char *alg);
void swap_comp_destroy(struct swap_comp *sc);

struct zcomp_strm *swap_compress_stream_get(struct swap_comp *sc);
void swap_compress_stream_put(struct zcomp_strm *zstrm);
int swap_compress(struct swap_comp *sc, struct zcomp_strm *zstrm,
		  const void *src, unsigned int *dst_len);
int swap_decompress(struct swap_comp *sc, struct zcomp_strm *zstrm,
		    const void *src, unsigned int src_len, void *dst);

#else /* CONFIG_SWAP_COMPRESS */

static inline struct swap_comp *swap_comp_create(const char *alg)
{
	return NULL;
}

static inline void swap_comp_destroy(struct swap_comp *sc)
{
}

static inline struct zcomp_strm *swap_compress_stream_get(struct swap_comp *sc)
{
	return NULL;
}

static inline void swap_compress_stream_put(struct zcomp_strm *zstrm)
{
}

static inline int swap_compress(struct swap_comp *sc, struct zcomp_strm *zstrm,
				const void *src, unsigned int *dst_len)
{
	return -EOPNOTSUPP;
}

static inline int swap_decompress(struct swap_comp *sc, struct zcomp_strm *zstrm,
				  const void *src, unsigned int src_len,
				  void *dst)
{
	return -EOPNOTSUPP;
}

#endif /* CONFIG_SWAP_COMPRESS */

#endif /* _LINUX_SWAP_COMPRESS_H */

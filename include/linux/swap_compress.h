/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SWAP_COMPRESS_H
#define _LINUX_SWAP_COMPRESS_H

struct zcomp_strm;

#ifdef CONFIG_SWAP_COMPRESS

struct zcomp_strm *swap_compress_stream_get(void);
void swap_compress_stream_put(struct zcomp_strm *zstrm);
int swap_compress(struct zcomp_strm *zstrm, const void *src, unsigned int *dst_len);
int swap_decompress(struct zcomp_strm *zstrm, const void *src, unsigned int src_len,
		    void *dst);
void *swap_compress_buffer(struct zcomp_strm *zstrm);
int swap_compress_set_algorithm(const char *alg);

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

static inline void *swap_compress_buffer(struct zcomp_strm *zstrm)
{
	return NULL;
}

static inline int swap_compress_set_algorithm(const char *alg)
{
	return -EOPNOTSUPP;
}

#endif /* CONFIG_SWAP_COMPRESS */

#endif /* _LINUX_SWAP_COMPRESS_H */

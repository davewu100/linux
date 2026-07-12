/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef _BACKEND_ACOMP_H_
#define _BACKEND_ACOMP_H_

#include <linux/crypto.h>
#include "zcomp.h"

extern const struct zcomp_ops backend_acomp_deflate;

const char *zcomp_acomp_crypto_name(struct zcomp *comp);
bool zcomp_acomp_async_decompress(struct zcomp *comp);
int zcomp_acomp_decompress_async(struct zcomp *comp, struct zcomp_strm *zstrm,
				 const void *src, unsigned int src_len,
				 struct page *dst, crypto_completion_t cb,
				 void *data);

#endif /* _BACKEND_ACOMP_H_ */

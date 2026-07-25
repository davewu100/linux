/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef _ZCOMP_H_
#define _ZCOMP_H_

#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/kernel.h>
#include <linux/mm.h>	/* PAGE_SIZE */

#define ZCOMP_PARAM_NOT_SET	INT_MIN

struct deflate_params {
	s32 winbits;
};

/*
 * Immutable driver (backend) parameters. The driver may attach private
 * data to it (e.g. driver representation of the dictionary, etc.).
 *
 * This data is kept per-comp and is shared among execution contexts.
 */
struct zcomp_params {
	void *dict;
	size_t dict_sz;
	s32 level;
	union {
		struct deflate_params deflate;
	};

	void *drv_data;
};

/*
 * Run-time driver context - scratch buffers, etc. It is modified during
 * request execution (compression/decompression), cannot be shared, so
 * it's in per-CPU area.
 */
struct zcomp_ctx {
	void *context;
};

struct zcomp_strm {
	struct mutex lock;
	/* compression buffer */
	void *buffer;
	/* local copy of handle memory */
	void *local_copy;
	struct zcomp_ctx ctx;
};

struct zcomp_req {
	const unsigned char *src;
	const size_t src_len;

	unsigned char *dst;
	size_t dst_len;
};

struct zcomp_ops {
	int (*compress)(struct zcomp_params *params, struct zcomp_ctx *ctx,
			struct zcomp_req *req);
	int (*decompress)(struct zcomp_params *params, struct zcomp_ctx *ctx,
			  struct zcomp_req *req);

	int (*create_ctx)(struct zcomp_params *params, struct zcomp_ctx *ctx);
	void (*destroy_ctx)(struct zcomp_ctx *ctx);

	int (*setup_params)(struct zcomp_params *params);
	void (*release_params)(struct zcomp_params *params);

	const char *name;
};

/* dynamic per-device compression frontend */
struct zcomp {
	struct zcomp_strm __percpu *stream;
	const struct zcomp_ops *ops;
	struct zcomp_params *params;
	struct hlist_node node;
};

int zcomp_cpu_up_prepare(unsigned int cpu, struct hlist_node *node);
int zcomp_cpu_dead(unsigned int cpu, struct hlist_node *node);
ssize_t zcomp_available_show(const char *comp, char *buf, ssize_t at);
const char *zcomp_lookup_backend_name(const char *comp);

struct zcomp *zcomp_create(const char *alg, struct zcomp_params *params);
void zcomp_destroy(struct zcomp *comp);

/*
 * These are tiny wrappers on the (de)compression hot path.  They are kept
 * inline in the header so that callers built as separate modules (e.g. zram
 * with CONFIG_ZRAM=m) do not pay a cross-module call into the built-in
 * lib/zcomp on every page.  Only the ->ops->{compress,decompress} indirect
 * call remains, which is inherent to the backend abstraction.
 */
static inline struct zcomp_strm *zcomp_stream_get(struct zcomp *comp)
{
	for (;;) {
		struct zcomp_strm *zstrm = raw_cpu_ptr(comp->stream);

		/*
		 * stream is returned with ->lock held which prevents
		 * cpu_dead() from releasing this stream under us, however
		 * there is still a race window between raw_cpu_ptr() and
		 * mutex_lock(), during which we could have been migrated
		 * from a CPU that has already destroyed its stream.  If
		 * so then unlock and re-try on the current CPU.
		 */
		mutex_lock(&zstrm->lock);
		if (likely(zstrm->buffer))
			return zstrm;
		mutex_unlock(&zstrm->lock);
	}
}

static inline void zcomp_stream_put(struct zcomp_strm *zstrm)
{
	mutex_unlock(&zstrm->lock);
}

static inline int zcomp_compress(struct zcomp *comp, struct zcomp_strm *zstrm,
				 const void *src, unsigned int *dst_len)
{
	struct zcomp_req req = {
		.src = src,
		.dst = zstrm->buffer,
		.src_len = PAGE_SIZE,
		.dst_len = 2 * PAGE_SIZE,
	};
	int ret;

	might_sleep();
	ret = comp->ops->compress(comp->params, &zstrm->ctx, &req);
	if (!ret)
		*dst_len = req.dst_len;
	return ret;
}

static inline int zcomp_decompress(struct zcomp *comp, struct zcomp_strm *zstrm,
				   const void *src, unsigned int src_len,
				   void *dst)
{
	struct zcomp_req req = {
		.src = src,
		.dst = dst,
		.src_len = src_len,
		.dst_len = PAGE_SIZE,
	};

	might_sleep();
	return comp->ops->decompress(comp->params, &zstrm->ctx, &req);
}

#endif /* _ZCOMP_H_ */

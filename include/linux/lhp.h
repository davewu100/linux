/* SPDX-License-Identifier: GPL-2.0 */
/*
 * LHP - Layered HugePage allocator.
 *
 * DPDK-style layering:
 *
 *   phys memory (CMA reserve at boot)
 *        -> lhp_phys_pool (1G/2M/4K split/merge)
 *        -> lhp_memzone   (named contiguous zone, rte_memzone-like)
 *        -> lhp_obj_pool  (fixed-size object pool, rte_mempool-like)
 *        -> lhp_buffer    (caller-visible allocation with header)
 *
 * Low-level chunk API (lhp_alloc/lhp_free) operates on the phys pool.
 * Callers that want kmalloc-like behaviour use lhp_kmalloc()/lhp_kfree().
 */
#ifndef _LINUX_LHP_H
#define _LINUX_LHP_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/gfp_types.h>
#include <linux/refcount.h>

struct kmem_cache;

/* Fixed three-level hierarchy.  Orders assume 4K base pages. */
enum lhp_level {
	LHP_LEVEL_4K = 0,
	LHP_LEVEL_2M,
	LHP_LEVEL_1G,
	LHP_NR_LEVELS,
};

#define LHP_ORDER_4K		0
#define LHP_ORDER_2M		9
#define LHP_ORDER_1G		18

/* Fan-out from a node to its children (512 on x86-64). */
#define LHP_FANOUT		512

#define LHP_NAME_MAX		32

/* Memzone creation flags (extend as needed). */
#define LHP_MEMZONE_F_NONE	0

/* Object pool creation flags. */
#define LHP_POOL_F_NONE		0

static inline unsigned int lhp_level_order(enum lhp_level level)
{
	switch (level) {
	case LHP_LEVEL_1G:
		return LHP_ORDER_1G;
	case LHP_LEVEL_2M:
		return LHP_ORDER_2M;
	default:
		return LHP_ORDER_4K;
	}
}

static inline unsigned long lhp_level_nr_pages(enum lhp_level level)
{
	return 1UL << lhp_level_order(level);
}

#ifdef CONFIG_LHP

struct page;

struct lhp_memzone;
struct lhp_obj_pool;

/*
 * Boot-time contiguous reservation.  Call from the arch mem-reserve path, in
 * the same window as hugetlb_cma_reserve() (memblock up, buddy not yet).
 */
void __init lhp_cma_reserve(void);

/* Phys pool ready (split/merge allocator initialised). */
int lhp_pool_available(void);

/*
 * Low-level chunk API on the phys pool.  Splits higher levels on demand.
 *
 * @gfp controls child-array preallocation for the split path.
 */
struct page *lhp_alloc(enum lhp_level level, gfp_t gfp);
void lhp_free(struct page *page, enum lhp_level level);

/*
 * Memzone: named contiguous backing range carved from the phys pool.
 * Analogous to rte_memzone_reserve().
 */
struct lhp_memzone *lhp_memzone_reserve(const char *name, size_t len,
					size_t align, int nid,
					unsigned long flags);
void lhp_memzone_free(struct lhp_memzone *zone);
struct lhp_memzone *lhp_memzone_lookup(const char *name);

/*
 * Object pool: fixed-size objects backed by a memzone.
 * Analogous to rte_mempool_create().
 */
struct lhp_obj_pool *lhp_pool_create(const char *name,
				     struct lhp_memzone *zone,
				     unsigned int obj_size,
				     unsigned int align,
				     unsigned long flags);
void lhp_pool_destroy(struct lhp_obj_pool *pool);
struct lhp_obj_pool *lhp_pool_lookup(const char *name);

void *lhp_pool_alloc(struct lhp_obj_pool *pool, gfp_t gfp);
void lhp_pool_free(struct lhp_obj_pool *pool, void *ptr);

/*
 * kmalloc-like API bound to an object pool / its memzone.
 * Small requests use the pool freelist; larger ones carve LHP chunks
 * from the memzone.  Always pair with lhp_kfree().
 */
void *lhp_kmalloc(struct lhp_obj_pool *pool, size_t size, gfp_t gfp);
void lhp_kfree(const void *ptr);

/* Detect LHP-owned pointers (for optional kfree() integration). */
bool lhp_ptr_is_owned(const void *ptr);

/*
 * Future SLUB integration (phase 4, not wired yet).
 * A kmem_cache backed by lhp_obj_pool would route alloc_slab_page() through
 * lhp_memzone_alloc_chunk() and __free_slab() through lhp_memzone_free_chunk().
 * See mm/lhp.c SLUB integration notes.
 */
struct lhp_obj_pool *
lhp_pool_for_slub_cache(const struct kmem_cache *s);

#else /* !CONFIG_LHP */

static inline void lhp_cma_reserve(void) { }
static inline int lhp_pool_available(void) { return 0; }
static inline struct page *lhp_alloc(enum lhp_level level, gfp_t gfp)
{
	return NULL;
}
static inline void lhp_free(struct page *page, enum lhp_level level) { }
static inline struct lhp_memzone *lhp_memzone_reserve(const char *name,
							size_t len, size_t align,
							int nid,
							unsigned long flags)
{
	return NULL;
}
static inline void lhp_memzone_free(struct lhp_memzone *zone) { }
static inline struct lhp_memzone *lhp_memzone_lookup(const char *name)
{
	return NULL;
}
static inline struct lhp_obj_pool *lhp_pool_create(const char *name,
						   struct lhp_memzone *zone,
						   unsigned int obj_size,
						   unsigned int align,
						   unsigned long flags)
{
	return NULL;
}
static inline void lhp_pool_destroy(struct lhp_obj_pool *pool) { }
static inline struct lhp_obj_pool *lhp_pool_lookup(const char *name)
{
	return NULL;
}
static inline void *lhp_pool_alloc(struct lhp_obj_pool *pool, gfp_t gfp)
{
	return NULL;
}
static inline void lhp_pool_free(struct lhp_obj_pool *pool, void *ptr) { }
static inline void *lhp_kmalloc(struct lhp_obj_pool *pool, size_t size, gfp_t gfp)
{
	return NULL;
}
static inline void lhp_kfree(const void *ptr) { }
static inline bool lhp_ptr_is_owned(const void *ptr) { return false; }
static inline struct lhp_obj_pool *
lhp_pool_for_slub_cache(const struct kmem_cache *s)
{
	return NULL;
}

#endif /* CONFIG_LHP */

#endif /* _LINUX_LHP_H */

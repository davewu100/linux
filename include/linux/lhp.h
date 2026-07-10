/* SPDX-License-Identifier: GPL-2.0 */
/*
 * LHP - Large HugePage pool.
 *
 * A self-contained large-page pool with a DPDK-style layering on top.  It
 * reserves a physically contiguous area at boot via CMA and carves it into 1G
 * regions, then hands memory out at two granularities:
 *
 *   phys memory (CMA reserve at boot)
 *        -> lhp_phys_pool          (1G regions, each a container of 512 2M
 *                                    chunks)
 *             -> lhp_alloc_2m()    a single 2M chunk (O(1) per-region bitmap)
 *             -> lhp_alloc_1g()    a whole 1G region as one contiguous block
 *        -> lhp_memzone            named contiguous zone built from 2M chunks
 *                                  (rte_memzone-like)
 *        -> lhp_heap               rte_malloc-like variable-size allocator,
 *                                  backed by either a memzone (<=2M objects) or
 *                                  a whole 1G region (<=1G objects)
 *
 * 2M is the small granularity: chunks are allocated and freed whole, never
 * split or merged, so a region never changes shape and physical contiguity
 * within a region is guaranteed by the 1G CMA block itself.  A whole region can
 * instead be handed out as a single 1G block for large contiguous needs.
 *
 * This targets static, DPDK-style users: reserve memzones/heaps up front and
 * manage the backing memory themselves.  See Documentation/mm/lhp.rst.
 */
#ifndef _LINUX_LHP_H
#define _LINUX_LHP_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/gfp_types.h>
#include <linux/refcount.h>
#include <asm/page.h>

/* Fixed 2M chunk geometry (assumes 4K base pages). */
#define LHP_CHUNK_ORDER		9		/* 2M / 4K = 512 */
#define LHP_CHUNK_PAGES		(1UL << LHP_CHUNK_ORDER)
#define LHP_CHUNK_BYTES		(LHP_CHUNK_PAGES << PAGE_SHIFT)

/* A 1G region is just a container of this many 2M chunks. */
#define LHP_REGION_ORDER	18		/* 1G / 4K */
#define LHP_REGION_PAGES	(1UL << LHP_REGION_ORDER)
#define LHP_CHUNKS_PER_REGION	(LHP_REGION_PAGES / LHP_CHUNK_PAGES)	/* 512 */

#define LHP_NAME_MAX		32

/* Memzone creation flags (extend as needed). */
#define LHP_MEMZONE_F_NONE	0

/* Heap backing selection (see lhp_heap_create_policy()). */
enum lhp_heap_policy {
	LHP_HEAP_2M = 0,	/* memzone-backed, single alloc <= ~2M */
	LHP_HEAP_1G,		/* one 1G region, single alloc <= ~1G */
	LHP_HEAP_AUTO,		/* pick per @size: > one 2M chunk -> 1G */
};

#ifdef CONFIG_LHP

struct page;
struct lhp_memzone;

/*
 * Boot-time contiguous reservation.  Call from the arch mem-reserve path, in
 * the same window as hugetlb_cma_reserve() (memblock up, buddy not yet).
 */
void __init lhp_cma_reserve(void);

/* Phys pool ready (2M chunk allocator initialised). */
int lhp_pool_available(void);

/*
 * Low-level chunk API on the phys pool.  The unit is always one 2M chunk;
 * @gfp only controls any bookkeeping allocation (never the chunk itself).
 */
struct page *lhp_alloc_2m(gfp_t gfp);
void lhp_free_2m(struct page *page);

/*
 * Allocate/free a whole 1G region as one physically contiguous block.  A
 * region is eligible only when completely idle; while held as 1G it cannot be
 * carved into 2M chunks.  Intended to back a large single-arena heap.
 */
struct page *lhp_alloc_1g(gfp_t gfp);
void lhp_free_1g(struct page *page);

/*
 * Memzone: named contiguous backing range carved from the phys pool as a set
 * of 2M chunks.  Analogous to rte_memzone_reserve().  @len is rounded up to a
 * 2M multiple.
 */
struct lhp_memzone *lhp_memzone_reserve(const char *name, size_t len,
					int nid, unsigned long flags);
void lhp_memzone_free(struct lhp_memzone *zone);
struct lhp_memzone *lhp_memzone_lookup(const char *name);

/*
 * Iterate a memzone's backing 2M chunks.  @fn is called once per chunk with
 * the chunk's kernel virtual base and its length (always LHP_CHUNK_BYTES).
 * Iteration stops early if @fn returns non-zero, which becomes the return
 * value.  Used by the heap layer to lay out its arenas over each chunk; the
 * chunks are not guaranteed to be physically contiguous with one another.
 */
typedef int (*lhp_chunk_fn)(void *base, size_t len, void *priv);
int lhp_memzone_for_each_chunk(struct lhp_memzone *zone, lhp_chunk_fn fn,
			       void *priv);

/*
 * rte_malloc-style variable-size heap built on top of the LHP pool.  A heap
 * manages its backing memory as first-fit arenas with coalescing free lists;
 * lhp_free() takes only the pointer (the owning block and heap are recovered
 * from an inline header).  The backing determines the maximum single
 * allocation:
 *
 *   LHP_HEAP_2M    memzone-backed, one arena per (discontiguous) 2M chunk;
 *                  a single allocation cannot exceed ~2M, but the heap only
 *                  consumes as many 2M chunks as @size needs.
 *
 *   LHP_HEAP_1G    backed by one whole 1G region as a single contiguous arena;
 *                  a single allocation can be up to ~1G, but the heap consumes
 *                  a full 1G region regardless of @size.
 *
 *   LHP_HEAP_AUTO  pick based on @size: use a 1G region when @size needs a
 *                  single object larger than one 2M chunk (i.e. the caller is
 *                  telling us it wants large contiguous allocations), else 2M.
 */
struct lhp_heap;

struct lhp_heap *lhp_heap_create_policy(const char *name, size_t size, int nid,
					enum lhp_heap_policy policy, gfp_t gfp);

/* Backwards-compatible wrappers. */
struct lhp_heap *lhp_heap_create(const char *name, size_t size, int nid,
				 gfp_t gfp);
struct lhp_heap *lhp_heap_create_1g(const char *name, int nid, gfp_t gfp);

void lhp_heap_destroy(struct lhp_heap *h);
struct lhp_heap *lhp_heap_lookup(const char *name);

void *lhp_malloc(struct lhp_heap *h, size_t size, size_t align, gfp_t gfp);
void lhp_free(void *ptr);

/* True if @ptr was handed out by some lhp_heap (for optional kfree routing). */
bool lhp_heap_owns(const void *ptr);

#else /* !CONFIG_LHP */

static inline void lhp_cma_reserve(void) { }
static inline int lhp_pool_available(void) { return 0; }
static inline struct page *lhp_alloc_2m(gfp_t gfp) { return NULL; }
static inline void lhp_free_2m(struct page *page) { }
static inline struct page *lhp_alloc_1g(gfp_t gfp) { return NULL; }
static inline void lhp_free_1g(struct page *page) { }
static inline struct lhp_memzone *lhp_memzone_reserve(const char *name,
						      size_t len, int nid,
						      unsigned long flags)
{
	return NULL;
}
static inline void lhp_memzone_free(struct lhp_memzone *zone) { }
static inline struct lhp_memzone *lhp_memzone_lookup(const char *name)
{
	return NULL;
}

typedef int (*lhp_chunk_fn)(void *base, size_t len, void *priv);
static inline int lhp_memzone_for_each_chunk(struct lhp_memzone *zone,
					     lhp_chunk_fn fn, void *priv)
{
	return 0;
}

struct lhp_heap;
static inline struct lhp_heap *lhp_heap_create_policy(const char *name,
						      size_t size, int nid,
						      enum lhp_heap_policy policy,
						      gfp_t gfp)
{
	return NULL;
}
static inline struct lhp_heap *lhp_heap_create(const char *name, size_t size,
					       int nid, gfp_t gfp)
{
	return NULL;
}
static inline struct lhp_heap *lhp_heap_create_1g(const char *name, int nid,
						  gfp_t gfp)
{
	return NULL;
}
static inline void lhp_heap_destroy(struct lhp_heap *h) { }
static inline struct lhp_heap *lhp_heap_lookup(const char *name)
{
	return NULL;
}
static inline void *lhp_malloc(struct lhp_heap *h, size_t size, size_t align,
			       gfp_t gfp)
{
	return NULL;
}
static inline void lhp_free(void *ptr) { }
static inline bool lhp_heap_owns(const void *ptr) { return false; }

#endif /* CONFIG_LHP */

#endif /* _LINUX_LHP_H */

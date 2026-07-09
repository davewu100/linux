/* SPDX-License-Identifier: GPL-2.0 */
/*
 * LHP - Layered HugePage allocator (standalone, plan B).
 *
 * A self-contained three-level (1G / 2M / 4K) split/merge memory pool.  A pool
 * is carved out of physically contiguous memory at boot time and never handed
 * back to the buddy allocator: the pool only ever hands out *usage rights* to
 * chunks, while page ownership stays inside the pool.  Because the backing
 * memory never leaves, physical contiguity is preserved for free, which is what
 * makes a deterministic "merge back up to 1G" possible.
 *
 * Level layout (x86-64 PUD/PMD/PTE mirror):
 *
 *      LHP_LEVEL_1G (order 18)  --split-->  512 x LHP_LEVEL_2M
 *      LHP_LEVEL_2M (order 9)   --split-->  512 x LHP_LEVEL_4K
 *      LHP_LEVEL_4K (order 0)   leaf
 *
 * merge is the exact inverse and only succeeds when *all* children of a node
 * are free, which the pool tracks with a per-node free child counter (O(1)).
 */
#ifndef _LINUX_LHP_H
#define _LINUX_LHP_H

#include <linux/types.h>
#include <linux/list.h>

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

/*
 * Boot-time contiguous reservation.  Call from the arch mem-reserve path, in
 * the same window as hugetlb_cma_reserve() (memblock up, buddy not yet).
 */
void __init lhp_cma_reserve(void);

/*
 * Public pool API.  These operate on the single global pool for now; the data
 * structures are written so a multi-pool / per-node variant is a mechanical
 * follow-up.
 */
int lhp_pool_available(void);

/*
 * Allocate one chunk of the requested level, returning its head page (usage
 * right) or NULL.  Splits higher levels on demand.
 */
struct page *lhp_alloc(enum lhp_level level);

/*
 * Return a chunk previously obtained from lhp_alloc().  Merges back up towards
 * 1G whenever a node becomes fully free.
 */
void lhp_free(struct page *page, enum lhp_level level);

#else /* !CONFIG_LHP */

static inline void lhp_cma_reserve(void) { }
static inline int lhp_pool_available(void) { return 0; }
static inline struct page *lhp_alloc(enum lhp_level level) { return NULL; }
static inline void lhp_free(struct page *page, enum lhp_level level) { }

#endif /* CONFIG_LHP */

#endif /* _LINUX_LHP_H */

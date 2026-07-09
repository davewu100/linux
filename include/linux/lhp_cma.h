/* SPDX-License-Identifier: GPL-2.0 */
/*
 * LHP-CMA - Layered HugePage view over CMA (plan C).
 *
 * Rather than owning a private pool (see plan B), this variant layers a
 * 1G / 2M / 4K hierarchy *on top of* the existing Contiguous Memory Allocator.
 * Chunks are handed out by cma_alloc()/released by cma_release(), so the pages
 * are real, kernel-usable, and their contiguity + reclaim is guaranteed by CMA.
 * A thin layering bitmap tracks which 2M and 4K sub-ranges of each 1G region
 * are in use, giving the split/merge (buddy-like) semantics and a deterministic
 * "merge back to 1G" once a 1G region is fully idle.
 *
 * Trade-off vs plan B: less code and reuses CMA's proven migration/reclaim, but
 * the merge point is only observable (a 1G region is "whole" when its bitmap is
 * empty) rather than physically reconstructed, and CMA pages may transiently be
 * borrowed by movable allocations, so a 1G allocation may need to wait for
 * migration.
 */
#ifndef _LINUX_LHP_CMA_H
#define _LINUX_LHP_CMA_H

#include <linux/types.h>
#include <linux/init.h>

enum lhp_cma_level {
	LHP_CMA_4K = 0,
	LHP_CMA_2M,
	LHP_CMA_1G,
	LHP_CMA_NR_LEVELS,
};

#define LHP_CMA_ORDER_4K	0
#define LHP_CMA_ORDER_2M	9
#define LHP_CMA_ORDER_1G	18

/* 2M chunks per 1G, 4K pages per 2M. */
#define LHP_CMA_2M_PER_1G	512
#define LHP_CMA_4K_PER_2M	512

static inline unsigned int lhp_cma_level_order(enum lhp_cma_level level)
{
	switch (level) {
	case LHP_CMA_1G:
		return LHP_CMA_ORDER_1G;
	case LHP_CMA_2M:
		return LHP_CMA_ORDER_2M;
	default:
		return LHP_CMA_ORDER_4K;
	}
}

static inline unsigned long lhp_cma_level_nr_pages(enum lhp_cma_level level)
{
	return 1UL << lhp_cma_level_order(level);
}

#ifdef CONFIG_LHP_CMA

struct page;

void __init lhp_cma_reserve(void);
int lhp_cma_available(void);

/* Allocate one chunk at @level (real CMA pages), or NULL. */
struct page *lhp_cma_alloc(enum lhp_cma_level level);

/* Release a chunk previously returned by lhp_cma_alloc(). */
void lhp_cma_free(struct page *page, enum lhp_cma_level level);

#else /* !CONFIG_LHP_CMA */

static inline void lhp_cma_reserve(void) { }
static inline int lhp_cma_available(void) { return 0; }
static inline struct page *lhp_cma_alloc(enum lhp_cma_level level)
{
	return NULL;
}
static inline void lhp_cma_free(struct page *page, enum lhp_cma_level level) { }

#endif /* CONFIG_LHP_CMA */

#endif /* _LINUX_LHP_CMA_H */

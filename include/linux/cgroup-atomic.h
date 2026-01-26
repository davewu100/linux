/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _LINUX_CGROUP_ATOMIC_H
#define _LINUX_CGROUP_ATOMIC_H

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/mm_types.h>
#include <linux/mmzone.h>
#include <linux/vm_event_item.h>

#ifdef CONFIG_MEMCG_ATOMIC_COUNTER

/* Forward declarations */
struct mem_cgroup;
struct mem_cgroup_per_node;
struct lruvec;
enum node_stat_item;
enum vm_event_item;
enum memcg_stat_item;

/*
 * Import array size constants from memcontrol.h
 * These must be available as compile-time constants for struct declarations.
 */
#ifndef MEMCG_VMSTAT_SIZE
#include <linux/memcontrol.h>
#endif

/*
 * Per-node atomic counter based stats (similar to lruvec_stats)
 *
 * Per-node atomic counters for NUMA-aware statistics. Each node maintains
 * its own atomic counters for per-node per-cgroup statistics.
 */
struct memcg_atomic_counter_per_node {
	atomic64_t		state[NR_VM_NODE_STAT_ITEMS];
} ____cacheline_aligned_in_smp;

/*
 * Per-cgroup atomic counter based stats - RSTAT-LIKE DESIGN
 *
 * Each counter stores LOCAL values (this cgroup only, not hierarchical).
 * Reads use time-based cache with dirty tracking (like rstat).
 *
 * Design (similar to rstat):
 * - Write: Update local counter + mark self and ancestors dirty (upward propagation)
 * - Read: Flush if age > 2s, otherwise use cache (even if dirty)
 * - Flush: Traverse tree to aggregate, clear dirty flags
 *
 * Key insight: Dirty flag propagates upward, but within 2s window we don't flush!
 *
 * Trade-offs:
 * - Pros: Fast writes (atomic + dirty flag), minimal flushes (2s rate limit)
 * - Cons: Up to 2s staleness, dirty propagation overhead (better than value propagation)
 *
 * Best for: Write-heavy workloads (like rstat)
 */
struct memcg_atomic_counter {
	/* Local counters (this cgroup only, NOT hierarchical) */
	atomic64_t		state[MEMCG_VMSTAT_SIZE];
	atomic64_t		events[NR_MEMCG_EVENTS];

#ifdef CONFIG_MEMCG_V1
	/* Local counters for cgroup v1 compatibility */
	atomic64_t		state_local[MEMCG_VMSTAT_SIZE];
	atomic64_t		events_local[NR_MEMCG_EVENTS];
#endif
} ____cacheline_aligned_in_smp;

/* Time-based cache for aggregated stats (rstat-like design) */
struct memcg_atomic_cache {
	/* Cache state (similar to rstat) */
	bool dirty;			/* Has unflushed updates (propagated upward) */
	unsigned long flush_time;	/* jiffies when cache was last flushed */
	
	/* Cached aggregated values (self + all descendants) */
	u64 stats[MEMCG_VMSTAT_SIZE] ____cacheline_aligned_in_smp;
	unsigned long events[NR_MEMCG_EVENTS];
};

#define ATOMIC_CACHE_TTL (2UL * HZ)  /* 2 second TTL, same as rstat */

/* Core read functions - cache-based with 2s TTL */
u64 css_atomic_page_state(struct mem_cgroup *memcg, int idx, bool force);
unsigned long css_atomic_events(struct mem_cgroup *memcg,
				enum vm_event_item idx, bool force);
unsigned long css_atomic_events_recursive(struct mem_cgroup *memcg,
					  enum vm_event_item idx);

/* Cache flush - explicitly refresh cache (called on memory.stat read) */
void css_atomic_flush(struct mem_cgroup *memcg);

#else /* !CONFIG_MEMCG_ATOMIC_COUNTER */

static inline u64 css_atomic_page_state(struct mem_cgroup *memcg, int idx,
					bool force)
{
	return 0;
}
static inline unsigned long css_atomic_events(struct mem_cgroup *memcg,
					      enum vm_event_item idx, bool force)
{
	return 0;
}
static inline unsigned long css_atomic_events_recursive(struct mem_cgroup *memcg,
							enum vm_event_item idx)
{
	return 0;
}
static inline void css_atomic_flush(struct mem_cgroup *memcg) { }

#endif /* CONFIG_MEMCG_ATOMIC_COUNTER */

#endif /* _LINUX_CGROUP_ATOMIC_H */

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
 * Exactly mimics rstat's threshold-based flush logic.
 *
 * Design (exactly like rstat):
 * - Write: Update local counter + increment stats_updates upward
 * - Read: Directly read cache (NO flush, like rstat's READ_ONCE(vmstats))
 * - Flush: Only when updates > threshold, with 2s rate limit
 *
 * Flush logic (same as rstat's __mem_cgroup_flush_stats):
 * 1. Check stats_updates > THRESHOLD * num_online_cpus()
 * 2. Check rate limit: last flush >= 2s ago
 * 3. Only flush if both conditions met
 *
 * Key insight: Internal reads NEVER trigger flush!
 * - memcg_page_state(): just READ_ONCE(cache)
 * - memory.stat read: calls css_atomic_flush() which checks threshold
 *
 * Trade-offs:
 * - Pros: Very fast reads (O(1) always), writes cheap (atomic + counter inc)
 * - Cons: Staleness depends on update frequency, threshold-based consistency
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

/* Cache for aggregated stats (rstat-like design) */
struct memcg_atomic_cache {
	/* Update tracking (like rstat's stats_updates) */
	atomic_t stats_updates;		/* Number of unflushed updates */
	unsigned long flush_time;	/* jiffies when cache was last flushed */
	
	/* Cached aggregated values (self + all descendants) */
	u64 stats[MEMCG_VMSTAT_SIZE] ____cacheline_aligned_in_smp;
	unsigned long events[NR_MEMCG_EVENTS];
};

/* Flush threshold (like rstat's MEMCG_CHARGE_BATCH * num_online_cpus) */
#define ATOMIC_FLUSH_THRESHOLD (64)  /* Will be multiplied by num_online_cpus */

/* Rate limit interval (like rstat's FLUSH_TIME) */
#define ATOMIC_FLUSH_TIME (2UL * HZ)  /* 2 seconds, same as rstat */

/* Core read functions - cache-based with 2s TTL */
u64 css_atomic_page_state(struct mem_cgroup *memcg, int idx, bool force);
unsigned long css_atomic_events(struct mem_cgroup *memcg,
				enum vm_event_item idx, bool force);
unsigned long css_atomic_events_recursive(struct mem_cgroup *memcg,
					  enum vm_event_item idx);

/* Cache flush - rstat-like threshold + rate limit check */
void css_atomic_flush(struct mem_cgroup *memcg, bool force);

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
static inline void css_atomic_flush(struct mem_cgroup *memcg, bool force) { }

#endif /* CONFIG_MEMCG_ATOMIC_COUNTER */

#endif /* _LINUX_CGROUP_ATOMIC_H */

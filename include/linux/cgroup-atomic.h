/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _LINUX_CGROUP_ATOMIC_H
#define _LINUX_CGROUP_ATOMIC_H

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/seqlock.h>
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
 * Per-cgroup atomic counter based stats (experimental alternative to rstat)
 *
 * Single atomic counter per cgroup (not per-CPU). Each stat update uses
 * atomic64_add() to update the counter, and reads can directly access the
 * value with atomic64_read(). Hierarchical stats are computed by recursively
 * aggregating all children using RCU-protected tree traversal.
 *
 * Trade-offs:
 * - Pros: Fast reads (no per-CPU aggregation), lock-free recursive queries
 * - Cons: Potential cache line bouncing on writes, higher write overhead
 */
struct memcg_atomic_counter {
	/* Hierarchical counters (includes all descendants) */
	atomic64_t		state[MEMCG_VMSTAT_SIZE];
	atomic64_t		events[NR_MEMCG_EVENTS];

#ifdef CONFIG_MEMCG_V1
	/* Local counters (cgroup v1 compatibility - this cgroup only) */
	atomic64_t		state_local[MEMCG_VMSTAT_SIZE];
	atomic64_t		events_local[NR_MEMCG_EVENTS];
#endif
} ____cacheline_aligned_in_smp;

/* Lightweight cache for batch-read stats (threshold-based invalidation) */
struct memcg_atomic_cache {
	/* Hot fields: frequently accessed together */
	bool valid;		/* cache validity flag */
	seqlock_t stats_seqlock;	/* protects stats updates */
	seqlock_t events_seqlock;	/* protects events updates */

	/* Large data arrays: separate cache lines for better false sharing avoidance */
	u64 stats[MEMCG_VMSTAT_SIZE] ____cacheline_aligned_in_smp;
	unsigned long events[NR_MEMCG_EVENTS];
};

/* Core read functions */
u64 css_atomic_page_state(struct mem_cgroup *memcg, int idx, bool force);
unsigned long css_atomic_events(struct mem_cgroup *memcg,
				enum vm_event_item idx, bool force);
unsigned long css_atomic_events_recursive(struct mem_cgroup *memcg,
					  enum vm_event_item idx);

/* Flush operations (similar to rstat) */
void css_atomic_flush(struct mem_cgroup *memcg, bool force);

/* Batch operations */
int css_atomic_page_state_batch(struct mem_cgroup *memcg, u64 *results);
int css_atomic_events_batch(struct mem_cgroup *memcg, unsigned long *results);
int css_atomic_walk(struct mem_cgroup *memcg,
		    int (*visit)(struct mem_cgroup *, void *),
		    void *arg);

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
static inline int css_atomic_page_state_batch(struct mem_cgroup *memcg,
					      u64 *results)
{
	return 0;
}
static inline int css_atomic_events_batch(struct mem_cgroup *memcg,
					  unsigned long *results)
{
	return 0;
}
static inline int css_atomic_walk(struct mem_cgroup *memcg,
				  int (*visit)(struct mem_cgroup *, void *),
				  void *arg)
{
	return 0;
}

#endif /* CONFIG_MEMCG_ATOMIC_COUNTER */

#endif /* _LINUX_CGROUP_ATOMIC_H */

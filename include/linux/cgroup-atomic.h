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
 * Per-cgroup atomic counter based stats - HIERARCHICAL VERSION
 *
 * Each counter stores HIERARCHICAL values (self + all descendants).
 * Writes propagate up the tree to all ancestors, making reads O(1).
 *
 * Design:
 * - Write: Update this cgroup and all ancestors (O(depth) atomic ops)
 * - Read: Direct atomic64_read(), no tree traversal needed (O(1))
 * - No cache, no locks, simple and direct
 *
 * Trade-offs:
 * - Pros: Instant reads O(1), no cache overhead, simpler code
 * - Cons: Slower writes O(depth), root cgroup hot spot, cache line bouncing
 *
 * Best for: Read-heavy workloads with shallow hierarchies
 * Not for: Write-heavy workloads (10000:1 write:read ratio)
 */
struct memcg_atomic_counter {
	/* Hierarchical counters (self + all descendants) */
	atomic64_t		state[MEMCG_VMSTAT_SIZE];
	atomic64_t		events[NR_MEMCG_EVENTS];

#ifdef CONFIG_MEMCG_V1
	/* Local counters (cgroup v1 compatibility - this cgroup only) */
	atomic64_t		state_local[MEMCG_VMSTAT_SIZE];
	atomic64_t		events_local[NR_MEMCG_EVENTS];
#endif
} ____cacheline_aligned_in_smp;

/* Core read functions - simple O(1) direct reads */
u64 css_atomic_page_state(struct mem_cgroup *memcg, int idx, bool force);
unsigned long css_atomic_events(struct mem_cgroup *memcg,
				enum vm_event_item idx, bool force);
unsigned long css_atomic_events_recursive(struct mem_cgroup *memcg,
					  enum vm_event_item idx);

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

#endif /* CONFIG_MEMCG_ATOMIC_COUNTER */

#endif /* _LINUX_CGROUP_ATOMIC_H */

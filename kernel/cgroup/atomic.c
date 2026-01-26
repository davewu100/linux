// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Cgroup atomic counter implementation - HIERARCHICAL VERSION
 *
 * This file contains the atomic counter based statistics implementation
 * for cgroups. This is a simplified hierarchical version where counters
 * store pre-aggregated values (self + all descendants).
 *
 * Design:
 * - Write: Propagate up the tree to all ancestors O(depth)
 * - Read: Direct atomic read O(1), no tree traversal
 * - No cache, no seqlock, simple and direct
 *
 * Trade-off: Fast reads, slower writes, root cgroup hot spot.
 */

#include <linux/cgroup.h>
#include <linux/cgroup-atomic.h>
#include <linux/memcontrol.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>

#ifdef CONFIG_MEMCG_ATOMIC_COUNTER

/**
 * css_atomic_page_state - read atomic counter (HIERARCHICAL VERSION)
 * @memcg: the memory cgroup
 * @idx: the stat item (enum memcg_stat_item or node_stat_item)
 * @force: ignored (kept for API compatibility)
 *
 * Direct O(1) read. counter->state[idx] already contains hierarchical value
 * (self + all descendants), maintained by write-time propagation.
 *
 * Returns: hierarchical stat value
 */
u64 css_atomic_page_state(struct mem_cgroup *memcg, int idx, bool force)
{
	struct memcg_atomic_counter *counter;
	int i = memcg_stats_index(idx);

	if (WARN_ONCE(BAD_STAT_IDX(i), "%s: missing stat item %d\n", __func__, idx))
		return 0;

	/* BOOT SAFETY: Check if memcg is fully online */
	if (unlikely(!mem_cgroup_online(memcg)))
		return 0;

	/* Direct read: counter already contains hierarchical value */
	counter = READ_ONCE(memcg->atomic_counter);
	if (unlikely(!counter))
		return 0;

	/*
	 * Direct atomic read - no tree traversal needed!
	 * The counter contains sum of this cgroup + all descendants.
	 */
	return atomic64_read(&counter->state[i]);
}

/**
 * css_atomic_page_state_recursive - alias for hierarchical version
 * @memcg: the memory cgroup
 * @idx: the stat item
 *
 * In hierarchical version, this is identical to css_atomic_page_state()
 * because counters already store aggregated values.
 *
 * Returns: hierarchical stat value
 */
static u64 css_atomic_page_state_recursive(struct mem_cgroup *memcg, int idx)
{
	return css_atomic_page_state(memcg, idx, false);
}

/**
 * css_atomic_events - read atomic counter events (HIERARCHICAL VERSION)
 * @memcg: the memory cgroup
 * @idx: the event item
 * @force: ignored (kept for API compatibility)
 *
 * Direct O(1) read of event counters.
 *
 * Returns: hierarchical event count
 */
unsigned long css_atomic_events(struct mem_cgroup *memcg, enum vm_event_item idx,
				bool force)
{
	struct memcg_atomic_counter *counter;
	int i = memcg_events_index(idx);

	if (WARN_ONCE(BAD_STAT_IDX(i), "%s: missing event item %d\n", __func__, idx))
		return 0;

	/* BOOT SAFETY */
	if (unlikely(!mem_cgroup_online(memcg)))
		return 0;

	/* Direct read: counter already contains hierarchical value */
	counter = READ_ONCE(memcg->atomic_counter);
	if (unlikely(!counter))
		return 0;

	return (unsigned long)atomic64_read(&counter->events[i]);
}

/**
 * css_atomic_events_recursive - alias for hierarchical version
 * @memcg: the memory cgroup
 * @idx: the event item
 *
 * In hierarchical version, identical to css_atomic_events().
 *
 * Returns: hierarchical event count
 */
unsigned long css_atomic_events_recursive(struct mem_cgroup *memcg,
					  enum vm_event_item idx)
{
	return css_atomic_events(memcg, idx, false);
}

#endif /* CONFIG_MEMCG_ATOMIC_COUNTER */

// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Cgroup atomic counter implementation - RSTAT-LIKE DESIGN
 *
 * Similar to rstat: local counters + dirty tracking + 2s time window.
 *
 * Design:
 * - Write: Update local counter + mark dirty upward (propagate dirty flag)
 * - Read: Flush only if age >= 2s, otherwise use cache (even if dirty)
 * - Explicit flush: On memory.stat read or force=true
 *
 * Key difference from naive time-window:
 * - Dirty flag propagates upward (like rstat)
 * - But within 2s, we don't flush even if dirty!
 * - This amortizes flush cost across multiple writes
 *
 * Trade-off: Fast writes (atomic + dirty flag), minimal flushes (2s rate limit).
 */

#include <linux/cgroup.h>
#include <linux/cgroup-atomic.h>
#include <linux/memcontrol.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include <linux/jiffies.h>

#ifdef CONFIG_MEMCG_ATOMIC_COUNTER

/* Forward declarations */
static void css_atomic_refresh_cache(struct mem_cgroup *memcg);
static u64 css_atomic_aggregate_stats(struct mem_cgroup *memcg, int idx);
static unsigned long css_atomic_aggregate_events(struct mem_cgroup *memcg, int idx);

/**
 * css_atomic_page_state - read atomic counter with rstat-like caching
 * @memcg: the memory cgroup
 * @idx: the stat item (enum memcg_stat_item or node_stat_item)
 * @force: if true, force cache refresh regardless of TTL
 *
 * RSTAT-LIKE DESIGN:
 * - Flush only if age > 2s (even if dirty!)
 * - Within 2s window: return cache (even if dirty)
 * - This is the key difference from naive time-window approach
 *
 * Returns: hierarchical stat value (self + all descendants)
 */
u64 css_atomic_page_state(struct mem_cgroup *memcg, int idx, bool force)
{
	struct memcg_atomic_cache *cache;
	int i = memcg_stats_index(idx);
	unsigned long now, age;

	if (WARN_ONCE(BAD_STAT_IDX(i), "%s: missing stat item %d\n", __func__, idx))
		return 0;

	/* BOOT SAFETY: Check if memcg is fully online */
	if (unlikely(!mem_cgroup_online(memcg)))
		return 0;

	cache = memcg->atomic_cache;
	if (unlikely(!cache))
		return 0;

	now = jiffies;
	age = now - READ_ONCE(cache->flush_time);

	/*
	 * RSTAT-LIKE FLUSH LOGIC:
	 *
	 * Flush if:
	 *   1. force=true (explicit flush request), OR
	 *   2. age >= 2s (time window expired)
	 *
	 * Key insight: We DON'T flush just because dirty=true!
	 * Within 2s window, we tolerate stale data even if dirty.
	 * This amortizes flush cost across multiple writes.
	 */
	if (force || age >= ATOMIC_CACHE_TTL) {
		/* Time window expired or forced: refresh cache */
		css_atomic_refresh_cache(memcg);
	}
	/* else: Within 2s window, use cache (even if dirty) */

	return READ_ONCE(cache->stats[i]);
}

/**
 * css_atomic_events - read atomic counter events with rstat-like caching
 * @memcg: the memory cgroup
 * @idx: the event item
 * @force: if true, force cache refresh
 *
 * Same rstat-like caching logic as css_atomic_page_state().
 *
 * Returns: hierarchical event count
 */
unsigned long css_atomic_events(struct mem_cgroup *memcg, enum vm_event_item idx,
				bool force)
{
	struct memcg_atomic_cache *cache;
	int i = memcg_events_index(idx);
	unsigned long now, age;

	if (WARN_ONCE(BAD_STAT_IDX(i), "%s: missing event item %d\n", __func__, idx))
		return 0;

	/* BOOT SAFETY */
	if (unlikely(!mem_cgroup_online(memcg)))
		return 0;

	cache = memcg->atomic_cache;
	if (unlikely(!cache))
		return 0;

	now = jiffies;
	age = now - READ_ONCE(cache->flush_time);

	/* RSTAT-LIKE: flush only if age >= 2s or force=true */
	if (force || age >= ATOMIC_CACHE_TTL) {
		css_atomic_refresh_cache(memcg);
	}

	return READ_ONCE(cache->events[i]);
}

/**
 * css_atomic_events_recursive - alias for css_atomic_events
 * @memcg: the memory cgroup
 * @idx: the event item
 *
 * Returns: hierarchical event count
 */
unsigned long css_atomic_events_recursive(struct mem_cgroup *memcg,
					  enum vm_event_item idx)
{
	return css_atomic_events(memcg, idx, false);
}

/**
 * css_atomic_flush - explicitly refresh cache (called on memory.stat read)
 * @memcg: the memory cgroup
 *
 * This is called when userspace reads memory.stat or memory.numa_stat.
 * Forces immediate cache refresh to ensure fresh data.
 */
void css_atomic_flush(struct mem_cgroup *memcg)
{
	if (unlikely(!mem_cgroup_online(memcg)))
		return;

	css_atomic_refresh_cache(memcg);
}

/**
 * css_atomic_refresh_cache - internal function to refresh cache
 * @memcg: the memory cgroup
 *
 * Traverses the cgroup tree and aggregates all descendant counters.
 * Updates cache with aggregated values and marks as valid.
 */
static void css_atomic_refresh_cache(struct mem_cgroup *memcg)
{
	struct memcg_atomic_cache *cache = memcg->atomic_cache;
	int i;

	if (unlikely(!cache))
		return;

	/*
	 * Aggregate stats and events from entire subtree.
	 * This is O(N) where N is number of descendants.
	 */
	for (i = 0; i < MEMCG_VMSTAT_SIZE; i++) {
		cache->stats[i] = css_atomic_aggregate_stats(memcg, i);
	}

	for (i = 0; i < NR_MEMCG_EVENTS; i++) {
		cache->events[i] = css_atomic_aggregate_events(memcg, i);
	}

	/*
	 * Clear dirty flag and update flush time (rstat-like).
	 * Cache is now fresh until next write or 2s expiry.
	 * Use WRITE_ONCE to ensure visibility to readers.
	 */
	WRITE_ONCE(cache->flush_time, jiffies);
	WRITE_ONCE(cache->dirty, false);
}

/**
 * css_atomic_aggregate_stats - aggregate stat from entire subtree
 * @memcg: the memory cgroup
 * @idx: the stat item index
 *
 * Traverses tree using mem_cgroup_iter() and sums up local counters.
 *
 * Returns: aggregated stat value
 */
static u64 css_atomic_aggregate_stats(struct mem_cgroup *memcg, int idx)
{
	struct mem_cgroup *iter;
	struct memcg_atomic_counter *counter;
	u64 total = 0;

	/*
	 * Use mem_cgroup_iter() for iterative tree traversal.
	 * RCU read lock is held internally by mem_cgroup_iter().
	 */
	for (iter = memcg; iter; iter = mem_cgroup_iter(memcg, iter, NULL)) {
		/* Skip non-online memcgs */
		if (!mem_cgroup_online(iter))
			continue;

		counter = READ_ONCE(iter->atomic_counter);
		if (unlikely(!counter))
			continue;

		/* Add local counter value (not hierarchical) */
		total += atomic64_read(&counter->state[idx]);
	}

	return total;
}

/**
 * css_atomic_aggregate_events - aggregate events from entire subtree
 * @memcg: the memory cgroup
 * @idx: the event item index
 *
 * Same as css_atomic_aggregate_stats but for events.
 *
 * Returns: aggregated event count
 */
static unsigned long css_atomic_aggregate_events(struct mem_cgroup *memcg, int idx)
{
	struct mem_cgroup *iter;
	struct memcg_atomic_counter *counter;
	unsigned long total = 0;

	for (iter = memcg; iter; iter = mem_cgroup_iter(memcg, iter, NULL)) {
		if (!mem_cgroup_online(iter))
			continue;

		counter = READ_ONCE(iter->atomic_counter);
		if (unlikely(!counter))
			continue;

		total += (unsigned long)atomic64_read(&counter->events[idx]);
	}

	return total;
}

#endif /* CONFIG_MEMCG_ATOMIC_COUNTER */

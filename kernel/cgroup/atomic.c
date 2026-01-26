// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Cgroup atomic counter implementation - RSTAT-LIKE DESIGN
 *
 * Exactly mimics rstat's flush logic: threshold-based + rate limited.
 *
 * Design (mirrors rstat):
 * - Write: Update local counter + increment stats_updates upward
 * - Read: Directly read cache (NO automatic flush, like rstat's READ_ONCE)
 * - Flush: Only when stats_updates > threshold (like rstat)
 * - Rate limit: 2s minimum between flushes (like rstat's FLUSH_TIME)
 *
 * Key insight:
 * - Internal reads NEVER trigger flush (just like rstat)
 * - Flush only happens via explicit css_atomic_flush() calls
 * - css_atomic_flush() checks threshold + rate limit before flushing
 *
 * This matches rstat's behavior where memcg_page_state() just reads
 * vmstats without flushing, and flush is triggered separately.
 *
 * Trade-off: Very fast reads (O(1) always), writes track updates,
 * flush amortized across many updates via threshold.
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
 * @force: if true, force cache refresh
 *
 * RSTAT-LIKE DESIGN (EXACTLY LIKE RSTAT):
 * - Normal read: directly return cached value, NO flush check!
 * - Force read: flush first, then return
 * - This is exactly how rstat works: memcg_page_state() directly reads vmstats
 *
 * Flush is ONLY triggered by:
 * - Explicit css_atomic_flush() calls (e.g., from memory.stat read)
 * - Those check threshold and rate limit
 *
 * Returns: hierarchical stat value (self + all descendants)
 */
u64 css_atomic_page_state(struct mem_cgroup *memcg, int idx, bool force)
{
	struct memcg_atomic_cache *cache;
	int i = memcg_stats_index(idx);

	if (WARN_ONCE(BAD_STAT_IDX(i), "%s: missing stat item %d\n", __func__, idx))
		return 0;

	/* BOOT SAFETY: Check if memcg is fully online */
	if (unlikely(!mem_cgroup_online(memcg)))
		return 0;

	cache = memcg->atomic_cache;
	if (unlikely(!cache))
		return 0;

	/*
	 * RSTAT-LIKE READ:
	 * - force=false: directly return cache, like rstat's READ_ONCE(vmstats->state[i])
	 * - force=true: flush first, then return
	 *
	 * NO automatic flush based on time or threshold here!
	 * That's handled by explicit css_atomic_flush() calls.
	 */
	if (force)
		css_atomic_refresh_cache(memcg);

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

	if (WARN_ONCE(BAD_STAT_IDX(i), "%s: missing event item %d\n", __func__, idx))
		return 0;

	/* BOOT SAFETY */
	if (unlikely(!mem_cgroup_online(memcg)))
		return 0;

	cache = memcg->atomic_cache;
	if (unlikely(!cache))
		return 0;

	/* RSTAT-LIKE: directly read cache, no automatic flush */
	if (force)
		css_atomic_refresh_cache(memcg);

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
 * css_atomic_flush - flush cache if needed (rstat-like threshold + rate limit)
 * @memcg: the memory cgroup
 * @force: if true, bypass threshold and rate limit checks
 *
 * RSTAT-LIKE FLUSH LOGIC (same as __mem_cgroup_flush_stats):
 * - force=false: Check threshold + rate limit before flushing
 *   1. Check threshold: stats_updates > THRESHOLD * num_online_cpus()
 *   2. Check rate limit: last flush > 2s ago
 *   3. Only flush if both conditions met
 * - force=true: Flush immediately, skip all checks
 *
 * This is called:
 * - From memory.stat read (force=false): normal flush with checks
 * - From OOM/comparison (force=true): immediate flush
 *
 * Similar to rstat's __mem_cgroup_flush_stats(memcg, force).
 */
void css_atomic_flush(struct mem_cgroup *memcg, bool force)
{
	struct memcg_atomic_cache *cache;
	unsigned long now;
	int threshold;
	int updates;

	if (unlikely(!mem_cgroup_online(memcg)))
		return;

	cache = memcg->atomic_cache;
	if (unlikely(!cache))
		return;

	/* Force flush: skip all checks */
	if (force) {
		css_atomic_refresh_cache(memcg);
		return;
	}

	/*
	 * Normal flush: check threshold and rate limit.
	 *
	 * Calculate threshold like rstat:
	 * MEMCG_CHARGE_BATCH * num_online_cpus()
	 *
	 * We use ATOMIC_FLUSH_THRESHOLD (64) as our base, similar to
	 * rstat's MEMCG_CHARGE_BATCH.
	 */
	threshold = ATOMIC_FLUSH_THRESHOLD * num_online_cpus();
	updates = atomic_read(&cache->stats_updates);

	/* Fast path: no updates to flush */
	if (updates == 0)
		return;

	/* Threshold check: only flush if exceeded (like rstat) */
	if (updates < threshold)
		return;

	/*
	 * Rate limit: avoid flushing too frequently.
	 * Even if threshold is exceeded, skip if we flushed recently.
	 * This is optional but helps reduce flush storms.
	 */
	now = jiffies;
	if (time_before(now, READ_ONCE(cache->flush_time) + ATOMIC_FLUSH_TIME))
		return;  /* Too soon since last flush */

	/* Threshold exceeded and rate limit passed: do the flush */
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
	 * Reset update counter and record flush time (rstat-like).
	 * Cache is now fresh until enough updates accumulate.
	 * Use WRITE_ONCE to ensure visibility to readers.
	 */
	WRITE_ONCE(cache->flush_time, jiffies);
	atomic_set(&cache->stats_updates, 0);
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

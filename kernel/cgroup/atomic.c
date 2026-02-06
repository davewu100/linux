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

/*
 * Note: css_atomic_page_state() and css_atomic_events() have been removed.
 * All callers now use the inlined memcg_atomic_read_state_cached() and
 * memcg_atomic_read_events_cached() functions in memcontrol.c for better
 * performance (avoiding cross-module calls).
 */

/**
 * css_atomic_flush - flush cache if needed (EXACTLY like rstat)
 * @memcg: the memory cgroup
 * @force: if true, bypass threshold check
 *
 * EXACTLY LIKE RSTAT's __mem_cgroup_flush_stats():
 * - force=false: ONLY check threshold (NO time check!)
 *   1. Check threshold: stats_updates > THRESHOLD * num_online_cpus()
 *   2. If exceeded → flush immediately
 * - force=true: Flush immediately, skip all checks
 *
 * This is called:
 * - From memory.stat read (force=false): threshold check only
 * - From OOM/comparison (force=true): immediate flush
 *
 * Rate limiting is ONLY in css_atomic_flush_ratelimited(), not here!
 * This matches rstat's mem_cgroup_flush_stats() behavior exactly.
 */
void css_atomic_flush(struct mem_cgroup *memcg, bool force)
{
	struct memcg_atomic_cache *cache;
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
	 * Normal flush: ONLY check threshold (exactly like rstat).
	 *
	 * Calculate threshold like rstat:
	 * MEMCG_CHARGE_BATCH * num_online_cpus()
	 *
	 * We use ATOMIC_FLUSH_THRESHOLD (64) as our base, similar to
	 * rstat's MEMCG_CHARGE_BATCH.
	 *
	 * NO rate limit here! That's only in css_atomic_flush_ratelimited().
	 * This matches rstat's mem_cgroup_flush_stats() which doesn't check time.
	 */
	threshold = ATOMIC_FLUSH_THRESHOLD * num_online_cpus();
	updates = atomic_read(&cache->stats_updates);

	/* Fast path: no updates to flush */
	if (updates == 0)
		return;

	/* Threshold check: only flush if exceeded (exactly like rstat) */
	if (updates < threshold)
		return;

	/* Threshold exceeded: flush immediately (like rstat) */
	css_atomic_refresh_cache(memcg);
}

/**
 * css_atomic_flush_ratelimited - rate-limited flush (stricter than normal)
 * @memcg: the memory cgroup
 *
 * More aggressive rate limiting than css_atomic_flush().
 * Only flush if last flush was > 4s ago (2 * FLUSH_TIME).
 *
 * This is used in hot paths where we want stats but can tolerate
 * more staleness. Matches rstat's mem_cgroup_flush_stats_ratelimited().
 */
void css_atomic_flush_ratelimited(struct mem_cgroup *memcg)
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

	/*
	 * Check threshold first (like normal flush)
	 */
	threshold = ATOMIC_FLUSH_THRESHOLD * num_online_cpus();
	updates = atomic_read(&cache->stats_updates);

	if (updates == 0 || updates < threshold)
		return;

	/*
	 * Stricter rate limit: only flush if > 4s (2 * FLUSH_TIME)
	 * This is more conservative than normal flush (2s).
	 */
	now = jiffies;
	if (time_before(now, READ_ONCE(cache->flush_time) + 2 * ATOMIC_FLUSH_TIME))
		return;  /* Too soon, need to wait longer */

	/* Both threshold and stricter rate limit passed: flush */
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

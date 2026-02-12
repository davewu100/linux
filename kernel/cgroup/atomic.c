// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Cgroup atomic counter: rstat-like (threshold flush, cache read).
 * Flush only via css_atomic_flush(); reads never trigger flush.
 *
 * Trade-off: Very fast reads (O(1)), flush amortized via threshold.
 */

#include <linux/cgroup.h>
#include <linux/cgroup-atomic.h>
#include <linux/memcontrol.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include <linux/jiffies.h>

#ifdef CONFIG_MEMCG_ATOMIC_COUNTER

/* Forward declaration */
static void css_atomic_refresh_cache(struct mem_cgroup *memcg);

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
	struct memcg_atomic_aggregated *agg;
	int i = memcg_stats_index(idx);

	if (WARN_ONCE(BAD_STAT_IDX(i), "%s: missing stat item %d\n", __func__, idx))
		return 0;

	/* BOOT SAFETY: Check if memcg is fully online */
	if (unlikely(!mem_cgroup_online(memcg)))
		return 0;

	agg = memcg->atomic_aggregated;
	if (unlikely(!agg))
		return 0;

	/*
	 * RSTAT-LIKE READ:
	 * - force=false: directly return aggregated, like rstat's READ_ONCE(vmstats->state[i])
	 * - force=true: flush first, then return
	 *
	 * NO automatic flush based on time or threshold here!
	 * That's handled by explicit css_atomic_flush() calls.
	 */
	if (force)
		css_atomic_refresh_cache(memcg);

	return READ_ONCE(agg->stats[i]);
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
	struct memcg_atomic_aggregated *agg;
	int i = memcg_events_index(idx);

	if (WARN_ONCE(BAD_STAT_IDX(i), "%s: missing event item %d\n", __func__, idx))
		return 0;

	/* BOOT SAFETY */
	if (unlikely(!mem_cgroup_online(memcg)))
		return 0;

	agg = memcg->atomic_aggregated;
	if (unlikely(!agg))
		return 0;

	/* RSTAT-LIKE: directly read aggregated, no automatic flush */
	if (force)
		css_atomic_refresh_cache(memcg);

	return READ_ONCE(agg->events[i]);
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
	struct memcg_atomic_aggregated *agg;
	int threshold;
	int updates;

	if (unlikely(!mem_cgroup_online(memcg)))
		return;

	agg = memcg->atomic_aggregated;
	if (unlikely(!agg))
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
	updates = atomic_read(&agg->stats_updates);

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
	struct memcg_atomic_aggregated *agg;
	unsigned long now;
	int threshold;
	int updates;

	if (unlikely(!mem_cgroup_online(memcg)))
		return;

	agg = memcg->atomic_aggregated;
	if (unlikely(!agg))
		return;

	/*
	 * Check threshold first (like normal flush)
	 */
	threshold = ATOMIC_FLUSH_THRESHOLD * num_online_cpus();
	updates = atomic_read(&agg->stats_updates);

	if (updates == 0 || updates < threshold)
		return;

	/*
	 * Stricter rate limit: only flush if > 4s (2 * FLUSH_TIME)
	 * This is more conservative than normal flush (2s).
	 */
	now = jiffies;
	if (time_before(now, READ_ONCE(agg->flush_time) + 2 * ATOMIC_FLUSH_TIME))
		return;  /* Too soon, need to wait longer */

	/* Both threshold and stricter rate limit passed: flush */
	css_atomic_refresh_cache(memcg);
}

/**
 * css_atomic_refresh_cache - refresh cache with one tree walk
 * @memcg: the memory cgroup
 *
 * Single traversal: for each descendant, add its state[] and events[]
 * into cache. O(descendants) instead of O(descendants * (stats + events)).
 */
static void css_atomic_refresh_cache(struct mem_cgroup *memcg)
{
	struct memcg_atomic_aggregated *agg = memcg->atomic_aggregated;
	struct mem_cgroup *iter;
	struct memcg_atomic_local *local;
	int j;

	if (unlikely(!agg))
		return;

	memset(agg->stats, 0, sizeof(agg->stats));
	memset(agg->events, 0, sizeof(agg->events));

	for (iter = memcg; iter; iter = mem_cgroup_iter(memcg, iter, NULL)) {
		if (!mem_cgroup_online(iter))
			continue;

		local = READ_ONCE(iter->atomic_local);
		if (unlikely(!local))
			continue;

		for (j = 0; j < MEMCG_VMSTAT_SIZE; j++)
			agg->stats[j] += atomic64_read(&local->state[j]);
		for (j = 0; j < NR_MEMCG_EVENTS; j++)
			agg->events[j] += (unsigned long)atomic64_read(&local->events[j]);
	}

	WRITE_ONCE(agg->flush_time, jiffies);
	atomic_set(&agg->stats_updates, 0);
}

#endif /* CONFIG_MEMCG_ATOMIC_COUNTER */

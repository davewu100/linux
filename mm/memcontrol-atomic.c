// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Memory cgroup atomic counter implementation
 *
 * This file contains all atomic counter logic: updates, flush, cache, and
 * aggregation. All code is in mm/ directory for better inline optimization
 * when called from memcontrol.c (same compilation unit).
 */

#include <linux/memcontrol.h>
#include <linux/memcontrol-atomic.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include <linux/jiffies.h>
#include <linux/cgroup-atomic.h>

#ifdef CONFIG_MEMCG_ATOMIC_COUNTER

/**
 * memcg_atomic_mod_state - update atomic counter state (rstat-like)
 * @memcg: the memory cgroup
 * @idx: the stat item index (already converted)
 * @val: delta to add to the counter
 *
 * RSTAT-LIKE UPDATE:
 * 1. Update local counter (atomic, per-cgroup)
 * 2. Increment stats_updates upward (like rstat's update counter propagation)
 *
 * Key: Only update counter propagates, not the value itself!
 * Flush only happens when updates exceed threshold.
 */
void memcg_atomic_mod_state(struct mem_cgroup *memcg, int idx, int val)
{
	struct memcg_atomic_counter *counter = READ_ONCE(memcg->atomic_counter);
	struct mem_cgroup *iter;

	/* Safety: counter might be NULL during initialization/cleanup */
	if (unlikely(!counter))
		return;

	/* Update local counter (atomic, fast) */
	atomic64_add(val, &counter->state[idx]);

#ifdef CONFIG_MEMCG_V1
	atomic64_add(val, &counter->state_local[idx]);
#endif

	/*
	 * Propagate update counter upward (rstat-like).
	 * This is used to decide when to flush, similar to rstat's
	 * stats_updates mechanism.
	 */
	for (iter = memcg; iter; iter = parent_mem_cgroup(iter)) {
		if (iter->atomic_cache)
			atomic_inc(&iter->atomic_cache->stats_updates);
	}
}

/**
 * memcg_atomic_mod_lruvec_state - update atomic counter lruvec state (rstat-like)
 * @memcg: the memory cgroup
 * @pn: the per-node structure
 * @idx: the stat item index (already converted)
 * @val: delta to add to the counter
 */
void memcg_atomic_mod_lruvec_state(struct mem_cgroup *memcg,
				 struct mem_cgroup_per_node *pn,
				 int idx, int val)
{
	struct memcg_atomic_counter *counter = READ_ONCE(memcg->atomic_counter);
	struct memcg_atomic_counter_per_node *node_counter =
		READ_ONCE(pn->atomic_counter_per_node);
	struct mem_cgroup *iter;

	/* Safety check */
	if (unlikely(!counter || !node_counter))
		return;

	/* Update local counter */
	atomic64_add(val, &counter->state[idx]);

#ifdef CONFIG_MEMCG_V1
	atomic64_add(val, &counter->state_local[idx]);
#endif

	/* Update per-node counter */
	atomic64_add(val, &node_counter->state[idx]);

	/* Propagate update counter upward (rstat-like) */
	for (iter = memcg; iter; iter = parent_mem_cgroup(iter)) {
		if (iter->atomic_cache)
			atomic_inc(&iter->atomic_cache->stats_updates);
	}
}

/**
 * memcg_atomic_count_events - update atomic event counter (rstat-like)
 * @memcg: the memory cgroup
 * @idx: the event item index (already converted)
 * @count: the number of events that occurred
 */
void memcg_atomic_count_events(struct mem_cgroup *memcg, int idx,
			     unsigned long count)
{
	struct memcg_atomic_counter *counter = READ_ONCE(memcg->atomic_counter);
	struct mem_cgroup *iter;

	/* Safety check */
	if (unlikely(!counter))
		return;

	/* Update local counter */
	atomic64_add(count, &counter->events[idx]);

#ifdef CONFIG_MEMCG_V1
	atomic64_add(count, &counter->events_local[idx]);
#endif

	/* Propagate update counter upward (rstat-like) */
	for (iter = memcg; iter; iter = parent_mem_cgroup(iter)) {
		if (iter->atomic_cache)
			atomic_inc(&iter->atomic_cache->stats_updates);
	}
}

/**
 * memcg_atomic_transfer_to_parent - transfer child stats to parent on offline
 * @memcg: the memory cgroup being offlined
 */
void memcg_atomic_transfer_to_parent(struct mem_cgroup *memcg)
{
	struct mem_cgroup *parent = parent_mem_cgroup(memcg);
	struct memcg_atomic_counter *child_counter;
	struct memcg_atomic_counter *parent_counter;
	int i;

	if (unlikely(!parent))
		return;

	child_counter = READ_ONCE(memcg->atomic_counter);
	parent_counter = READ_ONCE(parent->atomic_counter);
	if (unlikely(!child_counter || !parent_counter))
		return;

	for (i = 0; i < MEMCG_VMSTAT_SIZE; i++) {
		s64 delta = atomic64_xchg(&child_counter->state[i], 0);

		if (delta)
			atomic64_add(delta, &parent_counter->state[i]);
	}

	for (i = 0; i < NR_MEMCG_EVENTS; i++) {
		s64 delta = atomic64_xchg(&child_counter->events[i], 0);

		if (delta)
			atomic64_add(delta, &parent_counter->events[i]);
	}

	for_each_node_state(i, N_MEMORY) {
		struct mem_cgroup_per_node *pn = memcg->nodeinfo[i];
		struct mem_cgroup_per_node *ppn = parent->nodeinfo[i];
		struct memcg_atomic_counter_per_node *child_node_counter;
		struct memcg_atomic_counter_per_node *parent_node_counter;
		int j;

		if (unlikely(!pn || !ppn))
			continue;

		child_node_counter = READ_ONCE(pn->atomic_counter_per_node);
		parent_node_counter = READ_ONCE(ppn->atomic_counter_per_node);
		if (unlikely(!child_node_counter || !parent_node_counter))
			continue;

		for (j = 0; j < NR_VM_NODE_STAT_ITEMS; j++) {
			s64 delta = atomic64_xchg(&child_node_counter->state[j], 0);

			if (delta)
				atomic64_add(delta, &parent_node_counter->state[j]);
		}
	}
}

/**
 * memcg_atomic_lruvec_page_state - read per-node atomic counter stat
 * @lruvec: the lruvec
 * @idx: the stat item
 *
 * Returns the per-node atomic counter value for the given stat item.
 * Similar to lruvec_page_state but reads from atomic counter instead of rstat.
 */
unsigned long memcg_atomic_lruvec_page_state(struct lruvec *lruvec,
					   enum node_stat_item idx)
{
	struct mem_cgroup_per_node *pn;
	u64 x;
	int i;

	if (mem_cgroup_disabled())
		return node_page_state(lruvec_pgdat(lruvec), idx);

	i = memcg_stats_index(idx);
	if (WARN_ONCE(BAD_STAT_IDX(i), "%s: missing stat item %d\n", __func__, idx))
		return 0;

	pn = container_of(lruvec, struct mem_cgroup_per_node, lruvec);
	if (unlikely(!pn->atomic_counter_per_node))
		return 0;

	x = atomic64_read(&pn->atomic_counter_per_node->state[i]);
	return x;
}
EXPORT_SYMBOL(memcg_atomic_lruvec_page_state);

/**
 * memcg_atomic_init - allocate atomic counter structures
 * @memcg: the memory cgroup
 *
 * Returns: 0 on success, -ENOMEM on failure
 */
int memcg_atomic_init(struct mem_cgroup *memcg)
{
	/* Allocate per-cgroup atomic counter (stores local values) */
	memcg->atomic_counter = kzalloc(sizeof(struct memcg_atomic_counter),
					GFP_KERNEL_ACCOUNT);
	if (unlikely(!memcg->atomic_counter))
		return -ENOMEM;

	/* Initialize children list for tree traversal */
	INIT_LIST_HEAD(&memcg->atomic_children);
	INIT_LIST_HEAD(&memcg->atomic_sibling);
	spin_lock_init(&memcg->atomic_children_lock);

	/* Allocate time-based cache (rstat-like design) */
	memcg->atomic_cache = kzalloc(sizeof(struct memcg_atomic_cache),
				      GFP_KERNEL_ACCOUNT);
	if (unlikely(!memcg->atomic_cache)) {
		kfree(memcg->atomic_counter);
		memcg->atomic_counter = NULL;
		return -ENOMEM;
	}

	/* Initialize cache (rstat-like: update counter + flush time) */
	atomic_set(&memcg->atomic_cache->stats_updates, 0);
	memcg->atomic_cache->flush_time = 0;

	return 0;
}

/**
 * memcg_atomic_exit - free atomic counter structures
 * @memcg: the memory cgroup
 */
void memcg_atomic_exit(struct mem_cgroup *memcg)
{
	/*
	 * Free atomic counter and cache.
	 * RCU synchronization handled by caller.
	 */
	kfree(memcg->atomic_counter);
	kfree(memcg->atomic_cache);
}

/**
 * memcg_atomic_init_per_node - allocate per-node atomic counter
 * @pn: the per-node structure
 * @node: the node ID
 *
 * Returns: 0 on success, -ENOMEM on failure
 */
int memcg_atomic_init_per_node(struct mem_cgroup_per_node *pn, int node)
{
	pn->atomic_counter_per_node = kzalloc_node(
		sizeof(struct memcg_atomic_counter_per_node),
		GFP_KERNEL_ACCOUNT, node);
	if (!pn->atomic_counter_per_node)
		return -ENOMEM;

	return 0;
}

/**
 * memcg_atomic_exit_per_node - free per-node atomic counter
 * @pn: the per-node structure
 */
void memcg_atomic_exit_per_node(struct mem_cgroup_per_node *pn)
{
	kfree(pn->atomic_counter_per_node);
}

/**
 * memcg_atomic_online - handle memcg online event
 * @memcg: the memory cgroup
 *
 * Returns: 0 on success, error code on failure
 */
int memcg_atomic_online(struct mem_cgroup *memcg)
{
	/* Add to parent's atomic counter children list (RCU protected) */
	if (!mem_cgroup_is_root(memcg)) {
		struct mem_cgroup *parent = parent_mem_cgroup(memcg);

		spin_lock(&parent->atomic_children_lock);
		list_add_rcu(&memcg->atomic_sibling, &parent->atomic_children);
		spin_unlock(&parent->atomic_children_lock);
	}

	return 0;
}

/**
 * memcg_atomic_offline - handle memcg offline event
 * @memcg: the memory cgroup
 */
void memcg_atomic_offline(struct mem_cgroup *memcg)
{
	/*
	 * Remove from parent's atomic counter children list (RCU protected).
	 *
	 * RCU synchronization and memory safety:
	 * - list_del_rcu() marks the node as deleted but keeps it accessible
	 *   to concurrent RCU readers during their grace period
	 * - Concurrent readers in memcg_atomic_aggregate_stats() and
	 *   memcg_atomic_aggregate_events() (via memcg_atomic_refresh_cache)
	 *   use mem_cgroup_iter() which holds RCU read lock internally
	 * - The cgroup framework guarantees that css_free (which calls
	 *   __mem_cgroup_free) is invoked AFTER an RCU grace period,
	 *   ensuring all RCU readers have completed before memcg structure
	 *   and atomic_counter are freed
	 * - This ordering is: css_offline -> list_del_rcu -> RCU grace period
	 *   -> css_free -> __mem_cgroup_free -> kfree(memcg->atomic_counter)
	 *
	 * Therefore, no explicit synchronize_rcu() or call_rcu() is needed here.
	 */
	if (!mem_cgroup_is_root(memcg)) {
		memcg_atomic_transfer_to_parent(memcg);

		struct mem_cgroup *parent = parent_mem_cgroup(memcg);

		spin_lock(&parent->atomic_children_lock);
		list_del_rcu(&memcg->atomic_sibling);
		spin_unlock(&parent->atomic_children_lock);
	}
}

/* Forward declarations for flush/aggregate functions */
static void memcg_atomic_refresh_cache(struct mem_cgroup *memcg);
static u64 memcg_atomic_aggregate_stats(struct mem_cgroup *memcg, int idx);
static unsigned long memcg_atomic_aggregate_events(struct mem_cgroup *memcg, int idx);

/**
 * memcg_atomic_flush - flush cache if needed (EXACTLY like rstat)
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
 * Rate limiting is ONLY in memcg_atomic_flush_ratelimited(), not here!
 * This matches rstat's mem_cgroup_flush_stats() behavior exactly.
 */
void memcg_atomic_flush(struct mem_cgroup *memcg, bool force)
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
		memcg_atomic_refresh_cache(memcg);
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
	 * NO rate limit here! That's only in memcg_atomic_flush_ratelimited().
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
	memcg_atomic_refresh_cache(memcg);
}

/**
 * memcg_atomic_flush_ratelimited - rate-limited flush (stricter than normal)
 * @memcg: the memory cgroup
 *
 * More aggressive rate limiting than memcg_atomic_flush().
 * Only flush if last flush was > 4s ago (2 * FLUSH_TIME).
 *
 * This is used in hot paths where we want stats but can tolerate
 * more staleness. Matches rstat's mem_cgroup_flush_stats_ratelimited().
 */
void memcg_atomic_flush_ratelimited(struct mem_cgroup *memcg)
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
	memcg_atomic_refresh_cache(memcg);
}

/**
 * memcg_atomic_refresh_cache - internal function to refresh cache
 * @memcg: the memory cgroup
 *
 * Traverses the cgroup tree and aggregates all descendant counters.
 * Updates cache with aggregated values and marks as valid.
 */
static void memcg_atomic_refresh_cache(struct mem_cgroup *memcg)
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
		cache->stats[i] = memcg_atomic_aggregate_stats(memcg, i);
	}

	for (i = 0; i < NR_MEMCG_EVENTS; i++) {
		cache->events[i] = memcg_atomic_aggregate_events(memcg, i);
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
 * memcg_atomic_aggregate_stats - aggregate stat from entire subtree
 * @memcg: the memory cgroup
 * @idx: the stat item index
 *
 * Traverses tree using mem_cgroup_iter() and sums up local counters.
 *
 * Returns: aggregated stat value
 */
static u64 memcg_atomic_aggregate_stats(struct mem_cgroup *memcg, int idx)
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
 * memcg_atomic_aggregate_events - aggregate events from entire subtree
 * @memcg: the memory cgroup
 * @idx: the event item index
 *
 * Same as memcg_atomic_aggregate_stats but for events.
 *
 * Returns: aggregated event count
 */
static unsigned long memcg_atomic_aggregate_events(struct mem_cgroup *memcg, int idx)
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

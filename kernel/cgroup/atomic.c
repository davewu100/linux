// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Cgroup atomic counter implementation
 *
 * This file contains the atomic counter based statistics implementation
 * for cgroups, providing an alternative to the rstat-based approach.
 *
 * Similar to rstat (kernel/cgroup/rstat.c), this provides a general-purpose
 * statistics backend for cgroup subsystems. Currently used by memcg.
 */

#include <linux/cgroup.h>
#include <linux/cgroup-atomic.h>
#include <linux/memcontrol.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include <linux/seq_file.h>
#include <linux/limits.h>

#ifdef CONFIG_MEMCG_ATOMIC_COUNTER

/*
 * Time tracking for rate-limited flushing (similar to rstat's flush_last_time)
 * Updated on every flush to track when the last cache update occurred.
 */
static u64 atomic_flush_last_time;

/*
 * Flush interval aligned with rstat's FLUSH_TIME (2 seconds)
 * Rate-limited flush only triggers if one full cycle late (2*FLUSH_TIME)
 */
#define ATOMIC_FLUSH_TIME (2UL*HZ)

/**
 * Helper macro to read value from cache with seqlock protection
 * Generates inline functions for reading stats and events from cache
 *
 * @name: function name suffix (stats or events)
 * @type: value type (u64 or unsigned long)
 * @field: cache array field name (stats or events)
 * @seqlock: seqlock field name (stats_seqlock or events_seqlock)
 */
#define DEFINE_CACHE_READER(name, type, field, seqlock, updates_field)	\
static inline bool css_atomic_read_##name##_from_cache(		\
		struct memcg_atomic_cache *cache,			\
		atomic_t *updates_counter,				\
		int idx, type *value)					\
{									\
	unsigned int seq;						\
									\
	if (unlikely(!cache) || !READ_ONCE(cache->valid))		\
		return false;						\
									\
	/* Check threshold only if cache is valid (avoid unnecessary atomic read) */ \
	if (unlikely(atomic_read(updates_counter) >			\
		     get_atomic_flush_threshold()))			\
		return false;						\
									\
	do {								\
		seq = read_seqbegin(&cache->seqlock);			\
		*value = cache->field[idx];				\
	} while (read_seqretry(&cache->seqlock, seq));			\
									\
	return true;							\
}

/* Forward declarations (must be before DEFINE_CACHE_READER expansion) */
static inline unsigned int get_atomic_flush_threshold(void);

/* Generate cache reader functions */
DEFINE_CACHE_READER(stats, u64, stats, stats_seqlock, atomic_stats_updates)
DEFINE_CACHE_READER(events, unsigned long, events, events_seqlock, atomic_events_updates)

/* More forward declarations */
static int __css_atomic_walk_locked(struct mem_cgroup *memcg,
				    int (*visit)(struct mem_cgroup *, void *),
				    void *arg);
static u64 css_atomic_page_state_recursive(struct mem_cgroup *memcg, int idx);
static bool css_atomic_stats_need_flush(struct mem_cgroup *memcg);
static bool css_atomic_events_need_flush(struct mem_cgroup *memcg);
static int css_atomic_recompute_and_cache_stats(struct mem_cgroup *memcg);
static int css_atomic_recompute_and_cache_events(struct mem_cgroup *memcg);


/**
 * css_atomic_page_state - read atomic counter with cache support
 * @memcg: the memory cgroup
 * @idx: the stat item (enum memcg_stat_item or node_stat_item)
 * @force: if true, bypass cache and force recompute (for critical paths)
 *
 * This is the main interface for reading atomic counter stats, with behavior
 * similar to memcg_page_state() for rstat:
 * - Normal case (force=false): Uses threshold-based cache, O(1) when valid
 * - Forced case (force=true): Always recomputes by tree traversal, O(descendants)
 *
 * Returns: stat value
 */
u64 css_atomic_page_state(struct mem_cgroup *memcg, int idx, bool force)
{
	u64 value;
	int i = memcg_stats_index(idx);
	bool flushed = false;
	struct memcg_atomic_counter *counter;

	if (WARN_ONCE(BAD_STAT_IDX(i), "%s: missing stat item %d\n", __func__, idx))
		return 0;

	/*
	 * BOOT SAFETY: Check if memcg is fully online before allowing
	 * tree traversal. During early boot (css_alloc/css_online phase),
	 * the cgroup tree is not fully initialized and tree traversal
	 * via mem_cgroup_iter() could cause infinite loops or deadlocks.
	 *
	 * For non-online memcgs, return local counter only (no hierarchy).
	 * This is safe because:
	 * - Before online, no children exist yet
	 * - Local accounting is already active
	 * - Hierarchical reads can wait until online completes
	 */
	if (!mem_cgroup_online(memcg)) {
		counter = READ_ONCE(memcg->atomic_counter);
		if (counter && i >= 0)
			return atomic64_read(&counter->state[i]);
		return 0;
	}

	/* Fast path: try to read from cache without flushing */
	/* Threshold check is now done inside cache read function */
	if (!force && memcg->atomic_cache) {
		if (css_atomic_read_stats_from_cache(memcg->atomic_cache,
						     &memcg->atomic_stats_updates,
						     i, &value))
			return value;
	}

	/*
	 * Slow path: need to flush and update cache
	 * - force=true: explicitly requested
	 * - threshold exceeded: updates accumulated
	 * - cache miss: cache not yet initialized
	 */
	/* Force flush if cache is not initialized */
	if (!memcg->atomic_cache || !memcg->atomic_cache->valid)
		force = true;
	css_atomic_flush(memcg, force);
	flushed = true;

	/* Try to read from updated cache */
	if (css_atomic_read_stats_from_cache(memcg->atomic_cache,
					     &memcg->atomic_stats_updates,
					     i, &value))
		return value;

	/*
	 * Ultimate fallback: cache still invalid after flush.
	 * Possible causes:
	 * - Cache allocation failed during initialization
	 * - Extremely high update rate: cache invalidated again between
	 *   flush and read (concurrent writes exceeded threshold)
	 * This forces direct tree traversal (slower but correct).
	 */
	if (flushed && memcg->css.cgroup) {
		char name[NAME_MAX];
		cgroup_name(memcg->css.cgroup, name, sizeof(name));
		pr_err("atomic: cache invalid after flush "
		       "(high update rate or alloc failed), "
		       "memcg %s, page state idx %d\n",
		       name, idx);
	}

	smp_mb();  /* Ensure all updates visible */
	return css_atomic_page_state_recursive(memcg, idx);
}

/**
 * css_atomic_page_state_recursive - read atomic counter recursively
 * @memcg: the memory cgroup
 * @idx: the stat item
 *
 * Low-level function that always performs tree traversal without cache.
 * Most callers should use css_atomic_page_state() instead.
 *
 * Recursively aggregate per-cgroup atomic counters from this memcg and all
 * its descendants. Uses RCU for lock-free traversal of the cgroup tree.
 *
 * Each cgroup has a single atomic counter (not per-CPU), so reading is fast
 * - just atomic64_read() plus recursive aggregation. No per-CPU loops needed!
 *
 * IMPORTANT: This function returns SYSTEM-WIDE totals aggregated across all
 * NUMA nodes. Each cgroup's counter->state[] stores only its own (non-hierarchical)
 * value. This function recursively aggregates values from the entire subtree to
 * compute the hierarchical total. The per-node data in memcg_atomic_counter_per_node
 * is already aggregated during updates (mod_lruvec_state adds to both per-cgroup
 * and per-node counters), so counter->state[] reflects system-wide totals for
 * this specific cgroup (but not including descendants).
 *
 * For per-node statistics, use css_atomic_lruvec_page_state() instead,
 * which reads from the per-node atomic_counter_per_node structures.
 *
 * Returns: aggregated stat value including all descendants (across all nodes)
 */
static u64 css_atomic_page_state_recursive(struct mem_cgroup *memcg, int idx)
{
	struct mem_cgroup *child;
	struct memcg_atomic_counter *counter;
	u64 total = 0;

	/* BOOT SAFETY: Skip non-online memcgs to avoid reading partial state */
	if (unlikely(!mem_cgroup_online(memcg)))
		return 0;

	/* Early exit if no atomic counter */
	counter = READ_ONCE(memcg->atomic_counter);
	if (unlikely(!counter))
		return 0;

	/*
	 * Read single atomic value for this memcg.
	 *
	 * Note on consistency: This value is copied to a local variable and
	 * may become stale during the recursive aggregation below if another
	 * CPU modifies it concurrently. This results in a "mixed snapshot"
	 * where parent and children values come from slightly different times.
	 *
	 * This is acceptable for statistics because:
	 * 1. The race window is tiny (typically < 1 microsecond)
	 * 2. Errors don't accumulate across reads (each read is independent)
	 * 3. The actual counter value is not lost (next read will see new value)
	 *
	 * We trade perfect snapshot consistency for lock-free performance.
	 * Use READ_ONCE on .counter field instead of atomic64_read for better performance.
	 */
	total = READ_ONCE(counter->state[idx].counter);

	/* Early exit if no children to avoid RCU overhead */
	if (list_empty(&memcg->atomic_children))
		return total;

	/* Recursively aggregate all children - RCU protected, no lock */
	rcu_read_lock();
	list_for_each_entry_rcu(child, &memcg->atomic_children, atomic_sibling) {
		total += css_atomic_page_state_recursive(child, idx);
	}
	rcu_read_unlock();

	return total;
}

/* Visit callback for non-NUMA batch aggregation - IN-PLACE VERSION */
/* Optimized visit function: accumulate directly into cache (no temp array) */
static inline int css_atomic_visit_batch(struct mem_cgroup *memcg, void *data)
{
	struct memcg_atomic_cache *cache = data;
	struct memcg_atomic_counter *counter;
	int i;

	/* Fast path: check counter pointer once */
	counter = rcu_dereference(memcg->atomic_counter);
	if (unlikely(!counter))
		return 0;

	/* Directly accumulate into cache->stats IN-PLACE - cache-friendly sequential access
	 * Use direct read instead of READ_ONCE for better performance.
	 * Under RCU protection, counter pointer is stable and values are atomic64_t,
	 * so we can safely read the .counter field directly. The small race window
	 * for individual reads is acceptable for statistics.
	 *
	 * OPTIMIZATION: No temporary array needed! Accumulate directly into cache,
	 * protected by seqlock (write lock held by caller).
	 */
	for (i = 0; i < MEMCG_VMSTAT_SIZE; i++) {
		cache->stats[i] += counter->state[i].counter;
	}

	return 0;
}

/* Calculate threshold dynamically (handles CPU hotplug automatically) */
static inline unsigned int get_atomic_flush_threshold(void)
{
	return MEMCG_CHARGE_BATCH * num_online_cpus();
}

/* Check if atomic stats need fresh computation (similar to rstat threshold) */
static bool css_atomic_stats_need_flush(struct mem_cgroup *memcg)
{
	/* Use cached threshold to avoid repeated calculation */
	return atomic_read(&memcg->atomic_stats_updates) >
		get_atomic_flush_threshold();
}

/* Check if atomic events need fresh computation (similar to rstat threshold) */
static bool css_atomic_events_need_flush(struct mem_cgroup *memcg)
{
	/* Use cached threshold to avoid repeated calculation */
	return atomic_read(&memcg->atomic_events_updates) >
		get_atomic_flush_threshold();
}

/**
 * css_atomic_flush - flush atomic counter stats to cache
 * @memcg: the memory cgroup
 * @force: if true, always flush; if false, only flush if threshold exceeded
 *
 * Similar to __mem_cgroup_flush_stats() for rstat, this function ensures
 * the cache contains up-to-date aggregated statistics by:
 * 1. Checking if flush is needed (unless forced)
 * 2. Ensuring all atomic updates are visible (memory barrier)
 * 3. Recomputing stats/events by tree traversal (IN-PLACE to cache)
 * 4. Resetting the update counter
 *
 * This is more than just cache invalidation - it actively recomputes
 * and populates the cache with fresh data, similar to how rstat's flush
 * aggregates per-CPU data into the global vmstats.
 *
 * OPTIMIZATION: In-place cache updates with zero-copy reads.
 * Cache is updated directly during tree traversal, and reads access
 * cache data directly without any memory copying.
 */
void css_atomic_flush(struct mem_cgroup *memcg, bool force)
{
	/*
	 * BOOT SAFETY: Don't flush until memcg is fully online.
	 * Tree traversal during flush requires a stable cgroup hierarchy.
	 * Before online, local counters are sufficient (no children yet).
	 */
	if (!mem_cgroup_online(memcg))
		return;

	if (!force && !css_atomic_stats_need_flush(memcg) && !css_atomic_events_need_flush(memcg))
		return;

	/*
	 * Reset counters BEFORE recompute (similar to batch functions).
	 * This ensures updates during recompute are properly tracked for next flush.
	 */
	atomic_set(&memcg->atomic_stats_updates, 0);
	atomic_set(&memcg->atomic_events_updates, 0);

	/*
	 * Memory barrier to ensure atomic counter updates are visible.
	 *
	 * Unlike rstat's css_rstat_flush() which uses spinlocks that provide
	 * implicit memory barriers (acquire/release semantics), atomic counter
	 * is completely lockless. Without this explicit smp_mb():
	 * - Concurrent atomic64_add() on other CPUs may not be visible
	 * - Reordering could cause us to read stale counter values
	 * - No implicit synchronization like rstat's spin_lock_irq()
	 *
	 * This barrier ensures a best-effort visibility of concurrent updates.
	 * We don't guarantee all concurrent updates are visible (similar to
	 * rstat's eventual consistency model), but the barrier significantly
	 * improves the chances. Any missed updates will be caught in the next
	 * flush when the update counters accumulate again.
	 *
	 * Cost: smp_mb() overhead is negligible compared to tree traversal.
	 */
	smp_mb();

	/*
	 * Recompute and cache stats and events (similar to rstat's css_rstat_flush).
	 * These functions will traverse the tree and aggregate values directly into
	 * the cache (in-place), without using temporary arrays on stack.
	 */
	css_atomic_recompute_and_cache_stats(memcg);
	css_atomic_recompute_and_cache_events(memcg);

	/*
	 * Update flush timestamp for rate-limiting (similar to rstat).
	 * Only update for root memcg to have a global rate-limit reference.
	 */
	if (mem_cgroup_is_root(memcg))
		WRITE_ONCE(atomic_flush_last_time, jiffies_64);
}

/**
 * css_atomic_flush_ratelimited - rate-limited atomic counter cache flush
 * @memcg: the memory cgroup
 *
 * Similar to mem_cgroup_flush_stats_ratelimited() for rstat, this provides
 * a rate-limited flush for performance-sensitive paths (e.g., writeback).
 *
 * Only flushes if the periodic flusher is one full cycle late (4 seconds),
 * avoiding excessive flush overhead in hot paths while maintaining reasonable
 * accuracy for monitoring.
 */
void css_atomic_flush_ratelimited(struct mem_cgroup *memcg)
{
	/* Only flush if the periodic flusher is one full cycle late */
	if (time_after64(jiffies_64, READ_ONCE(atomic_flush_last_time) + 2*ATOMIC_FLUSH_TIME))
		css_atomic_flush(memcg, false);
}

/*
 * NOTE: css_atomic_cache_is_valid() has been removed as it's no longer needed.
 * Cache validity checks are now integrated directly into the cache read functions
 * (css_atomic_cache_read_stats/events) for better performance.
 */


/* Recompute stats from atomic counters and update cache - IN-PLACE VERSION */
static int css_atomic_recompute_and_cache_stats(struct mem_cgroup *memcg)
{
	struct memcg_atomic_cache *cache;

	cache = memcg->atomic_cache;
	if (unlikely(!cache))
		return -1;

	/*
	 * IN-PLACE UPDATE:
	 * 1. Take write lock on cache
	 * 2. Zero the cache in-place
	 * 3. Walk tree and accumulate directly into cache
	 * 4. Mark cache as valid and release lock
	 *
	 * Benefits:
	 * - No temporary arrays (saves ~512+ bytes stack space)
	 * - No memory copying between buffers
	 * - Better cache locality (single array accessed)
	 */
	write_seqlock(&cache->stats_seqlock);

	/* Zero cache in-place */
	memset(cache->stats, 0, MEMCG_VMSTAT_SIZE * sizeof(u64));

	/*
	 * Walk entire cgroup tree to aggregate stats directly into cache.
	 * Seqlock write protection ensures readers either see old or new data,
	 * never inconsistent intermediate state during accumulation.
	 */
	css_atomic_walk(memcg, css_atomic_visit_batch, cache);

	cache->valid = true; /* Mark cache as valid */
	write_sequnlock(&cache->stats_seqlock);

	return 0;
}


/* Visit callback for events batch aggregation - IN-PLACE VERSION */
static inline int css_atomic_visit_events_batch(struct mem_cgroup *memcg, void *data)
{
	struct memcg_atomic_cache *cache = data;
	struct memcg_atomic_counter *counter;
	int i;

	/* Fast path: check counter pointer once */
	counter = rcu_dereference(memcg->atomic_counter);
	if (unlikely(!counter))
		return 0;

	/* Directly accumulate into cache->events IN-PLACE - cache-friendly sequential access
	 * Use direct read instead of READ_ONCE for better performance.
	 * Under RCU protection, counter pointer is stable and values are atomic64_t,
	 * so we can safely read the .counter field directly.
	 *
	 * OPTIMIZATION: No temporary array needed! Accumulate directly into cache,
	 * protected by seqlock (write lock held by caller).
	 */
	for (i = 0; i < NR_MEMCG_EVENTS; i++)
		cache->events[i] += (unsigned long)counter->events[i].counter;

	return 0;
}

/* Recompute events from atomic counters and update cache - IN-PLACE VERSION */
static int css_atomic_recompute_and_cache_events(struct mem_cgroup *memcg)
{
	struct memcg_atomic_cache *cache;

	cache = memcg->atomic_cache;
	if (unlikely(!cache))
		return -1;

	/*
	 * IN-PLACE UPDATE:
	 * 1. Take write lock on cache
	 * 2. Zero the cache in-place
	 * 3. Walk tree and accumulate directly into cache
	 * 4. Mark cache as valid and release lock
	 *
	 * Benefits:
	 * - No temporary arrays (saves stack space)
	 * - No memory copying between buffers
	 * - Better cache locality (single array accessed)
	 */
	write_seqlock(&cache->events_seqlock);

	/* Zero cache in-place */
	memset(cache->events, 0, NR_MEMCG_EVENTS * sizeof(unsigned long));

	/*
	 * Walk entire cgroup tree to aggregate events directly into cache.
	 * Seqlock write protection ensures readers either see old or new data,
	 * never inconsistent intermediate state during accumulation.
	 */
	css_atomic_walk(memcg, css_atomic_visit_events_batch, cache);

	cache->valid = true; /* Mark cache as valid */
	write_sequnlock(&cache->events_seqlock);

	return 0;
}


/**
 * css_atomic_events - read atomic counter events with cache support
 * @memcg: the memory cgroup
 * @idx: the event item
 * @force: if true, bypass cache and force recursive read
 *
 * Returns: event count
 */
unsigned long css_atomic_events(struct mem_cgroup *memcg, enum vm_event_item idx,
				bool force)
{
	unsigned long value;
	int i = memcg_events_index(idx);
	bool flushed = false;
	struct memcg_atomic_counter *counter;

	if (WARN_ONCE(BAD_STAT_IDX(i), "%s: missing event item %d\n", __func__, idx))
		return 0;

	/*
	 * BOOT SAFETY: Check if memcg is fully online before allowing
	 * tree traversal. Same rationale as css_atomic_page_state().
	 */
	if (!mem_cgroup_online(memcg)) {
		counter = READ_ONCE(memcg->atomic_counter);
		if (counter && i >= 0)
			return (unsigned long)atomic64_read(&counter->events[i]);
		return 0;
	}

	/* Fast path: try to read from cache without flushing */
	/* Threshold check is now done inside cache read function */
	if (!force && memcg->atomic_cache) {
		if (css_atomic_read_events_from_cache(memcg->atomic_cache,
						      &memcg->atomic_events_updates,
						      i, &value))
			return value;
	}

	/*
	 * Slow path: need to flush and update cache
	 * - force=true: explicitly requested
	 * - threshold exceeded: updates accumulated
	 * - cache miss: cache not yet initialized
	 */
	/* Force flush if cache is not initialized */
	if (!memcg->atomic_cache || !memcg->atomic_cache->valid)
		force = true;
	css_atomic_flush(memcg, force);
	flushed = true;

	/* Try to read from updated cache */
	if (css_atomic_read_events_from_cache(memcg->atomic_cache,
					      &memcg->atomic_events_updates,
					      i, &value))
		return value;

	/*
	 * Ultimate fallback: cache still invalid after flush.
	 * Possible causes:
	 * - Cache allocation failed during initialization
	 * - Extremely high update rate: cache invalidated again between
	 *   flush and read (concurrent writes exceeded threshold)
	 * This forces direct tree traversal (slower but correct).
	 */
	if (flushed && memcg->css.cgroup) {
		char name[NAME_MAX];
		cgroup_name(memcg->css.cgroup, name, sizeof(name));
		pr_err("atomic: cache invalid after flush "
		       "(high update rate or alloc failed), "
		       "memcg %s, event idx %d\n",
		       name, idx);
	}

	smp_mb(); /* Ensure all updates visible */
	return css_atomic_events_recursive(memcg, idx);
}

/**
 * css_atomic_events_recursive - read atomic counter events recursively
 * @memcg: the memory cgroup
 * @idx: the event item
 *
 * Recursively aggregate per-cgroup atomic event counters from this memcg and all
 * its descendants. Uses RCU for lock-free traversal of the cgroup tree.
 *
 * Returns: aggregated event count including all descendants
 */
unsigned long css_atomic_events_recursive(struct mem_cgroup *memcg,
					  enum vm_event_item idx)
{
	struct mem_cgroup *child;
	struct memcg_atomic_counter *counter;
	unsigned long total = 0;
	int i;

	/* BOOT SAFETY: Skip non-online memcgs to avoid reading partial state */
	if (unlikely(!mem_cgroup_online(memcg)))
		return 0;

	/* Early exit if no atomic counter */
	counter = READ_ONCE(memcg->atomic_counter);
	if (unlikely(!counter))
		return 0;

	i = memcg_events_index(idx);

	/* Read single atomic value for this memcg
	 * Use READ_ONCE on .counter field instead of atomic64_read for better performance.
	 */
	total = READ_ONCE(counter->events[i].counter);

	/* Early exit if no children to avoid RCU overhead */
	if (list_empty(&memcg->atomic_children))
		return total;

	/* Recursively aggregate all children - RCU protected, no lock */
	rcu_read_lock();
	list_for_each_entry_rcu(child, &memcg->atomic_children, atomic_sibling) {
		total += css_atomic_events_recursive(child, idx);
	}
	rcu_read_unlock();

	return total;
}

/**
 * css_atomic_walk - iteratively walk cgroup tree
 * @memcg: starting memory cgroup
 * @visit: callback function to call for each cgroup
 * @arg: user data passed to visit callback
 *
 * Iteratively traverse the cgroup tree using mem_cgroup_iter().
 * Uses iteration instead of recursion to prevent stack overflow with
 * extremely deep cgroup hierarchies (kernel stack is only 8-16KB).
 *
 * Returns: 0 on success, or non-zero if visit callback returns non-zero
 */
/* Internal helper that assumes RCU lock is already held */
/* Use iteration instead of recursion to avoid stack overflow with deep hierarchies */
static int __css_atomic_walk_locked(struct mem_cgroup *memcg,
				     int (*visit)(struct mem_cgroup *, void *),
				     void *arg)
{
	struct mem_cgroup *iter;
	int ret;

	/*
	 * Use mem_cgroup_iter() for iterative tree traversal.
	 * This avoids stack overflow with extremely deep cgroup hierarchies
	 * (kernel stack is only 8-16KB, deep recursion would overflow).
	 */
	for (iter = memcg; iter; iter = mem_cgroup_iter(memcg, iter, NULL)) {
		/*
		 * BOOT SAFETY: Skip memcgs that aren't fully online yet.
		 * During boot/initialization, some memcgs may be in the tree
		 * but not fully initialized. Accessing their atomic_counter
		 * or other fields could be unsafe.
		 */
		if (!mem_cgroup_online(iter))
			continue;

		ret = visit(iter, arg);
		if (unlikely(ret))
			return ret;
	}

	return 0;
}

int css_atomic_walk(struct mem_cgroup *memcg,
		    int (*visit)(struct mem_cgroup *, void *),
		    void *arg)
{
	int ret;

	/* Optimize: single RCU lock for entire tree traversal */
	/* This reduces lock overhead significantly for deep trees */
	rcu_read_lock();
	ret = __css_atomic_walk_locked(memcg, visit, arg);
	rcu_read_unlock();
	return ret;
}


#endif /* CONFIG_MEMCG_ATOMIC_COUNTER */

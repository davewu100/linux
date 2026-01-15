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

/* Forward declarations */
static u64 css_atomic_page_state_recursive(struct mem_cgroup *memcg, int idx);
static int __css_atomic_walk_locked(struct mem_cgroup *memcg,
				    int (*visit)(struct mem_cgroup *, void *),
				    void *arg);


/**
 * css_atomic_page_state - read atomic counter by tree traversal
 * @memcg: the memory cgroup
 * @idx: the stat item (enum memcg_stat_item or node_stat_item)
 * @force: unused (kept for API compatibility)
 *
 * Directly aggregates atomic counters from the entire cgroup tree.
 * This is simpler and more efficient for low-read-frequency scenarios.
 *
 * Returns: aggregated stat value
 */
u64 css_atomic_page_state(struct mem_cgroup *memcg, int idx, bool force)
{
	int i = memcg_stats_index(idx);

	if (WARN_ONCE(BAD_STAT_IDX(i), "%s: missing stat item %d\n", __func__, idx))
		return 0;

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

/**
 * css_atomic_events - read atomic counter events by tree traversal
 * @memcg: the memory cgroup
 * @idx: the event item
 * @force: unused (kept for API compatibility)
 *
 * Directly aggregates atomic event counters from the entire cgroup tree.
 *
 * Returns: aggregated event count
 */
unsigned long css_atomic_events(struct mem_cgroup *memcg, enum vm_event_item idx,
				bool force)
{
	int i = memcg_events_index(idx);

	if (WARN_ONCE(BAD_STAT_IDX(i), "%s: missing event item %d\n", __func__, idx))
		return 0;

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

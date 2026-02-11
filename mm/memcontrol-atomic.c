// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Memory cgroup atomic counter implementation (memcg-specific parts)
 *
 * This file contains memcg-specific update and lifecycle logic for the
 * atomic counter backend. The generic read/flush/cache logic lives in
 * kernel/cgroup/atomic.c, similar to how rstat splits generic vs. memcg parts.
 */

#include <linux/memcontrol.h>
#include <linux/memcontrol-atomic.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>

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
 * memcg_atomic_mod_lruvec_state - update per-node atomic counter (for numa_stat)
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

	if (unlikely(!counter || !node_counter))
		return;

	atomic64_add(val, &counter->state[idx]);

#ifdef CONFIG_MEMCG_V1
	atomic64_add(val, &counter->state_local[idx]);
#endif

	atomic64_add(val, &node_counter->state[idx]);

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
 * memcg_atomic_init_per_node - allocate per-node atomic counter (for numa_stat)
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
	 * - Concurrent readers in css_atomic_page_state_recursive()
	 *   and css_atomic_events_recursive() use rcu_read_lock()
	 *   to traverse the children list safely
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

#endif /* CONFIG_MEMCG_ATOMIC_COUNTER */

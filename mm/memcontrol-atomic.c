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
#include <linux/mmzone.h>
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
	struct memcg_atomic_local *local;
	struct mem_cgroup *iter;

	/* Redirect to parent if this cgroup is being offlined (avoids lost deltas) */
	if (unlikely(READ_ONCE(memcg->atomic_offlining))) {
		memcg = parent_mem_cgroup(memcg);
		if (!memcg)
			return;
	}

	local = READ_ONCE(memcg->atomic_local);
	/* Safety: local might be NULL during init/teardown */
	if (unlikely(!local))
		return;
	if (unlikely(idx < 0 || idx >= MEMCG_VMSTAT_SIZE))
		return;

	/* Update local counts (atomic, fast) */
	atomic64_add(val, &local->state[idx]);

#ifdef CONFIG_MEMCG_V1
	atomic64_add(val, &local->state_local[idx]);
#endif

	/* Propagate stats_updates upward (rstat-like; triggers flush when over threshold) */
	for (iter = memcg; iter; iter = parent_mem_cgroup(iter)) {
		if (iter->atomic_aggregated)
			atomic_inc(&iter->atomic_aggregated->stats_updates);
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
	struct memcg_atomic_local *local;
	struct memcg_atomic_local_per_node *node_local;
	struct mem_cgroup *iter;

	/* Redirect to parent if this cgroup is being offlined (avoids lost deltas) */
	if (unlikely(READ_ONCE(memcg->atomic_offlining))) {
		struct mem_cgroup *parent = parent_mem_cgroup(memcg);
		int nid;

		if (!parent)
			return;
		nid = lruvec_pgdat(&pn->lruvec)->node_id;
		pn = parent->nodeinfo[nid];
		if (unlikely(!pn))
			return;
		memcg = parent;
	}

	local = READ_ONCE(memcg->atomic_local);
	node_local = READ_ONCE(pn->atomic_local_per_node);
	if (unlikely(!local || !node_local))
		return;
	if (unlikely(idx < 0 || idx >= NR_VM_NODE_STAT_ITEMS))
		return;

	atomic64_add(val, &local->state[idx]);

#ifdef CONFIG_MEMCG_V1
	atomic64_add(val, &local->state_local[idx]);
#endif

	atomic64_add(val, &node_local->state[idx]);

	/* Propagate stats_updates upward (rstat-like) */
	for (iter = memcg; iter; iter = parent_mem_cgroup(iter)) {
		if (iter->atomic_aggregated)
			atomic_inc(&iter->atomic_aggregated->stats_updates);
	}
}

/**
 * memcg_atomic_lruvec_page_state - read per-node atomic counter (for numa_stat.atomic)
 * @lruvec: the lruvec (implies node)
 * @idx: node_stat_item (same as memory_stats[].idx for node stats)
 */
unsigned long memcg_atomic_lruvec_page_state(struct lruvec *lruvec, int idx)
{
	struct mem_cgroup_per_node *pn;
	struct memcg_atomic_local_per_node *node_local;
	int i;

	i = memcg_stats_index(idx);
	if (i >= NR_VM_NODE_STAT_ITEMS)
		return 0;

	pn = container_of(lruvec, struct mem_cgroup_per_node, lruvec);
	node_local = READ_ONCE(pn->atomic_local_per_node);
	if (!node_local)
		return 0;

	return atomic64_read(&node_local->state[i]);
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
	struct memcg_atomic_local *local;
	struct mem_cgroup *iter;

	/* Redirect to parent if this cgroup is being offlined (avoids lost deltas) */
	if (unlikely(READ_ONCE(memcg->atomic_offlining))) {
		memcg = parent_mem_cgroup(memcg);
		if (!memcg)
			return;
	}

	local = READ_ONCE(memcg->atomic_local);
	if (unlikely(!local))
		return;
	if (unlikely(idx < 0 || idx >= NR_MEMCG_EVENTS))
		return;

	atomic64_add(count, &local->events[idx]);

#ifdef CONFIG_MEMCG_V1
	atomic64_add(count, &local->events_local[idx]);
#endif

	/* Propagate stats_updates upward (rstat-like) */
	for (iter = memcg; iter; iter = parent_mem_cgroup(iter)) {
		if (iter->atomic_aggregated)
			atomic_inc(&iter->atomic_aggregated->stats_updates);
	}
}

/**
 * memcg_atomic_transfer_to_parent - transfer child stats to parent on offline
 * @memcg: the memory cgroup being offlined. Caller sets atomic_offlining first so in-flight updates are redirected to parent.
 */
void memcg_atomic_transfer_to_parent(struct mem_cgroup *memcg)
{
	struct mem_cgroup *parent = parent_mem_cgroup(memcg);
	struct memcg_atomic_local *child_local;
	struct memcg_atomic_local *parent_local;
	int i;

	if (unlikely(!parent))
		return;

	child_local = READ_ONCE(memcg->atomic_local);
	parent_local = READ_ONCE(parent->atomic_local);
	if (unlikely(!child_local || !parent_local))
		return;

	for (i = 0; i < MEMCG_VMSTAT_SIZE; i++) {
		s64 delta = atomic64_xchg(&child_local->state[i], 0);

		if (delta)
			atomic64_add(delta, &parent_local->state[i]);
	}

	for (i = 0; i < NR_MEMCG_EVENTS; i++) {
		s64 delta = atomic64_xchg(&child_local->events[i], 0);

		if (delta)
			atomic64_add(delta, &parent_local->events[i]);
	}

	for_each_node_state(i, N_MEMORY) {
		struct mem_cgroup_per_node *pn = memcg->nodeinfo[i];
		struct mem_cgroup_per_node *ppn = parent->nodeinfo[i];
		struct memcg_atomic_local_per_node *child_node;
		struct memcg_atomic_local_per_node *parent_node;
		int j;

		if (unlikely(!pn || !ppn))
			continue;

		child_node = READ_ONCE(pn->atomic_local_per_node);
		parent_node = READ_ONCE(ppn->atomic_local_per_node);
		if (unlikely(!child_node || !parent_node))
			continue;

		for (j = 0; j < NR_VM_NODE_STAT_ITEMS; j++) {
			s64 delta = atomic64_xchg(&child_node->state[j], 0);

			if (delta)
				atomic64_add(delta, &parent_node->state[j]);
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
	/* Allocate per-cgroup local counts (this cgroup only) */
	memcg->atomic_local = kzalloc(sizeof(struct memcg_atomic_local),
					GFP_KERNEL_ACCOUNT);
	if (unlikely(!memcg->atomic_local))
		return -ENOMEM;

	INIT_LIST_HEAD(&memcg->atomic_children);
	INIT_LIST_HEAD(&memcg->atomic_sibling);
	spin_lock_init(&memcg->atomic_children_lock);

	/* Allocate aggregated (subtree sum for reads; rstat-like) */
	memcg->atomic_aggregated = kzalloc(sizeof(struct memcg_atomic_aggregated),
				      GFP_KERNEL_ACCOUNT);
	if (unlikely(!memcg->atomic_aggregated)) {
		kfree(memcg->atomic_local);
		memcg->atomic_local = NULL;
		return -ENOMEM;
	}

	atomic_set(&memcg->atomic_aggregated->stats_updates, 0);
	memcg->atomic_aggregated->flush_time = 0;

	return 0;
}

/**
 * memcg_atomic_exit - free atomic_local and atomic_aggregated
 * @memcg: the memory cgroup
 *
 * RCU synchronization is handled by the caller (cgroup teardown).
 */
void memcg_atomic_exit(struct mem_cgroup *memcg)
{
	kfree(memcg->atomic_local);
	memcg->atomic_local = NULL;
	kfree(memcg->atomic_aggregated);
	memcg->atomic_aggregated = NULL;
}

/**
 * memcg_atomic_init_per_node - allocate per-node atomic counter (for numa_stat)
 */
int memcg_atomic_init_per_node(struct mem_cgroup_per_node *pn, int node)
{
	pn->atomic_local_per_node = kzalloc_node(
		sizeof(struct memcg_atomic_local_per_node),
		GFP_KERNEL_ACCOUNT, node);
	if (!pn->atomic_local_per_node)
		return -ENOMEM;
	return 0;
}

/**
 * memcg_atomic_exit_per_node - free per-node atomic counter
 */
void memcg_atomic_exit_per_node(struct mem_cgroup_per_node *pn)
{
	kfree(pn->atomic_local_per_node);
	pn->atomic_local_per_node = NULL;
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
 *
 * To avoid losing in-flight updates (and thus under-counting the parent),
 * we set atomic_offlining before transfer. Any mod_state/mod_lruvec_state/
 * count_events that target this memcg after the flag is visible are
 * redirected to the parent; then we transfer the child's current counts
 * to the parent. Order: WRITE_ONCE(atomic_offlining) -> smp_mb() ->
 * transfer_to_parent -> list_del_rcu.
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
	 *   and atomic_local are freed
	 * - This ordering is: css_offline -> list_del_rcu -> RCU grace period
	 *   -> css_free -> __mem_cgroup_free -> kfree(memcg->atomic_local)
	 *
	 * Therefore, no explicit synchronize_rcu() or call_rcu() is needed here.
	 */
	if (!mem_cgroup_is_root(memcg)) {
		struct mem_cgroup *parent = parent_mem_cgroup(memcg);

		/* Redirect any in-flight updates to parent before transfer */
		WRITE_ONCE(memcg->atomic_offlining, true);
		smp_mb();

		memcg_atomic_transfer_to_parent(memcg);

		spin_lock(&parent->atomic_children_lock);
		list_del_rcu(&memcg->atomic_sibling);
		spin_unlock(&parent->atomic_children_lock);
	}
}

#endif /* CONFIG_MEMCG_ATOMIC_COUNTER */

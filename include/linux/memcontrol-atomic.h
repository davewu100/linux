/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _LINUX_MEMCONTROL_ATOMIC_H
#define _LINUX_MEMCONTROL_ATOMIC_H

#include <linux/cgroup-atomic.h>

#ifdef CONFIG_MEMCG_ATOMIC_COUNTER

/* Core update functions */
void css_atomic_mod_state(struct mem_cgroup *memcg, int idx, int val);
void css_atomic_mod_lruvec_state(struct mem_cgroup *memcg,
				 struct mem_cgroup_per_node *pn,
				 int idx, int val);
void css_atomic_count_events(struct mem_cgroup *memcg, int idx,
			     unsigned long count);

/* Per-node reads */
unsigned long css_atomic_lruvec_page_state(struct lruvec *lruvec,
					   enum node_stat_item idx);

/* Lifecycle management */
int css_atomic_init(struct mem_cgroup *memcg);
void css_atomic_exit(struct mem_cgroup *memcg);
int css_atomic_init_per_node(struct mem_cgroup_per_node *pn, int node);
void css_atomic_exit_per_node(struct mem_cgroup_per_node *pn);
int css_atomic_online(struct mem_cgroup *memcg);
void css_atomic_offline(struct mem_cgroup *memcg);

/* Tree management */
void css_atomic_transfer_to_parent(struct mem_cgroup *memcg);

#else /* !CONFIG_MEMCG_ATOMIC_COUNTER */

static inline void css_atomic_mod_state(struct mem_cgroup *memcg,
					int idx, int val) { }
static inline void css_atomic_mod_lruvec_state(struct mem_cgroup *memcg,
					       struct mem_cgroup_per_node *pn,
					       int idx, int val) { }
static inline void css_atomic_count_events(struct mem_cgroup *memcg, int idx,
					   unsigned long count) { }
static inline unsigned long css_atomic_lruvec_page_state(struct lruvec *lruvec,
							 enum node_stat_item idx)
{
	return 0;
}
static inline int css_atomic_init(struct mem_cgroup *memcg)
{
	return 0;
}
static inline void css_atomic_exit(struct mem_cgroup *memcg) { }
static inline int css_atomic_init_per_node(struct mem_cgroup_per_node *pn,
					    int node)
{
	return 0;
}
static inline void css_atomic_exit_per_node(struct mem_cgroup_per_node *pn) { }
static inline int css_atomic_online(struct mem_cgroup *memcg)
{
	return 0;
}
static inline void css_atomic_offline(struct mem_cgroup *memcg) { }
static inline void css_atomic_transfer_to_parent(struct mem_cgroup *memcg) { }

#endif /* CONFIG_MEMCG_ATOMIC_COUNTER */

#endif /* _LINUX_MEMCONTROL_ATOMIC_H */

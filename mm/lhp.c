// SPDX-License-Identifier: GPL-2.0
/*
 * LHP - Layered HugePage allocator (standalone, plan B).
 *
 * See include/linux/lhp.h for the high-level design.  In short: reserve a chunk
 * of physically contiguous memory at boot, carve it into 1G nodes, and manage a
 * three-level (1G/2M/4K) split/merge hierarchy entirely inside this file.  The
 * backing pages never return to the buddy allocator, so contiguity is retained
 * and merging back up to 1G is deterministic.
 *
 * Layering (DPDK-style):
 *   phys pool (split/merge) -> memzone -> obj pool -> buffer (kmalloc-like).
 *
 * SLUB integration is deferred; see lhp_slub_integration_notes below.
 */
#define pr_fmt(fmt) "lhp: " fmt

#include <linux/lhp.h>
#include <linux/cma.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/init.h>
#include <linux/sizes.h>
#include <linux/memblock.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/ktime.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/string.h>
#include <linux/err.h>
#include <linux/sysfs.h>

/*
 * A node in the layered hierarchy.  One struct describes a single 1G, 2M or 4K
 * chunk.  Nodes are allocated lazily as parents are split.
 *
 * Invariants:
 *   - A node is on exactly one of: its parent's free list, the allocated set
 *     (LHP_ALLOCATED), or the split set (LHP_SPLIT, children exist).
 *   - free_children counts children currently in state LHP_FREE.  A node can be
 *     merged (children freed, node returned to FREE) iff it is LHP_SPLIT and
 *     free_children == LHP_FANOUT.
 */
enum lhp_state {
	LHP_FREE,		/* on parent->free_children list, allocatable */
	LHP_ALLOCATED,		/* handed out to a caller */
	LHP_SPLIT,		/* broken into LHP_FANOUT children */
};

struct lhp_node {
	struct page		*base;		/* head page of the chunk */
	enum lhp_level		level;
	enum lhp_state		state;
	struct lhp_node		*parent;	/* NULL for 1G roots */
	/*
	 * When SPLIT, @children points at a single bulk allocation of
	 * LHP_FANOUT contiguous lhp_node structs (not 512 separate objects).
	 * This lets split/merge allocate, free, list-splice and account the
	 * whole fan-out in O(1) batch operations instead of 512 times each.
	 * child i lives at &children->nodes[i].
	 */
	struct lhp_child_array	*children;	/* fan-out when SPLIT */
	unsigned int		free_children;	/* # children in LHP_FREE */
	struct list_head	list;		/* member of a per-level freelist */
};

/*
 * A whole fan-out worth of child nodes in one allocation.  These are recycled
 * through the pool's array cache rather than kvfree()d on every merge: this
 * removes the repeated kvcalloc/kvfree churn on hot split/merge paths and, more
 * importantly, avoids the vmalloc unmap (and its TLB flush) that kvfree() would
 * otherwise incur when the array is large enough to fall back to vmalloc.
 */
struct lhp_child_array {
	struct list_head	cache;		/* member of pool->array_cache */
	struct lhp_node		nodes[LHP_FANOUT];
};

/* Upper bound on splits a single lhp_alloc() may trigger (1G->2M->4K). */
#define LHP_MAX_SPLIT_DEPTH	(LHP_NR_LEVELS - 1)

/*
 * A single 1G region.  This is the unit of locking: a region owns its whole
 * subtree (its 1G root and any 2M/4K nodes it was split into), its own free
 * lists and its own child-array recycle cache.  Because a region's subtree
 * never spans another region, all split/merge/alloc/free within it are
 * serialised only by the region's own lock -- different CPUs operating on
 * different regions never contend.
 *
 * free_lists[]/nr_free[] only ever hold sub-1G levels (2M, 4K); the 1G root's
 * availability is tracked separately by @root_free so a whole-region 1G alloc
 * does not have to walk a list.
 */
struct lhp_region {
	spinlock_t		lock;
	struct lhp_node		*root;			/* the 1G node */
	bool			root_free;		/* root allocatable as 1G */
	struct list_head	free_lists[LHP_NR_LEVELS];
	unsigned long		nr_free[LHP_NR_LEVELS];
	unsigned long		nr_alloc[LHP_NR_LEVELS];
	/* Per-region recycled child-array cache (see struct lhp_child_array). */
	struct list_head	array_cache;
	unsigned long		nr_cached;
	struct page		*base;			/* 1G head page */
};

/*
 * The pool: an immutable-after-init array of regions plus a round-robin cursor
 * used to spread allocations across regions and reduce per-region contention.
 */
struct lhp_phys_pool {
	struct cma		*cma;
	unsigned long		nr_regions;
	struct lhp_region	*regions;		/* [nr_regions] */
	atomic_t		cursor;			/* round-robin start hint */
	bool			ready;
};

static struct lhp_phys_pool lhp_phys_pool;

/* Boot-time reservation state. */
static phys_addr_t lhp_reserve_size __initdata;
static struct cma *lhp_cma __initdata;

/* --------------------------------------------------------------------------
 * Boot-time reservation
 * -------------------------------------------------------------------------- */

static int __init lhp_cmdline(char *p)
{
	if (!p)
		return -EINVAL;
	lhp_reserve_size = memparse(p, &p);
	return 0;
}
early_param("lhp", lhp_cmdline);

/*
 * Reserve the LHP CMA area.  Must be called from the arch mem-reserve path,
 * after memblock is up and before the buddy allocator takes over (same window
 * as hugetlb_cma_reserve()).  Size is rounded up to a 1G multiple.
 */
void __init lhp_cma_reserve(void)
{
	phys_addr_t size = lhp_reserve_size;
	int ret;

	if (!size)
		return;

	size = ALIGN(size, SZ_1G);

	ret = cma_declare_contiguous_nid(0, size, 0, SZ_1G, 0, false,
					 "lhp", &lhp_cma, NUMA_NO_NODE);
	if (ret) {
		pr_warn("failed to reserve %llu MiB CMA (%d)\n",
			(u64)size / SZ_1M, ret);
		lhp_cma = NULL;
		return;
	}

	pr_info("reserved %llu MiB contiguous for layered hugepage pool\n",
		(u64)size / SZ_1M);
}

/* --------------------------------------------------------------------------
 * Node helpers
 * -------------------------------------------------------------------------- */

/* Initialise a node in place (used for both bulk children and single roots). */
static void lhp_node_init(struct lhp_node *n, struct page *base,
			  enum lhp_level level, struct lhp_node *parent)
{
	n->base = base;
	n->level = level;
	n->state = LHP_FREE;
	n->parent = parent;
	n->children = NULL;
	n->free_children = 0;
	INIT_LIST_HEAD(&n->list);
}

/* Allocate and initialise a single node (used for 1G roots). */
static struct lhp_node *lhp_node_alloc(struct page *base, enum lhp_level level,
				       struct lhp_node *parent)
{
	struct lhp_node *n;

	n = kzalloc(sizeof(*n), GFP_KERNEL);
	if (!n)
		return NULL;
	lhp_node_init(n, base, level, parent);
	return n;
}

static void lhp_freelist_add(struct lhp_region *rg, struct lhp_node *n)
{
	n->state = LHP_FREE;
	list_add(&n->list, &rg->free_lists[n->level]);
	rg->nr_free[n->level]++;
}

static void lhp_freelist_del(struct lhp_region *rg, struct lhp_node *n)
{
	list_del_init(&n->list);
	rg->nr_free[n->level]--;
}

/*
 * Bulk-add a whole fan-out of @count nodes (all of the same @level) to their
 * freelist in one O(1) list_splice plus a single counter update, instead of
 * @count individual list_add + increments.
 */
static void lhp_freelist_add_bulk(struct lhp_region *rg, struct lhp_node *nodes,
				  unsigned int count, enum lhp_level level)
{
	LIST_HEAD(batch);
	unsigned int i;

	for (i = 0; i < count; i++) {
		nodes[i].state = LHP_FREE;
		list_add_tail(&nodes[i].list, &batch);
	}
	list_splice_tail(&batch, &rg->free_lists[level]);
	rg->nr_free[level] += count;
}

/*
 * Bulk-remove a whole fan-out of @count nodes from their freelist.  The nodes
 * are contiguous (one allocation) so we just unlink each and drop the counter
 * once.  Unlinking is required because merge frees the backing array afterwards.
 */
static void lhp_freelist_del_bulk(struct lhp_region *rg, struct lhp_node *nodes,
				  unsigned int count, enum lhp_level level)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		list_del_init(&nodes[i].list);
	rg->nr_free[level] -= count;
}

/* --------------------------------------------------------------------------
 * Child-array cache + prealloc reservoir
 *
 * kvzalloc() may sleep and even trigger reclaim, so we never call it under a
 * region lock.  Instead lhp_alloc() pre-allocates enough arrays into a small
 * on-stack reservoir *before* taking the lock; the split path consumes from the
 * reservoir, and any leftovers are pushed into the target region's recycle
 * cache.  Merge returns arrays to the region cache instead of freeing them.
 * The cache is now per-region, so recycling never crosses the region lock.
 * -------------------------------------------------------------------------- */

/* A tiny stack-local stash of preallocated arrays handed into the locked path. */
struct lhp_reservoir {
	struct lhp_child_array	*arr[LHP_MAX_SPLIT_DEPTH];
	unsigned int		nr;
};

static struct lhp_child_array *lhp_array_alloc(gfp_t gfp)
{
	return kvzalloc(sizeof(struct lhp_child_array), gfp);
}

/* Pop a recycled array from the region cache (caller holds region lock). */
static struct lhp_child_array *lhp_cache_pop(struct lhp_region *rg)
{
	struct lhp_child_array *a;

	a = list_first_entry_or_null(&rg->array_cache,
				     struct lhp_child_array, cache);
	if (a) {
		list_del(&a->cache);
		rg->nr_cached--;
	}
	return a;
}

/* Upper bound on how many recycled arrays a single region caches. */
#define LHP_ARRAY_CACHE_MAX	64

/*
 * Push an array into the region cache (caller holds region lock).  Returns true
 * if it was cached, or false if the cache is already at LHP_ARRAY_CACHE_MAX, in
 * which case the caller owns @a and must free it.
 */
static bool lhp_cache_push(struct lhp_region *rg, struct lhp_child_array *a)
{
	if (rg->nr_cached >= LHP_ARRAY_CACHE_MAX)
		return false;
	list_add(&a->cache, &rg->array_cache);
	rg->nr_cached++;
	return true;
}

/*
 * Fill @r with @need freshly allocated arrays, outside any lock.  Returns 0 or
 * -ENOMEM (freeing anything it managed to grab).  We deliberately over-provision
 * from the allocator rather than peek at a cache, since caches can only be
 * inspected under a region lock; unused arrays are recycled into a region cache
 * later.
 */
static int lhp_reservoir_fill(struct lhp_reservoir *r, unsigned int need,
			      gfp_t gfp)
{
	r->nr = 0;
	while (r->nr < need) {
		struct lhp_child_array *a = lhp_array_alloc(gfp);

		if (!a) {
			/*
			 * kvfree_atomic() rather than kvfree(): a GFP_ATOMIC
			 * caller reaches this error path in atomic context, and
			 * a vmalloc-backed array would otherwise sleep in
			 * vfree().
			 */
			while (r->nr)
				kvfree_atomic(r->arr[--r->nr]);
			return -ENOMEM;
		}
		r->arr[r->nr++] = a;
	}
	return 0;
}

/* Take one array from the reservoir, or NULL if exhausted. */
static struct lhp_child_array *lhp_reservoir_take(struct lhp_reservoir *r)
{
	if (!r->nr)
		return NULL;
	return r->arr[--r->nr];
}

/*
 * Drain leftover reservoir arrays into a region cache (caller holds the region
 * lock).  Stops once the per-region cache is full (lhp_cache_push() returns
 * false), leaving any remainder in the reservoir for lhp_reservoir_free().
 */
static void lhp_reservoir_drain(struct lhp_region *rg, struct lhp_reservoir *r)
{
	while (r->nr && lhp_cache_push(rg, r->arr[r->nr - 1]))
		r->nr--;
}

/*
 * Free any arrays still left in the reservoir.  Uses kvfree_atomic() so it is
 * safe to call from atomic context (a GFP_ATOMIC caller) even for the
 * vmalloc-backed arrays, where plain kvfree() -> vfree() would sleep.
 */
static void lhp_reservoir_free(struct lhp_reservoir *r)
{
	while (r->nr)
		kvfree_atomic(r->arr[--r->nr]);
}

/*
 * Number of splits an allocation of @level may trigger in the worst case (when
 * every intermediate freelist is empty): one per level between @level and 1G.
 */
static unsigned int lhp_splits_needed(enum lhp_level level)
{
	return LHP_LEVEL_1G - level;
}

/* --------------------------------------------------------------------------
 * Split / merge state machine
 * -------------------------------------------------------------------------- */

/*
 * Split a free node into LHP_FANOUT free children of the next level down.
 * The node moves FREE -> SPLIT.  The backing child array is taken from the
 * region recycle cache, else from @res (preallocated outside the lock).
 * Returns 0 or -ENOMEM.  Caller holds the region lock.
 */
static int lhp_split_node(struct lhp_region *rg, struct lhp_node *node,
			  struct lhp_reservoir *res)
{
	enum lhp_level child_level = node->level - 1;
	unsigned long child_pages = lhp_level_nr_pages(child_level);
	struct lhp_child_array *children;
	unsigned int i;

	if (WARN_ON_ONCE(node->level == LHP_LEVEL_4K))
		return -EINVAL;
	if (WARN_ON_ONCE(node->state != LHP_FREE))
		return -EINVAL;

	/*
	 * No allocation here: pull a recycled array from the region cache, else
	 * one the caller preallocated outside the lock.  This keeps the locked
	 * critical section allocation-free (no sleeping, no reclaim) and reuses
	 * arrays across split/merge cycles instead of kvzalloc/kvfree churn.
	 */
	children = lhp_cache_pop(rg);
	if (!children)
		children = lhp_reservoir_take(res);
	if (!children)
		return -ENOMEM;

	for (i = 0; i < LHP_FANOUT; i++)
		lhp_node_init(&children->nodes[i],
			      node->base + i * child_pages, child_level, node);

	/* Commit: publish children and move parent to SPLIT. */
	if (node->level == LHP_LEVEL_1G)
		rg->root_free = false;	/* root now subdivided, not a whole 1G */
	else
		lhp_freelist_del(rg, node);
	node->children = children;
	node->free_children = LHP_FANOUT;
	node->state = LHP_SPLIT;

	/* Batch-insert all LHP_FANOUT children in one splice + one counter add. */
	lhp_freelist_add_bulk(rg, children->nodes, LHP_FANOUT, child_level);

	return 0;
}

/*
 * If every child of a SPLIT node is free, collapse it back: free the children
 * and return the node to its own freelist as FREE.  Recurses upward so a single
 * 4K free can bubble all the way to a reconstituted 1G.
 */
static void lhp_try_merge(struct lhp_region *rg, struct lhp_node *node)
{
	while (node && node->state == LHP_SPLIT &&
	       node->free_children == LHP_FANOUT) {
		struct lhp_node *parent = node->parent;
		enum lhp_level child_level = node->level - 1;

		/*
		 * Batch-unlink the whole fan-out (one counter subtract) and
		 * recycle the child array into the region cache instead of
		 * kvfree()ing it.  This avoids repeated alloc/free and, when the
		 * array lives in vmalloc, the unmap + TLB flush kvfree() incurs.
		 *
		 * If the cache is full, free the overflow with kvfree_atomic():
		 * lhp_free() (hence this path) may run in atomic context, so a
		 * vmalloc-backed array must not sleep in vfree() here.
		 */
		lhp_freelist_del_bulk(rg, node->children->nodes, LHP_FANOUT,
				      child_level);
		if (!lhp_cache_push(rg, node->children))
			kvfree_atomic(node->children);
		node->children = NULL;
		node->free_children = 0;
		node->state = LHP_FREE;

		if (node->level == LHP_LEVEL_1G) {
			/* Whole region merged back to a single free 1G. */
			rg->root_free = true;
			node = NULL;
		} else {
			lhp_freelist_add(rg, node);
			/* node just became free again -> bump parent's counter */
			parent->free_children++;
			node = parent;
		}
	}
}

/*
 * Obtain a free node of exactly @level within region @rg, splitting a higher
 * level if needed.  @res supplies preallocated child arrays for any splits.
 * Returns a free node (root for 1G, or a node still on a freelist for 2M/4K)
 * or NULL.  Caller holds the region lock.
 */
static struct lhp_node *lhp_get_free(struct lhp_region *rg, enum lhp_level level,
				     struct lhp_reservoir *res)
{
	if (level == LHP_LEVEL_1G)
		return rg->root_free ? rg->root : NULL;

	if (!list_empty(&rg->free_lists[level]))
		return list_first_entry(&rg->free_lists[level],
					struct lhp_node, list);

	/* Recursively make a parent, then split it. */
	{
		struct lhp_node *parent = lhp_get_free(rg, level + 1, res);

		if (!parent)
			return NULL;
		if (lhp_split_node(rg, parent, res))
			return NULL;
	}

	if (list_empty(&rg->free_lists[level]))
		return NULL;
	return list_first_entry(&rg->free_lists[level], struct lhp_node, list);
}

/*
 * Try to allocate one @level chunk from region @rg.  Returns the head page or
 * NULL.  Handles both the whole-1G case and the sub-1G split case, plus the
 * bookkeeping (state, parent free_children, per-region nr_alloc).
 * Caller holds the region lock.
 */
static struct page *lhp_region_alloc(struct lhp_region *rg, enum lhp_level level,
				     struct lhp_reservoir *res)
{
	struct lhp_node *n = lhp_get_free(rg, level, res);
	struct lhp_node *parent;

	if (!n)
		return NULL;

	if (level == LHP_LEVEL_1G) {
		rg->root_free = false;
		n->state = LHP_ALLOCATED;
		rg->nr_alloc[level]++;
		return n->base;
	}

	parent = n->parent;
	lhp_freelist_del(rg, n);
	n->state = LHP_ALLOCATED;
	if (parent)
		parent->free_children--;
	rg->nr_alloc[level]++;
	return n->base;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

int lhp_pool_available(void)
{
	/*
	 * Acquire load pairs with the smp_store_release() in lhp_pool_init():
	 * a caller that observes ready == true is guaranteed to also see the
	 * fully initialised regions array.
	 */
	return smp_load_acquire(&lhp_phys_pool.ready);
}

struct page *lhp_alloc(enum lhp_level level, gfp_t gfp)
{
	struct lhp_phys_pool *p = &lhp_phys_pool;
	struct lhp_reservoir res;
	struct page *page = NULL;
	unsigned long i, start;

	if (!lhp_pool_available() || level >= LHP_NR_LEVELS)
		return NULL;

	/*
	 * Preallocate the worst-case number of child arrays *before* taking any
	 * region lock, so the locked path never allocates.  A region's recycle
	 * cache usually satisfies splits, in which case these go straight into
	 * that region's cache via lhp_reservoir_drain() and no real allocation
	 * happened at all; the remainder is freed after unlocking.  @gfp lets an
	 * atomic-context caller pass a non-sleeping mask.
	 */
	if (lhp_reservoir_fill(&res, lhp_splits_needed(level), gfp))
		return NULL;

	/*
	 * Round-robin across regions from a shared cursor to spread contention,
	 * then linear-probe.  Only one region lock is held at a time, so CPUs
	 * hitting different regions never contend.  The cursor is read as an
	 * unsigned int so wrapping past INT_MAX keeps advancing the start index
	 * by one region rather than jumping after sign-extension.
	 */
	start = (unsigned int)atomic_fetch_inc(&p->cursor);
	for (i = 0; i < p->nr_regions; i++) {
		struct lhp_region *rg = &p->regions[(start + i) % p->nr_regions];

		spin_lock(&rg->lock);
		page = lhp_region_alloc(rg, level, &res);
		if (page)
			lhp_reservoir_drain(rg, &res);
		spin_unlock(&rg->lock);
		if (page)
			break;
	}

	lhp_reservoir_free(&res);	/* free any undrained remainder */
	return page;
}
EXPORT_SYMBOL_GPL(lhp_alloc);

/*
 * Find the region owning @page.  The regions[] array is immutable after init,
 * so this needs no lock.
 */
static struct lhp_region *lhp_find_region(struct lhp_phys_pool *p, struct page *page)
{
	unsigned long root_pages = lhp_level_nr_pages(LHP_LEVEL_1G);
	unsigned long i;

	for (i = 0; i < p->nr_regions; i++) {
		struct lhp_region *rg = &p->regions[i];

		if (page >= rg->base && page < rg->base + root_pages)
			return rg;
	}
	return NULL;
}

/*
 * Locate the allocated node backing @page at @level within region @rg.
 * O(depth): walk from the root down through SPLIT nodes by page offset.
 * Caller holds the region lock.
 */
static struct lhp_node *lhp_lookup(struct lhp_region *rg, struct page *page,
				   enum lhp_level level)
{
	struct lhp_node *n = rg->root;

	while (n) {
		if (n->level == level && n->base == page)
			return n;
		if (n->state != LHP_SPLIT)
			return NULL;
		{
			unsigned long child_pages =
				lhp_level_nr_pages(n->level - 1);
			unsigned long idx = (page - n->base) / child_pages;

			if (idx >= LHP_FANOUT)
				return NULL;
			n = &n->children->nodes[idx];
		}
	}
	return NULL;
}

void lhp_free(struct page *page, enum lhp_level level)
{
	struct lhp_phys_pool *p = &lhp_phys_pool;
	struct lhp_region *rg;
	struct lhp_node *n;

	if (!lhp_pool_available() || !page || level >= LHP_NR_LEVELS)
		return;

	rg = lhp_find_region(p, page);
	if (WARN_ONCE(!rg, "lhp_free: page %px not in any region\n", page))
		return;

	spin_lock(&rg->lock);
	n = lhp_lookup(rg, page, level);
	if (!n || n->state != LHP_ALLOCATED) {
		WARN_ONCE(1, "lhp_free: bad page %px level %d\n", page, level);
		goto out;
	}

	rg->nr_alloc[level]--;

	if (level == LHP_LEVEL_1G) {
		/* Whole-region free: root becomes an allocatable 1G again. */
		n->state = LHP_FREE;
		rg->root_free = true;
	} else {
		lhp_freelist_add(rg, n);
		if (n->parent) {
			n->parent->free_children++;
			lhp_try_merge(rg, n->parent);
		}
	}
out:
	spin_unlock(&rg->lock);
}
EXPORT_SYMBOL_GPL(lhp_free);

/* --------------------------------------------------------------------------
 * Pool init: pull 1G compound blocks out of the reserved CMA area
 * -------------------------------------------------------------------------- */

static void lhp_region_init(struct lhp_region *rg, struct page *base,
			    struct lhp_node *root)
{
	int level;

	spin_lock_init(&rg->lock);
	for (level = 0; level < LHP_NR_LEVELS; level++)
		INIT_LIST_HEAD(&rg->free_lists[level]);
	INIT_LIST_HEAD(&rg->array_cache);
	rg->base = base;
	rg->root = root;
	rg->root_free = true;
}

static int __init lhp_pool_init(void)
{
	struct lhp_phys_pool *p = &lhp_phys_pool;
	unsigned long nr_1g, i;

	atomic_set(&p->cursor, 0);

	if (!lhp_cma)
		return 0;

	nr_1g = cma_get_size(lhp_cma) / SZ_1G;
	if (!nr_1g)
		return 0;

	p->regions = kcalloc(nr_1g, sizeof(*p->regions), GFP_KERNEL);
	if (!p->regions)
		return -ENOMEM;

	p->cma = lhp_cma;

	for (i = 0; i < nr_1g; i++) {
		struct page *base;
		struct lhp_node *root;

		/* One 1G contiguous block == order LHP_ORDER_1G. */
		base = cma_alloc(lhp_cma, lhp_level_nr_pages(LHP_LEVEL_1G),
				 LHP_ORDER_1G, true);
		if (!base) {
			pr_warn("only obtained %lu/%lu 1G blocks\n", i, nr_1g);
			break;
		}

		root = lhp_node_alloc(base, LHP_LEVEL_1G, NULL);
		if (!root) {
			cma_release(lhp_cma, base,
				    lhp_level_nr_pages(LHP_LEVEL_1G));
			break;
		}
		lhp_region_init(&p->regions[i], base, root);
	}

	p->nr_regions = i;
	if (!p->nr_regions) {
		kfree(p->regions);
		p->regions = NULL;
		return 0;
	}

	/*
	 * Release store: publishes the fully initialised regions array to any
	 * consumer that observes ready == true via lhp_pool_available().
	 */
	smp_store_release(&p->ready, true);
	pr_info("pool ready: %lu x 1G regions (per-region locking)\n",
		p->nr_regions);
	return 0;
}
/* After buddy/CMA are fully up. */
late_initcall(lhp_pool_init);

/* --------------------------------------------------------------------------
 * DPDK-style layering: memzone -> obj pool -> buffer
 * -------------------------------------------------------------------------- */

#define LHP_BUFFER_MAGIC	0x4c485042U	/* "LHPB" */

enum lhp_buf_type {
	LHP_BUF_POOL_OBJ = 0,
	LHP_BUF_LARGE,
};

struct lhp_buffer_hdr {
	u32			magic;
	u32			user_size;
	struct lhp_obj_pool	*pool;
	u8			type;
	u8			_pad[3];
	union {
		struct {
			struct page	*page;
			enum lhp_level	level;
		} large;
	};
};

#define LHP_HDR_SIZE		ALIGN(sizeof(struct lhp_buffer_hdr), 8)

struct lhp_memzone_chunk {
	struct list_head	list;
	struct page		*base;
	enum lhp_level		level;
};

struct lhp_memzone {
	struct list_head	list;
	char			name[LHP_NAME_MAX];
	size_t			len;
	size_t			align;
	int			nid;
	unsigned long		flags;
	unsigned long		bytes_used;
	struct list_head	chunks;
	spinlock_t		lock;
	refcount_t		refs;
};

struct lhp_obj_pool {
	struct list_head	list;
	char			name[LHP_NAME_MAX];
	struct lhp_memzone	*zone;
	unsigned int		obj_size;
	unsigned int		obj_stride;
	unsigned int		align;
	unsigned long		flags;
	void			*free_list;
	unsigned long		nr_free;
	unsigned long		nr_alloc;
	struct page		*carve_page;
	unsigned int		carve_offset;
	spinlock_t		lock;
};

static LIST_HEAD(lhp_memzone_list);
static LIST_HEAD(lhp_obj_pool_list);
static DEFINE_MUTEX(lhp_registry_lock);

static inline void *lhp_buffer_user_ptr(struct lhp_buffer_hdr *hdr)
{
	return (char *)hdr + LHP_HDR_SIZE;
}

static inline struct lhp_buffer_hdr *lhp_buffer_from_user(const void *ptr)
{
	return (struct lhp_buffer_hdr *)((char *)ptr - LHP_HDR_SIZE);
}

static bool lhp_virt_in_phys_pool(const void *addr)
{
	struct lhp_phys_pool *p = &lhp_phys_pool;
	unsigned long root_pages = lhp_level_nr_pages(LHP_LEVEL_1G);
	unsigned long i;

	if (!lhp_pool_available() || !addr)
		return false;

	for (i = 0; i < p->nr_regions; i++) {
		struct page *base = p->regions[i].base;
		void *start = page_address(base);
		void *end = start + root_pages * PAGE_SIZE;

		if (addr >= start && addr < end)
			return true;
	}
	return false;
}

static enum lhp_level lhp_bytes_to_level(size_t bytes)
{
	unsigned long pages = DIV_ROUND_UP(bytes, PAGE_SIZE);

	if (pages <= lhp_level_nr_pages(LHP_LEVEL_4K))
		return LHP_LEVEL_4K;
	if (pages <= lhp_level_nr_pages(LHP_LEVEL_2M))
		return LHP_LEVEL_2M;
	return LHP_LEVEL_1G;
}

static unsigned long lhp_level_bytes(enum lhp_level level)
{
	return lhp_level_nr_pages(level) * PAGE_SIZE;
}

static int lhp_memzone_attach_chunk(struct lhp_memzone *mz,
				    struct page *page, enum lhp_level level,
				    gfp_t gfp)
{
	struct lhp_memzone_chunk *c;

	c = kmalloc(sizeof(*c), gfp);
	if (!c)
		return -ENOMEM;

	INIT_LIST_HEAD(&c->list);
	c->base = page;
	c->level = level;
	list_add_tail(&c->list, &mz->chunks);
	mz->bytes_used += lhp_level_bytes(level);
	return 0;
}

static void lhp_memzone_detach_chunk(struct lhp_memzone *mz,
				     struct lhp_memzone_chunk *c)
{
	list_del(&c->list);
	mz->bytes_used -= lhp_level_bytes(c->level);
	lhp_free(c->base, c->level);
	kfree(c);
}

static struct lhp_memzone_chunk *lhp_memzone_find_chunk(struct lhp_memzone *mz,
							struct page *page,
							enum lhp_level level)
{
	struct lhp_memzone_chunk *c;

	list_for_each_entry(c, &mz->chunks, list) {
		if (c->base == page && c->level == level)
			return c;
	}
	return NULL;
}

static int lhp_memzone_grow(struct lhp_memzone *mz, size_t need, gfp_t gfp)
{
	size_t remaining = need;
	int ret;

	while (remaining > 0) {
		enum lhp_level level;
		struct page *page;

		if (remaining > lhp_level_bytes(LHP_LEVEL_1G))
			level = LHP_LEVEL_1G;
		else if (remaining > lhp_level_bytes(LHP_LEVEL_2M))
			level = LHP_LEVEL_2M;
		else
			level = LHP_LEVEL_4K;

		page = lhp_alloc(level, gfp);
		if (!page)
			return -ENOMEM;

		ret = lhp_memzone_attach_chunk(mz, page, level, gfp);
		if (ret) {
			lhp_free(page, level);
			return ret;
		}

		remaining = (remaining > lhp_level_bytes(level)) ?
			remaining - lhp_level_bytes(level) : 0;
	}
	return 0;
}

static struct lhp_memzone *lhp_memzone_find_locked(const char *name)
{
	struct lhp_memzone *mz;

	list_for_each_entry(mz, &lhp_memzone_list, list) {
		if (!strcmp(mz->name, name))
			return mz;
	}
	return NULL;
}

struct lhp_memzone *lhp_memzone_lookup(const char *name)
{
	struct lhp_memzone *mz;

	if (!name)
		return NULL;

	mutex_lock(&lhp_registry_lock);
	mz = lhp_memzone_find_locked(name);
	mutex_unlock(&lhp_registry_lock);
	return mz;
}
EXPORT_SYMBOL_GPL(lhp_memzone_lookup);

struct lhp_memzone *lhp_memzone_reserve(const char *name, size_t len,
					size_t align, int nid,
					unsigned long flags)
{
	struct lhp_memzone *mz;
	size_t reserve_len;
	int ret;

	if (!lhp_pool_available() || !name || !*name || !len)
		return ERR_PTR(-EINVAL);
	if (strnlen(name, LHP_NAME_MAX) >= LHP_NAME_MAX)
		return ERR_PTR(-EINVAL);

	if (!align)
		align = PAGE_SIZE;
	if (!is_power_of_2(align) || align < PAGE_SIZE)
		return ERR_PTR(-EINVAL);

	reserve_len = ALIGN(len, PAGE_SIZE);

	mutex_lock(&lhp_registry_lock);
	if (lhp_memzone_find_locked(name)) {
		mutex_unlock(&lhp_registry_lock);
		return ERR_PTR(-EEXIST);
	}
	mutex_unlock(&lhp_registry_lock);

	mz = kzalloc(sizeof(*mz), GFP_KERNEL);
	if (!mz)
		return ERR_PTR(-ENOMEM);

	strscpy(mz->name, name, LHP_NAME_MAX);
	mz->len = reserve_len;
	mz->align = align;
	mz->nid = nid;
	mz->flags = flags;
	INIT_LIST_HEAD(&mz->chunks);
	spin_lock_init(&mz->lock);
	refcount_set(&mz->refs, 1);

	ret = lhp_memzone_grow(mz, reserve_len, GFP_KERNEL);
	if (ret) {
		struct lhp_memzone_chunk *c, *tmp;

		list_for_each_entry_safe(c, tmp, &mz->chunks, list)
			lhp_memzone_detach_chunk(mz, c);
		kfree(mz);
		return ERR_PTR(ret);
	}

	mutex_lock(&lhp_registry_lock);
	list_add_tail(&mz->list, &lhp_memzone_list);
	mutex_unlock(&lhp_registry_lock);

	pr_info("memzone %s: reserved %zu bytes (%lu chunks)\n",
		mz->name, reserve_len, mz->bytes_used / PAGE_SIZE);
	return mz;
}
EXPORT_SYMBOL_GPL(lhp_memzone_reserve);

void lhp_memzone_free(struct lhp_memzone *zone)
{
	struct lhp_memzone_chunk *c, *tmp;

	if (!zone)
		return;

	mutex_lock(&lhp_registry_lock);
	if (!refcount_dec_and_test(&zone->refs)) {
		mutex_unlock(&lhp_registry_lock);
		return;
	}
	list_del(&zone->list);
	mutex_unlock(&lhp_registry_lock);

	spin_lock(&zone->lock);
	list_for_each_entry_safe(c, tmp, &zone->chunks, list)
		lhp_memzone_detach_chunk(zone, c);
	spin_unlock(&zone->lock);

	kfree(zone);
}
EXPORT_SYMBOL_GPL(lhp_memzone_free);

static struct lhp_obj_pool *lhp_obj_pool_find_locked(const char *name)
{
	struct lhp_obj_pool *pool;

	list_for_each_entry(pool, &lhp_obj_pool_list, list) {
		if (!strcmp(pool->name, name))
			return pool;
	}
	return NULL;
}

struct lhp_obj_pool *lhp_pool_lookup(const char *name)
{
	struct lhp_obj_pool *pool;

	if (!name)
		return NULL;

	mutex_lock(&lhp_registry_lock);
	pool = lhp_obj_pool_find_locked(name);
	mutex_unlock(&lhp_registry_lock);
	return pool;
}
EXPORT_SYMBOL_GPL(lhp_pool_lookup);

struct lhp_obj_pool *lhp_pool_create(const char *name,
				     struct lhp_memzone *zone,
				     unsigned int obj_size,
				     unsigned int align,
				     unsigned long flags)
{
	struct lhp_obj_pool *pool;
	unsigned int stride;

	if (!lhp_pool_available() || !name || !*name || !zone || !obj_size)
		return ERR_PTR(-EINVAL);
	if (strnlen(name, LHP_NAME_MAX) >= LHP_NAME_MAX)
		return ERR_PTR(-EINVAL);

	if (!align)
		align = 8;
	if (!is_power_of_2(align))
		return ERR_PTR(-EINVAL);

	if (obj_size > zone->len)
		return ERR_PTR(-EINVAL);

	stride = LHP_HDR_SIZE + ALIGN(obj_size, align);
	if (stride > PAGE_SIZE)
		return ERR_PTR(-E2BIG);

	mutex_lock(&lhp_registry_lock);
	if (lhp_obj_pool_find_locked(name)) {
		mutex_unlock(&lhp_registry_lock);
		return ERR_PTR(-EEXIST);
	}
	mutex_unlock(&lhp_registry_lock);

	pool = kzalloc(sizeof(*pool), GFP_KERNEL);
	if (!pool)
		return ERR_PTR(-ENOMEM);

	strscpy(pool->name, name, LHP_NAME_MAX);
	pool->zone = zone;
	pool->obj_size = obj_size;
	pool->obj_stride = stride;
	pool->align = align;
	pool->flags = flags;
	spin_lock_init(&pool->lock);
	refcount_inc(&zone->refs);

	mutex_lock(&lhp_registry_lock);
	list_add_tail(&pool->list, &lhp_obj_pool_list);
	mutex_unlock(&lhp_registry_lock);

	pr_info("pool %s: obj_size=%u stride=%u zone=%s\n",
		pool->name, obj_size, stride, zone->name);
	return pool;
}
EXPORT_SYMBOL_GPL(lhp_pool_create);

void lhp_pool_destroy(struct lhp_obj_pool *pool)
{
	if (!pool)
		return;

	mutex_lock(&lhp_registry_lock);
	list_del(&pool->list);
	mutex_unlock(&lhp_registry_lock);

	if (pool->nr_alloc)
		WARN_ONCE(1, "lhp_pool_destroy: %s has %lu outstanding objects\n",
			  pool->name, pool->nr_alloc);

	refcount_dec(&pool->zone->refs);
	kfree(pool);
}
EXPORT_SYMBOL_GPL(lhp_pool_destroy);

static int lhp_pool_new_carve_page(struct lhp_obj_pool *pool, gfp_t gfp)
{
	struct page *page;
	int ret;

	page = lhp_alloc(LHP_LEVEL_4K, gfp);
	if (!page)
		return -ENOMEM;

	ret = lhp_memzone_attach_chunk(pool->zone, page, LHP_LEVEL_4K, gfp);
	if (ret) {
		lhp_free(page, LHP_LEVEL_4K);
		return ret;
	}

	pool->carve_page = page;
	pool->carve_offset = 0;
	return 0;
}

static void lhp_buffer_init(struct lhp_buffer_hdr *hdr,
			    struct lhp_obj_pool *pool,
			    enum lhp_buf_type type, u32 user_size)
{
	hdr->magic = LHP_BUFFER_MAGIC;
	hdr->user_size = user_size;
	hdr->pool = pool;
	hdr->type = type;
}

static void *lhp_pool_alloc_locked(struct lhp_obj_pool *pool, gfp_t gfp)
{
	struct lhp_buffer_hdr *hdr;
	void *obj;

	if (pool->free_list) {
		obj = pool->free_list;
		pool->free_list = *(void **)obj;
		pool->nr_free--;
		pool->nr_alloc++;
		return obj;
	}

	if (!pool->carve_page ||
	    pool->carve_offset + pool->obj_stride > PAGE_SIZE) {
		if (lhp_pool_new_carve_page(pool, gfp))
			return NULL;
	}

	hdr = page_address(pool->carve_page) + pool->carve_offset;
	pool->carve_offset += pool->obj_stride;
	lhp_buffer_init(hdr, pool, LHP_BUF_POOL_OBJ, pool->obj_size);

	pool->nr_alloc++;
	return lhp_buffer_user_ptr(hdr);
}

void *lhp_pool_alloc(struct lhp_obj_pool *pool, gfp_t gfp)
{
	void *ptr;

	if (!pool)
		return NULL;

	spin_lock(&pool->lock);
	ptr = lhp_pool_alloc_locked(pool, gfp);
	spin_unlock(&pool->lock);

	if (ptr && (gfp & __GFP_ZERO))
		memset(ptr, 0, pool->obj_size);

	return ptr;
}
EXPORT_SYMBOL_GPL(lhp_pool_alloc);

void lhp_pool_free(struct lhp_obj_pool *pool, void *ptr)
{
	struct lhp_buffer_hdr *hdr;

	if (!pool || !ptr)
		return;

	if (!lhp_virt_in_phys_pool(ptr))
		goto bad;

	hdr = lhp_buffer_from_user(ptr);
	if (hdr->magic != LHP_BUFFER_MAGIC || hdr->pool != pool ||
	    hdr->type != LHP_BUF_POOL_OBJ)
		goto bad;

	spin_lock(&pool->lock);
	*(void **)ptr = pool->free_list;
	pool->free_list = ptr;
	pool->nr_free++;
	pool->nr_alloc--;
	spin_unlock(&pool->lock);
	return;

bad:
	WARN_ONCE(1, "lhp_pool_free: bad ptr %px for pool %s\n", ptr, pool->name);
}
EXPORT_SYMBOL_GPL(lhp_pool_free);

static void *lhp_kmalloc_large(struct lhp_obj_pool *pool, size_t size, gfp_t gfp)
{
	struct lhp_memzone *mz = pool->zone;
	enum lhp_level level = lhp_bytes_to_level(size + LHP_HDR_SIZE);
	struct lhp_buffer_hdr *hdr;
	struct page *page;
	int ret;

	page = lhp_alloc(level, gfp);
	if (!page)
		return NULL;

	ret = lhp_memzone_attach_chunk(mz, page, level, gfp);
	if (ret) {
		lhp_free(page, level);
		return NULL;
	}

	hdr = page_address(page);
	lhp_buffer_init(hdr, pool, LHP_BUF_LARGE, size);
	hdr->large.page = page;
	hdr->large.level = level;

	if (gfp & __GFP_ZERO)
		memset(lhp_buffer_user_ptr(hdr), 0, size);

	return lhp_buffer_user_ptr(hdr);
}

void *lhp_kmalloc(struct lhp_obj_pool *pool, size_t size, gfp_t gfp)
{
	void *ptr;

	if (!pool)
		return NULL;
	if (!size)
		return ZERO_SIZE_PTR;
	if (size > pool->zone->len)
		return NULL;

	if (size <= pool->obj_size)
		ptr = lhp_pool_alloc(pool, gfp);
	else
		ptr = lhp_kmalloc_large(pool, size, gfp);

	if (ptr && size <= pool->obj_size) {
		struct lhp_buffer_hdr *hdr = lhp_buffer_from_user(ptr);

		hdr->user_size = size;
	}

	return ptr;
}
EXPORT_SYMBOL_GPL(lhp_kmalloc);

void lhp_kfree(const void *ptr)
{
	struct lhp_buffer_hdr *hdr;
	struct lhp_obj_pool *pool;
	struct lhp_memzone *mz;
	struct lhp_memzone_chunk *c;

	if (unlikely(ZERO_OR_NULL_PTR(ptr)))
		return;

	if (!lhp_virt_in_phys_pool(ptr)) {
		WARN_ONCE(1, "lhp_kfree: %px not in LHP phys pool\n", ptr);
		return;
	}

	hdr = lhp_buffer_from_user(ptr);
	if (hdr->magic != LHP_BUFFER_MAGIC) {
		WARN_ONCE(1, "lhp_kfree: %px is not an LHP buffer (use kfree?)\n",
			  ptr);
		return;
	}

	pool = hdr->pool;
	if (!pool)
		return;

	if (hdr->type == LHP_BUF_POOL_OBJ) {
		lhp_pool_free(pool, (void *)ptr);
		return;
	}

	mz = pool->zone;
	spin_lock(&mz->lock);
	c = lhp_memzone_find_chunk(mz, hdr->large.page, hdr->large.level);
	if (c)
		lhp_memzone_detach_chunk(mz, c);
	spin_unlock(&mz->lock);
}
EXPORT_SYMBOL_GPL(lhp_kfree);

bool lhp_ptr_is_owned(const void *ptr)
{
	const struct lhp_buffer_hdr *hdr;

	if (unlikely(ZERO_OR_NULL_PTR(ptr)) || !lhp_virt_in_phys_pool(ptr))
		return false;

	hdr = lhp_buffer_from_user(ptr);
	return hdr->magic == LHP_BUFFER_MAGIC;
}
EXPORT_SYMBOL_GPL(lhp_ptr_is_owned);

/*
 * SLUB integration evaluation (phase 4, not implemented).
 *
 * To back a kmem_cache with LHP:
 *   1. Add struct lhp_obj_pool *lhp_backing to struct kmem_cache (or a flag).
 *   2. In alloc_slab_page(), if cache->lhp_backing, call
 *      lhp_alloc() at oo_order instead of alloc_frozen_pages().
 *   3. Mark slab pages with a new page type or page->lhp_pool pointer.
 *   4. In __free_slab(), route to lhp_free() instead of free_frozen_pages().
 *   5. memcg/KASAN/kmemleak hooks must mirror the buddy path.
 *
 * Until then, subsystem code should use lhp_pool_create() +
 * kmem_cache-like lhp_pool_alloc() explicitly.
 */
struct lhp_obj_pool *
lhp_pool_for_slub_cache(const struct kmem_cache *s)
{
	return NULL;
}
EXPORT_SYMBOL_GPL(lhp_pool_for_slub_cache);

/* --------------------------------------------------------------------------
 * debugfs: observe and drive the pool
 *
 *   /sys/kernel/debug/lhp/stats   - read aggregated per-region counters
 *   /sys/kernel/debug/lhp/alloc   - write "0|1|2" (4K|2M|1G) to test-alloc
 *   /sys/kernel/debug/lhp/bench   - write "<ops_per_thread> [threads]" to run
 *                                   the (optionally concurrent) microbenchmark
 * -------------------------------------------------------------------------- */

#ifdef CONFIG_DEBUG_FS
static const char *lhp_level_name(enum lhp_level l)
{
	switch (l) {
	case LHP_LEVEL_1G: return "1G";
	case LHP_LEVEL_2M: return "2M";
	default:	   return "4K";
	}
}

static int lhp_stats_show(struct seq_file *m, void *v)
{
	struct lhp_phys_pool *p = &lhp_phys_pool;
	unsigned long nr_free[LHP_NR_LEVELS] = {};
	unsigned long nr_alloc[LHP_NR_LEVELS] = {};
	unsigned long cached = 0, free_1g = 0, i;
	struct lhp_memzone *mz;
	struct lhp_obj_pool *pool;
	int l;

	if (!smp_load_acquire(&p->ready)) {
		seq_puts(m, "ready: 0\n");
		return 0;
	}

	/* Sum per-region counters, briefly locking each region in turn. */
	for (i = 0; i < p->nr_regions; i++) {
		struct lhp_region *rg = &p->regions[i];

		spin_lock(&rg->lock);
		if (rg->root_free)
			free_1g++;
		cached += rg->nr_cached;
		for (l = 0; l < LHP_NR_LEVELS; l++) {
			nr_free[l] += rg->nr_free[l];
			nr_alloc[l] += rg->nr_alloc[l];
		}
		spin_unlock(&rg->lock);
	}

	seq_printf(m, "ready: %d\nregions: %lu\nfree 1G regions: %lu\narray cache: %lu\n",
		   p->ready, p->nr_regions, free_1g, cached);
	/* 1G "free" is the number of wholly-idle regions. */
	nr_free[LHP_LEVEL_1G] = free_1g;
	for (l = LHP_NR_LEVELS - 1; l >= 0; l--)
		seq_printf(m, "phys %s: free=%lu alloc=%lu\n",
			   lhp_level_name(l), nr_free[l], nr_alloc[l]);

	mutex_lock(&lhp_registry_lock);
	seq_puts(m, "\nmemzones:\n");
	list_for_each_entry(mz, &lhp_memzone_list, list) {
		seq_printf(m, "  %s: len=%zu used=%lu chunks=%u refs=%d\n",
			   mz->name, mz->len, mz->bytes_used,
			   (unsigned int)list_count_nodes(&mz->chunks),
			   refcount_read(&mz->refs));
	}
	seq_puts(m, "pools:\n");
	list_for_each_entry(pool, &lhp_obj_pool_list, list) {
		seq_printf(m, "  %s: zone=%s obj_size=%u free=%lu alloc=%lu\n",
			   pool->name, pool->zone->name, pool->obj_size,
			   pool->nr_free, pool->nr_alloc);
	}
	mutex_unlock(&lhp_registry_lock);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(lhp_stats);

static ssize_t lhp_alloc_write(struct file *file, const char __user *ubuf,
			       size_t count, loff_t *ppos)
{
	unsigned int level;
	struct page *pg;
	int ret;

	ret = kstrtouint_from_user(ubuf, count, 0, &level);
	if (ret)
		return ret;
	if (level >= LHP_NR_LEVELS)
		return -EINVAL;

	pg = lhp_alloc(level, GFP_KERNEL);
	if (!pg)
		return -ENOMEM;

	pr_info("debugfs alloc %s -> pfn %lu\n",
		lhp_level_name(level), page_to_pfn(pg));
	return count;
}

static const struct file_operations lhp_alloc_fops = {
	.write = lhp_alloc_write,
	.open = simple_open,
	.llseek = default_llseek,
};

/*
 * Microbenchmark, with optional multi-threaded concurrency to measure lock
 * contention.  Write "<ops_per_thread> [threads]" to lhp/bench.  Each thread
 * repeatedly allocates then immediately frees a 4K chunk @ops_per_thread times,
 * driving the full 1G->2M->4K split + merge machinery and the array cache.
 * Running T threads on T CPUs exercises the per-region locking: with more
 * regions than threads and round-robin region selection, threads mostly hit
 * different regions and should scale close to linearly.
 *
 * Because plan B never copies page data, this isolates the pure metadata cost
 * and the lock-contention behaviour of split/merge.
 */
#define LHP_BENCH_MAX_THREADS	64

struct lhp_bench_thread {
	struct task_struct	*task;
	unsigned long		ops;		/* iterations to run */
	unsigned long		done;		/* iterations completed */
	u64			ns;		/* per-thread wall time */
	struct completion	start;
	struct completion	end;
};

struct lhp_bench_result {
	unsigned long	ops_per_thread;
	unsigned int	threads;
	unsigned long	total_ops;
	u64		wall_ns;	/* max thread wall time (parallel) */
	u64		sum_ns;		/* sum of thread times (total CPU) */
	bool		valid;
};
static struct lhp_bench_result lhp_bench_last;

static int lhp_bench_fn(void *arg)
{
	struct lhp_bench_thread *t = arg;
	unsigned long i;
	ktime_t t0, t1;

	wait_for_completion(&t->start);

	t0 = ktime_get();
	for (i = 0; i < t->ops; i++) {
		struct page *pg = lhp_alloc(LHP_LEVEL_4K, GFP_KERNEL);

		if (!pg)
			break;
		lhp_free(pg, LHP_LEVEL_4K);
	}
	t1 = ktime_get();

	t->done = i;
	t->ns = ktime_to_ns(ktime_sub(t1, t0));
	complete(&t->end);

	/* Park until kthread_stop() so the task_struct stays valid. */
	while (!kthread_should_stop())
		schedule_timeout_interruptible(HZ / 10);
	return 0;
}

static int lhp_bench_run(unsigned long ops_per_thread, unsigned int threads)
{
	struct lhp_bench_thread *t;
	unsigned int i, launched = 0;
	u64 wall = 0, sum = 0;
	unsigned long total_done = 0;

	/*
	 * Cap at one thread per online CPU so each thread gets a distinct CPU
	 * via kthread_bind() below.  Otherwise threads would share CPUs and the
	 * reported "parallel /op" scaling number would be skewed.
	 */
	threads = min(threads, num_online_cpus());

	t = kcalloc(threads, sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;

	for (i = 0; i < threads; i++) {
		t[i].ops = ops_per_thread;
		init_completion(&t[i].start);
		init_completion(&t[i].end);
		t[i].task = kthread_create(lhp_bench_fn, &t[i],
					   "lhp_bench/%u", i);
		if (IS_ERR(t[i].task))
			break;
		kthread_bind(t[i].task, i);
		wake_up_process(t[i].task);
		launched++;
	}
	if (!launched) {
		kfree(t);
		return -EAGAIN;
	}

	/* Release all threads as simultaneously as possible. */
	for (i = 0; i < launched; i++)
		complete(&t[i].start);

	for (i = 0; i < launched; i++) {
		wait_for_completion(&t[i].end);
		wall = max(wall, t[i].ns);
		sum += t[i].ns;
		total_done += t[i].done;
	}

	for (i = 0; i < launched; i++)
		kthread_stop(t[i].task);

	lhp_bench_last.ops_per_thread = ops_per_thread;
	lhp_bench_last.threads = launched;
	lhp_bench_last.total_ops = total_done;
	lhp_bench_last.wall_ns = wall;
	lhp_bench_last.sum_ns = sum;
	lhp_bench_last.valid = true;

	pr_info("bench: %u thread(s) x %lu ops  wall=%llu ns  %llu ns/op (parallel)  %llu ns/op (per-cpu)\n",
		launched, ops_per_thread, wall,
		total_done ? wall * launched / total_done : 0,
		total_done ? sum / total_done : 0);

	kfree(t);
	return 0;
}

static ssize_t lhp_bench_write(struct file *file, const char __user *ubuf,
			       size_t count, loff_t *ppos)
{
	char buf[64];
	unsigned long ops;
	unsigned int threads = 1;
	int ret;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	ret = sscanf(buf, "%lu %u", &ops, &threads);
	if (ret < 1)
		return -EINVAL;
	if (!ops || ops > (1UL << 22))
		return -EINVAL;
	if (threads < 1 || threads > LHP_BENCH_MAX_THREADS)
		return -EINVAL;

	ret = lhp_bench_run(ops, threads);
	if (ret)
		return ret;
	return count;
}

static int lhp_bench_show(struct seq_file *m, void *v)
{
	struct lhp_bench_result r = lhp_bench_last;

	if (!r.valid) {
		seq_puts(m, "no run yet; write \"<ops_per_thread> [threads]\"\n");
		return 0;
	}
	seq_printf(m, "threads:        %u\n", r.threads);
	seq_printf(m, "ops/thread:     %lu\n", r.ops_per_thread);
	seq_printf(m, "total ops done: %lu\n", r.total_ops);
	seq_printf(m, "wall time:      %llu ns\n", r.wall_ns);
	seq_printf(m, "parallel /op:   %llu ns\n",
		   r.total_ops ? r.wall_ns * r.threads / r.total_ops : 0);
	seq_printf(m, "per-cpu /op:    %llu ns\n",
		   r.total_ops ? r.sum_ns / r.total_ops : 0);
	return 0;
}

static int lhp_bench_open(struct inode *inode, struct file *file)
{
	return single_open(file, lhp_bench_show, NULL);
}

static const struct file_operations lhp_bench_fops = {
	.open = lhp_bench_open,
	.read = seq_read,
	.write = lhp_bench_write,
	.llseek = seq_lseek,
	.release = single_release,
};

/*
 * Layer self-test via debugfs.  Write "run" to exercise memzone -> pool ->
 * buffer alloc/free and merge.
 */
static int lhp_layer_test_run(void)
{
	struct lhp_memzone *mz;
	struct lhp_obj_pool *pool;
	void *small[8], *large = NULL;
	unsigned int i, n_small = 0;
	int err = 0;

	mz = lhp_memzone_lookup("dbgtest");
	if (mz)
		lhp_memzone_free(mz);
	pool = lhp_pool_lookup("dbgpool");
	if (pool)
		lhp_pool_destroy(pool);

	mz = lhp_memzone_reserve("dbgtest", 2 * SZ_2M, PAGE_SIZE,
				   NUMA_NO_NODE, LHP_MEMZONE_F_NONE);
	if (IS_ERR(mz)) {
		pr_err("layer_test: memzone_reserve failed %ld\n", PTR_ERR(mz));
		return PTR_ERR(mz);
	}

	pool = lhp_pool_create("dbgpool", mz, 64, 8, LHP_POOL_F_NONE);
	if (IS_ERR(pool)) {
		pr_err("layer_test: pool_create failed %ld\n", PTR_ERR(pool));
		lhp_memzone_free(mz);
		return PTR_ERR(pool);
	}

	for (i = 0; i < ARRAY_SIZE(small); i++) {
		small[i] = lhp_kmalloc(pool, 32, GFP_KERNEL);
		if (!small[i]) {
			err = -ENOMEM;
			n_small = i;
			goto out;
		}
		if (!lhp_ptr_is_owned(small[i])) {
			err = -EFAULT;
			n_small = i + 1;
			goto out;
		}
	}
	n_small = ARRAY_SIZE(small);

	large = lhp_kmalloc(pool, 8192, GFP_KERNEL);
	if (!large) {
		err = -ENOMEM;
		goto out;
	}

	if (!err)
		pr_info("layer_test: memzone/pool/kmalloc cycle ok\n");

out:
	for (i = 0; i < n_small; i++)
		lhp_kfree(small[i]);
	if (large)
		lhp_kfree(large);
	lhp_pool_destroy(pool);
	lhp_memzone_free(mz);
	return err;
}

static ssize_t lhp_layer_test_write(struct file *file, const char __user *ubuf,
				    size_t count, loff_t *ppos)
{
	char buf[16];
	int ret;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	if (sysfs_streq(buf, "run")) {
		ret = lhp_layer_test_run();
		if (ret)
			return ret;
	}
	return count;
}

static const struct file_operations lhp_layer_test_fops = {
	.write = lhp_layer_test_write,
	.open = simple_open,
	.llseek = default_llseek,
};

static int __init lhp_debugfs_init(void)
{
	struct dentry *dir;

	dir = debugfs_create_dir("lhp", NULL);
	debugfs_create_file("stats", 0400, dir, NULL, &lhp_stats_fops);
	debugfs_create_file("alloc", 0200, dir, NULL, &lhp_alloc_fops);
	debugfs_create_file("bench", 0600, dir, NULL, &lhp_bench_fops);
	debugfs_create_file("layer_test", 0200, dir, NULL, &lhp_layer_test_fops);
	return 0;
}
late_initcall(lhp_debugfs_init);
#endif /* CONFIG_DEBUG_FS */

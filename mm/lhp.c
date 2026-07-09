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
 * This is an intentionally self-contained skeleton: it implements the data
 * structures and the split/merge state machine, exposes a debugfs interface to
 * drive and observe it, but does not (yet) wire itself into any real kernel
 * allocation path (e.g. page-table page allocation).  That is a follow-up.
 */
#define pr_fmt(fmt) "lhp: " fmt

#include <linux/lhp.h>
#include <linux/cma.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/init.h>
#include <linux/sizes.h>
#include <linux/memblock.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/ktime.h>

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
 * The pool.  A single global instance for now.  free_lists[level] holds all
 * LHP_FREE nodes of that level, newest-first.
 */
struct lhp_pool {
	struct cma		*cma;
	unsigned long		nr_1g;			/* # of 1G roots */
	struct lhp_node		**roots;		/* [nr_1g] */
	struct list_head	free_lists[LHP_NR_LEVELS];
	unsigned long		nr_free[LHP_NR_LEVELS];
	unsigned long		nr_alloc[LHP_NR_LEVELS];
	/* Recycled child-array cache (see struct lhp_child_array). */
	struct list_head	array_cache;
	unsigned long		nr_cached;
	struct mutex		lock;
	bool			ready;
};

static struct lhp_pool lhp_pool;

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

static void lhp_freelist_add(struct lhp_pool *p, struct lhp_node *n)
{
	n->state = LHP_FREE;
	list_add(&n->list, &p->free_lists[n->level]);
	p->nr_free[n->level]++;
}

static void lhp_freelist_del(struct lhp_pool *p, struct lhp_node *n)
{
	list_del_init(&n->list);
	p->nr_free[n->level]--;
}

/*
 * Bulk-add a whole fan-out of @count nodes (all of the same @level) to their
 * freelist in one O(1) list_splice plus a single counter update, instead of
 * @count individual list_add + increments.
 */
static void lhp_freelist_add_bulk(struct lhp_pool *p, struct lhp_node *nodes,
				  unsigned int count, enum lhp_level level)
{
	LIST_HEAD(batch);
	unsigned int i;

	for (i = 0; i < count; i++) {
		nodes[i].state = LHP_FREE;
		list_add_tail(&nodes[i].list, &batch);
	}
	list_splice_tail(&batch, &p->free_lists[level]);
	p->nr_free[level] += count;
}

/*
 * Bulk-remove a whole fan-out of @count nodes from their freelist.  The nodes
 * are contiguous (one allocation) so we just unlink each and drop the counter
 * once.  Unlinking is required because merge frees the backing array afterwards.
 */
static void lhp_freelist_del_bulk(struct lhp_pool *p, struct lhp_node *nodes,
				  unsigned int count, enum lhp_level level)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		list_del_init(&nodes[i].list);
	p->nr_free[level] -= count;
}

/* --------------------------------------------------------------------------
 * Child-array cache + prealloc reservoir
 *
 * kvcalloc() may sleep and even trigger reclaim, so we never call it under the
 * pool lock.  Instead lhp_alloc() pre-allocates enough arrays into a small
 * on-stack reservoir *before* taking the lock; the split path consumes from the
 * reservoir, and any leftovers are pushed into the pool's recycle cache (also
 * outside the lock).  Merge returns arrays to the cache instead of freeing them.
 * -------------------------------------------------------------------------- */

/* A tiny stack-local stash of preallocated arrays handed into the locked path. */
struct lhp_reservoir {
	struct lhp_child_array	*arr[LHP_MAX_SPLIT_DEPTH];
	unsigned int		nr;
};

static struct lhp_child_array *lhp_array_alloc(void)
{
	return kvzalloc(sizeof(struct lhp_child_array), GFP_KERNEL);
}

/* Pop a recycled array from the cache (caller holds lock), or NULL if empty. */
static struct lhp_child_array *lhp_cache_pop(struct lhp_pool *p)
{
	struct lhp_child_array *a;

	a = list_first_entry_or_null(&p->array_cache,
				     struct lhp_child_array, cache);
	if (a) {
		list_del(&a->cache);
		p->nr_cached--;
	}
	return a;
}

/* Push an array into the cache (caller holds lock). */
static void lhp_cache_push(struct lhp_pool *p, struct lhp_child_array *a)
{
	list_add(&a->cache, &p->array_cache);
	p->nr_cached++;
}

/*
 * Fill @r with @need freshly allocated arrays, outside the lock.  Returns 0 or
 * -ENOMEM (freeing anything it managed to grab).  We deliberately over-provision
 * from the allocator rather than peek at the cache, since the cache can only be
 * inspected under the lock; unused arrays are recycled into the cache later.
 */
static int lhp_reservoir_fill(struct lhp_reservoir *r, unsigned int need)
{
	r->nr = 0;
	while (r->nr < need) {
		struct lhp_child_array *a = lhp_array_alloc();

		if (!a) {
			while (r->nr)
				kvfree(r->arr[--r->nr]);
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
 * Drain leftover reservoir arrays into the pool cache (caller holds lock).
 * Keeps the cache from growing without bound by capping it.
 */
#define LHP_ARRAY_CACHE_MAX	64
static void lhp_reservoir_drain(struct lhp_pool *p, struct lhp_reservoir *r)
{
	while (r->nr) {
		struct lhp_child_array *a = r->arr[--r->nr];

		if (p->nr_cached < LHP_ARRAY_CACHE_MAX)
			lhp_cache_push(p, a);
		else
			kvfree(a);	/* rare: cache full, drop excess */
	}
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
 * The node moves FREE -> SPLIT.  The backing child array is taken from @res
 * (preallocated outside the lock) or, failing that, the pool recycle cache.
 * Returns 0 or -ENOMEM.  Caller holds the pool lock.
 */
static int lhp_split_node(struct lhp_pool *p, struct lhp_node *node,
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
	 * No allocation here: pull a recycled array from the cache, else one the
	 * caller preallocated outside the lock.  This keeps the locked critical
	 * section allocation-free (no sleeping, no reclaim) and reuses arrays
	 * across split/merge cycles instead of kvcalloc/kvfree churn.
	 */
	children = lhp_cache_pop(p);
	if (!children)
		children = lhp_reservoir_take(res);
	if (!children)
		return -ENOMEM;

	for (i = 0; i < LHP_FANOUT; i++)
		lhp_node_init(&children->nodes[i],
			      node->base + i * child_pages, child_level, node);

	/* Commit: publish children and move parent to SPLIT. */
	lhp_freelist_del(p, node);
	node->children = children;
	node->free_children = LHP_FANOUT;
	node->state = LHP_SPLIT;

	/* Batch-insert all LHP_FANOUT children in one splice + one counter add. */
	lhp_freelist_add_bulk(p, children->nodes, LHP_FANOUT, child_level);

	return 0;
}

/*
 * If every child of a SPLIT node is free, collapse it back: free the children
 * and return the node to its own freelist as FREE.  Recurses upward so a single
 * 4K free can bubble all the way to a reconstituted 1G.
 */
static void lhp_try_merge(struct lhp_pool *p, struct lhp_node *node)
{
	while (node && node->state == LHP_SPLIT &&
	       node->free_children == LHP_FANOUT) {
		struct lhp_node *parent = node->parent;
		enum lhp_level child_level = node->level - 1;

		/*
		 * Batch-unlink the whole fan-out (one counter subtract) and
		 * recycle the child array into the cache instead of kvfree()ing
		 * it.  This avoids repeated alloc/free and, when the array lives
		 * in vmalloc, the unmap + TLB flush that kvfree() would trigger.
		 */
		lhp_freelist_del_bulk(p, node->children->nodes, LHP_FANOUT,
				      child_level);
		lhp_cache_push(p, node->children);
		node->children = NULL;
		node->free_children = 0;

		lhp_freelist_add(p, node);

		if (parent) {
			/* node just became free again -> bump parent's counter */
			parent->free_children++;
			node = parent;
		} else {
			node = NULL;
		}
	}
}

/*
 * Obtain a free node of exactly @level, splitting a higher level if needed.
 * @res supplies preallocated child arrays for any splits.  Returns a node in
 * state LHP_FREE (still on its freelist) or NULL.  Caller holds the pool lock.
 */
static struct lhp_node *lhp_get_free(struct lhp_pool *p, enum lhp_level level,
				     struct lhp_reservoir *res)
{
	if (!list_empty(&p->free_lists[level]))
		return list_first_entry(&p->free_lists[level],
					struct lhp_node, list);

	if (level == LHP_LEVEL_1G)
		return NULL;	/* nothing bigger to split */

	/* Recursively make a parent, then split it. */
	{
		struct lhp_node *parent = lhp_get_free(p, level + 1, res);

		if (!parent)
			return NULL;
		if (lhp_split_node(p, parent, res))
			return NULL;
	}

	if (list_empty(&p->free_lists[level]))
		return NULL;
	return list_first_entry(&p->free_lists[level], struct lhp_node, list);
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

int lhp_pool_available(void)
{
	return READ_ONCE(lhp_pool.ready);
}

struct page *lhp_alloc(enum lhp_level level)
{
	struct lhp_pool *p = &lhp_pool;
	struct lhp_reservoir res;
	struct lhp_node *n;
	struct page *page = NULL;

	if (!lhp_pool_available() || level >= LHP_NR_LEVELS)
		return NULL;

	/*
	 * Preallocate the worst-case number of child arrays *before* taking the
	 * lock, so the locked path never allocates.  The recycle cache usually
	 * satisfies splits, in which case these go straight back to the cache
	 * via lhp_reservoir_drain() and no real allocation happened at all.
	 */
	if (lhp_reservoir_fill(&res, lhp_splits_needed(level)))
		return NULL;

	mutex_lock(&p->lock);
	n = lhp_get_free(p, level, &res);
	if (n) {
		struct lhp_node *parent = n->parent;

		lhp_freelist_del(p, n);
		n->state = LHP_ALLOCATED;
		if (parent)
			parent->free_children--;
		p->nr_alloc[level]++;
		page = n->base;
	}
	lhp_reservoir_drain(p, &res);
	mutex_unlock(&p->lock);
	return page;
}
EXPORT_SYMBOL_GPL(lhp_alloc);

/*
 * Locate the allocated node backing @page at @level.  O(depth): walk from the
 * owning root down through SPLIT nodes by page offset.  Kept simple on purpose;
 * a production version would hash page->private or similar.
 */
static struct lhp_node *lhp_lookup(struct lhp_pool *p, struct page *page,
				   enum lhp_level level)
{
	unsigned long i;

	for (i = 0; i < p->nr_1g; i++) {
		struct lhp_node *n = p->roots[i];
		unsigned long root_pages = lhp_level_nr_pages(LHP_LEVEL_1G);

		if (page < n->base || page >= n->base + root_pages)
			continue;

		while (n) {
			if (n->level == level && n->base == page)
				return n;
			if (n->state != LHP_SPLIT)
				return NULL;
			{
				unsigned long child_pages =
					lhp_level_nr_pages(n->level - 1);
				unsigned long idx =
					(page - n->base) / child_pages;

				if (idx >= LHP_FANOUT)
					return NULL;
				n = &n->children->nodes[idx];
			}
		}
		return NULL;
	}
	return NULL;
}

void lhp_free(struct page *page, enum lhp_level level)
{
	struct lhp_pool *p = &lhp_pool;
	struct lhp_node *n;

	if (!lhp_pool_available() || !page || level >= LHP_NR_LEVELS)
		return;

	mutex_lock(&p->lock);
	n = lhp_lookup(p, page, level);
	if (!n || n->state != LHP_ALLOCATED) {
		WARN_ONCE(1, "lhp_free: bad page %px level %d\n", page, level);
		goto out;
	}

	lhp_freelist_add(p, n);
	p->nr_alloc[level]--;
	if (n->parent) {
		n->parent->free_children++;
		lhp_try_merge(p, n->parent);
	}
out:
	mutex_unlock(&p->lock);
}
EXPORT_SYMBOL_GPL(lhp_free);

/* --------------------------------------------------------------------------
 * Pool init: pull 1G compound blocks out of the reserved CMA area
 * -------------------------------------------------------------------------- */

static int __init lhp_pool_init(void)
{
	struct lhp_pool *p = &lhp_pool;
	unsigned long nr_1g, i;
	int level;

	mutex_init(&p->lock);
	for (level = 0; level < LHP_NR_LEVELS; level++)
		INIT_LIST_HEAD(&p->free_lists[level]);
	INIT_LIST_HEAD(&p->array_cache);

	if (!lhp_cma)
		return 0;

	nr_1g = cma_get_size(lhp_cma) / SZ_1G;
	if (!nr_1g)
		return 0;

	p->roots = kcalloc(nr_1g, sizeof(*p->roots), GFP_KERNEL);
	if (!p->roots)
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
		p->roots[i] = root;
		lhp_freelist_add(p, root);
	}

	p->nr_1g = i;
	if (!p->nr_1g) {
		kfree(p->roots);
		p->roots = NULL;
		return 0;
	}

	WRITE_ONCE(p->ready, true);
	pr_info("pool ready: %lu x 1G blocks\n", p->nr_1g);
	return 0;
}
/* After buddy/CMA are fully up. */
late_initcall(lhp_pool_init);

/* --------------------------------------------------------------------------
 * debugfs: observe and drive the pool
 *
 *   /sys/kernel/debug/lhp/stats   - read counters
 *   /sys/kernel/debug/lhp/alloc   - write "0|1|2" (4K|2M|1G) to test-alloc
 *   /sys/kernel/debug/lhp/free    - write a pfn to free the chunk at that pfn
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
	struct lhp_pool *p = &lhp_pool;
	int l;

	mutex_lock(&p->lock);
	seq_printf(m, "ready: %d\n1G roots: %lu\narray cache: %lu\n",
		   p->ready, p->nr_1g, p->nr_cached);
	for (l = LHP_NR_LEVELS - 1; l >= 0; l--)
		seq_printf(m, "%s: free=%lu alloc=%lu\n",
			   lhp_level_name(l), p->nr_free[l], p->nr_alloc[l]);
	mutex_unlock(&p->lock);
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

	pg = lhp_alloc(level);
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
 * Microbenchmark.  Writing "<n>" to lhp/bench allocates @n 4K chunks (which
 * forces the full 1G->2M->4K split chain and exercises the array cache) and
 * then frees them all (forcing merges back up), timing each phase.  The result
 * is stashed for reading back via the same file.
 *
 * This measures the pure metadata split/merge cost of plan B: no page data is
 * ever copied, so it isolates the effect of the bulk + array-cache changes.
 */
struct lhp_bench_result {
	unsigned long	n;
	u64		alloc_ns;
	u64		free_ns;
	bool		valid;
};
static struct lhp_bench_result lhp_bench_last;

static ssize_t lhp_bench_write(struct file *file, const char __user *ubuf,
			       size_t count, loff_t *ppos)
{
	struct page **pages;
	unsigned long n, i, done;
	ktime_t t0, t1, t2;
	int ret;

	ret = kstrtoul_from_user(ubuf, count, 0, &n);
	if (ret)
		return ret;
	if (!n || n > (1UL << 20))	/* cap at ~1M chunks */
		return -EINVAL;

	pages = kvcalloc(n, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	/* Phase 1: allocate n x 4K, driving splits down to the leaf level. */
	t0 = ktime_get();
	for (i = 0; i < n; i++) {
		pages[i] = lhp_alloc(LHP_LEVEL_4K);
		if (!pages[i])
			break;
	}
	t1 = ktime_get();
	done = i;

	/* Phase 2: free them all, driving merges back up towards 1G. */
	for (i = 0; i < done; i++)
		lhp_free(pages[i], LHP_LEVEL_4K);
	t2 = ktime_get();

	lhp_bench_last.n = done;
	lhp_bench_last.alloc_ns = ktime_to_ns(ktime_sub(t1, t0));
	lhp_bench_last.free_ns = ktime_to_ns(ktime_sub(t2, t1));
	lhp_bench_last.valid = true;

	pr_info("bench: %lu x 4K  alloc=%llu ns (%llu ns/op)  free=%llu ns (%llu ns/op)\n",
		done, lhp_bench_last.alloc_ns,
		done ? lhp_bench_last.alloc_ns / done : 0,
		lhp_bench_last.free_ns,
		done ? lhp_bench_last.free_ns / done : 0);

	kvfree(pages);
	if (done != n)
		return -ENOSPC;		/* pool exhausted before finishing */
	return count;
}

static int lhp_bench_show(struct seq_file *m, void *v)
{
	struct lhp_bench_result r = lhp_bench_last;

	if (!r.valid) {
		seq_puts(m, "no run yet; write an iteration count to run\n");
		return 0;
	}
	seq_printf(m, "n:            %lu x 4K\n", r.n);
	seq_printf(m, "alloc total:  %llu ns\n", r.alloc_ns);
	seq_printf(m, "alloc /op:    %llu ns\n", r.n ? r.alloc_ns / r.n : 0);
	seq_printf(m, "free total:   %llu ns\n", r.free_ns);
	seq_printf(m, "free /op:     %llu ns\n", r.n ? r.free_ns / r.n : 0);
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

static int __init lhp_debugfs_init(void)
{
	struct dentry *dir;

	dir = debugfs_create_dir("lhp", NULL);
	debugfs_create_file("stats", 0400, dir, NULL, &lhp_stats_fops);
	debugfs_create_file("alloc", 0200, dir, NULL, &lhp_alloc_fops);
	debugfs_create_file("bench", 0600, dir, NULL, &lhp_bench_fops);
	return 0;
}
late_initcall(lhp_debugfs_init);
#endif /* CONFIG_DEBUG_FS */

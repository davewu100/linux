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
	struct lhp_node		**children;	/* [LHP_FANOUT] when SPLIT */
	unsigned int		free_children;	/* # children in LHP_FREE */
	struct list_head	list;		/* member of a per-level freelist */
};

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

static struct lhp_node *lhp_node_alloc(struct page *base, enum lhp_level level,
				       struct lhp_node *parent)
{
	struct lhp_node *n;

	n = kzalloc(sizeof(*n), GFP_KERNEL);
	if (!n)
		return NULL;
	n->base = base;
	n->level = level;
	n->state = LHP_FREE;
	n->parent = parent;
	INIT_LIST_HEAD(&n->list);
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

/* --------------------------------------------------------------------------
 * Split / merge state machine
 * -------------------------------------------------------------------------- */

/*
 * Split a free node into LHP_FANOUT free children of the next level down.
 * The node moves FREE -> SPLIT.  Returns 0 or -ENOMEM.
 */
static int lhp_split_node(struct lhp_pool *p, struct lhp_node *node)
{
	enum lhp_level child_level = node->level - 1;
	unsigned long child_pages = lhp_level_nr_pages(child_level);
	struct lhp_node **children;
	unsigned int i;

	if (WARN_ON_ONCE(node->level == LHP_LEVEL_4K))
		return -EINVAL;
	if (WARN_ON_ONCE(node->state != LHP_FREE))
		return -EINVAL;

	children = kcalloc(LHP_FANOUT, sizeof(*children), GFP_KERNEL);
	if (!children)
		return -ENOMEM;

	for (i = 0; i < LHP_FANOUT; i++) {
		struct page *cbase = node->base + i * child_pages;

		children[i] = lhp_node_alloc(cbase, child_level, node);
		if (!children[i]) {
			while (i--)
				kfree(children[i]);
			kfree(children);
			return -ENOMEM;
		}
	}

	/* Commit: publish children and move parent to SPLIT. */
	lhp_freelist_del(p, node);
	node->children = children;
	node->free_children = LHP_FANOUT;
	node->state = LHP_SPLIT;

	for (i = 0; i < LHP_FANOUT; i++)
		lhp_freelist_add(p, children[i]);

	return 0;
}

/*
 * If every child of a SPLIT node is free, collapse it back: free the children
 * and return the node to its own freelist as FREE.  Recurses upward so a single
 * 4K free can bubble all the way to a reconstituted 1G.
 */
static void lhp_try_merge(struct lhp_pool *p, struct lhp_node *node)
{
	unsigned int i;

	while (node && node->state == LHP_SPLIT &&
	       node->free_children == LHP_FANOUT) {
		struct lhp_node *parent = node->parent;

		for (i = 0; i < LHP_FANOUT; i++) {
			lhp_freelist_del(p, node->children[i]);
			kfree(node->children[i]);
		}
		kfree(node->children);
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
 * Returns a node in state LHP_FREE (still on its freelist) or NULL.
 */
static struct lhp_node *lhp_get_free(struct lhp_pool *p, enum lhp_level level)
{
	if (!list_empty(&p->free_lists[level]))
		return list_first_entry(&p->free_lists[level],
					struct lhp_node, list);

	if (level == LHP_LEVEL_1G)
		return NULL;	/* nothing bigger to split */

	/* Recursively make a parent, then split it. */
	{
		struct lhp_node *parent = lhp_get_free(p, level + 1);

		if (!parent)
			return NULL;
		if (lhp_split_node(p, parent))
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
	struct lhp_node *n;
	struct page *page = NULL;

	if (!lhp_pool_available() || level >= LHP_NR_LEVELS)
		return NULL;

	mutex_lock(&p->lock);
	n = lhp_get_free(p, level);
	if (n) {
		struct lhp_node *parent = n->parent;

		lhp_freelist_del(p, n);
		n->state = LHP_ALLOCATED;
		if (parent)
			parent->free_children--;
		p->nr_alloc[level]++;
		page = n->base;
	}
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
				n = n->children[idx];
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
	seq_printf(m, "ready: %d\n1G roots: %lu\n", p->ready, p->nr_1g);
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

static int __init lhp_debugfs_init(void)
{
	struct dentry *dir;

	dir = debugfs_create_dir("lhp", NULL);
	debugfs_create_file("stats", 0400, dir, NULL, &lhp_stats_fops);
	debugfs_create_file("alloc", 0200, dir, NULL, &lhp_alloc_fops);
	return 0;
}
late_initcall(lhp_debugfs_init);
#endif /* CONFIG_DEBUG_FS */

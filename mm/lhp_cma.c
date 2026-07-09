// SPDX-License-Identifier: GPL-2.0
/*
 * LHP-CMA - Layered HugePage view over CMA (plan C).
 *
 * See include/linux/lhp_cma.h for the design.  This file reserves a CMA area at
 * boot and layers a 1G/2M/4K hierarchy on top of it.  Actual memory is obtained
 * with cma_alloc() and returned with cma_release(); the layering state here
 * only records which sub-ranges of each 1G-aligned region are handed out, so we
 * can answer "is this whole 1G region idle again?" (i.e. merged) in O(1).
 *
 * Like plan B this is a compilable skeleton with a debugfs driver; it does not
 * wire into a real allocation path yet.
 */
#define pr_fmt(fmt) "lhp-cma: " fmt

#include <linux/lhp_cma.h>
#include <linux/cma.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/bitmap.h>
#include <linux/init.h>
#include <linux/sizes.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>

/*
 * One 1G-aligned region of the CMA area.  We track occupancy at two
 * granularities:
 *
 *   twom_used  - bit i set => the i-th 2M slice is either handed out whole as a
 *                2M chunk, or has been sub-divided for 4K chunks.
 *   twom_split - bit i set => the i-th 2M slice is sub-divided into 4K chunks;
 *                fourk_used[i] then tracks the 512 4K pages within it.
 *
 * A region is "whole" (fully merged, equivalent to an idle 1G) exactly when
 * twom_used is all-zero.  A region allocated as a single 1G chunk sets
 * whole_1g and leaves the bitmaps empty (it owns everything implicitly).
 */
struct lhp_cma_region {
	struct page		*base;		/* 1G-aligned head page */
	bool			whole_1g;	/* handed out as one 1G chunk */
	DECLARE_BITMAP(twom_used, LHP_CMA_2M_PER_1G);
	DECLARE_BITMAP(twom_split, LHP_CMA_2M_PER_1G);
	unsigned long		*fourk_used[LHP_CMA_2M_PER_1G];
	unsigned int		nr_twom_used;	/* popcount(twom_used) */
	struct list_head	list;		/* member of regions or free_1g */
};

struct lhp_cma_pool {
	struct cma		*cma;
	struct list_head	regions;	/* all populated regions */
	struct list_head	free_1g;	/* regions that are wholly idle */
	unsigned long		nr_regions;
	unsigned long		nr_alloc[LHP_CMA_NR_LEVELS];
	struct mutex		lock;
	bool			ready;
};

static struct lhp_cma_pool lhp_cma_pool;

static phys_addr_t lhp_cma_size __initdata;
static struct cma *lhp_cma_area __initdata;

/* --------------------------------------------------------------------------
 * Boot reservation
 * -------------------------------------------------------------------------- */

static int __init lhp_cma_cmdline(char *p)
{
	if (!p)
		return -EINVAL;
	lhp_cma_size = memparse(p, &p);
	return 0;
}
early_param("lhp_cma", lhp_cma_cmdline);

void __init lhp_cma_reserve(void)
{
	phys_addr_t size = lhp_cma_size;
	int ret;

	if (!size)
		return;

	size = ALIGN(size, SZ_1G);

	ret = cma_declare_contiguous_nid(0, size, 0, SZ_1G, 0, false,
					 "lhp-cma", &lhp_cma_area, NUMA_NO_NODE);
	if (ret) {
		pr_warn("failed to reserve %llu MiB CMA (%d)\n",
			(u64)size / SZ_1M, ret);
		lhp_cma_area = NULL;
		return;
	}

	pr_info("reserved %llu MiB CMA for layered view\n",
		(u64)size / SZ_1M);
}

/* --------------------------------------------------------------------------
 * Region helpers
 * -------------------------------------------------------------------------- */

static struct lhp_cma_region *lhp_cma_region_of(struct lhp_cma_pool *p,
						struct page *page)
{
	unsigned long region_pages = lhp_cma_level_nr_pages(LHP_CMA_1G);
	struct lhp_cma_region *r;

	list_for_each_entry(r, &p->regions, list) {
		if (page >= r->base && page < r->base + region_pages)
			return r;
	}
	return NULL;
}

/* Grab a fresh 1G region from CMA and register it. */
static struct lhp_cma_region *lhp_cma_new_region(struct lhp_cma_pool *p,
						 gfp_t gfp)
{
	unsigned long region_pages = lhp_cma_level_nr_pages(LHP_CMA_1G);
	struct lhp_cma_region *r;
	struct page *base;

	base = cma_alloc(p->cma, region_pages, LHP_CMA_ORDER_1G, true);
	if (!base)
		return NULL;

	r = kzalloc(sizeof(*r), gfp);
	if (!r) {
		cma_release(p->cma, base, region_pages);
		return NULL;
	}
	r->base = base;
	INIT_LIST_HEAD(&r->list);
	list_add(&r->list, &p->regions);
	p->nr_regions++;
	return r;
}

/* Return a region to CMA once it is wholly idle. */
static void lhp_cma_put_region(struct lhp_cma_pool *p,
			       struct lhp_cma_region *r)
{
	unsigned long region_pages = lhp_cma_level_nr_pages(LHP_CMA_1G);

	list_del(&r->list);
	cma_release(p->cma, r->base, region_pages);
	kfree(r);
	p->nr_regions--;
}

/*
 * Release every wholly-idle (merged) 1G region back to CMA so the rest of the
 * system can reuse the contiguous memory.  Returns the number trimmed.  This is
 * the payoff of the merge-back bookkeeping: a region only lands on free_1g once
 * its bitmaps are entirely clear.
 */
static unsigned long __maybe_unused lhp_cma_trim(struct lhp_cma_pool *p)
{
	struct lhp_cma_region *r, *tmp;
	unsigned long trimmed = 0;

	list_for_each_entry_safe(r, tmp, &p->free_1g, list) {
		lhp_cma_put_region(p, r);
		trimmed++;
	}
	return trimmed;
}

/* --------------------------------------------------------------------------
 * Allocation
 * -------------------------------------------------------------------------- */

static struct page *lhp_cma_alloc_1g(struct lhp_cma_pool *p, gfp_t gfp)
{
	struct lhp_cma_region *r;

	/* Prefer recycling a wholly-idle region. */
	if (!list_empty(&p->free_1g)) {
		r = list_first_entry(&p->free_1g, struct lhp_cma_region, list);
		list_del_init(&r->list);
		/* Re-thread onto the populated list. */
		list_add(&r->list, &p->regions);
	} else {
		r = lhp_cma_new_region(p, gfp);
		if (!r)
			return NULL;
	}
	r->whole_1g = true;
	p->nr_alloc[LHP_CMA_1G]++;
	return r->base;
}

/* Find (or create) a region with a free 2M slice and return its index. */
static struct lhp_cma_region *lhp_cma_region_with_free_2m(struct lhp_cma_pool *p,
							  int *idx_out, gfp_t gfp)
{
	struct lhp_cma_region *r;
	int idx;

	list_for_each_entry(r, &p->regions, list) {
		if (r->whole_1g)
			continue;
		idx = find_first_zero_bit(r->twom_used, LHP_CMA_2M_PER_1G);
		if (idx < LHP_CMA_2M_PER_1G) {
			*idx_out = idx;
			return r;
		}
	}

	r = lhp_cma_new_region(p, gfp);
	if (!r)
		return NULL;
	*idx_out = 0;
	return r;
}

static struct page *lhp_cma_alloc_2m(struct lhp_cma_pool *p, gfp_t gfp)
{
	struct lhp_cma_region *r;
	int idx;

	r = lhp_cma_region_with_free_2m(p, &idx, gfp);
	if (!r)
		return NULL;

	set_bit(idx, r->twom_used);
	r->nr_twom_used++;
	p->nr_alloc[LHP_CMA_2M]++;
	return r->base + idx * lhp_cma_level_nr_pages(LHP_CMA_2M);
}

static struct page *lhp_cma_alloc_4k(struct lhp_cma_pool *p, gfp_t gfp)
{
	struct lhp_cma_region *r;
	int t, b;

	/* Look for a 2M slice already split for 4K use with a free page. */
	list_for_each_entry(r, &p->regions, list) {
		if (r->whole_1g)
			continue;
		for_each_set_bit(t, r->twom_split, LHP_CMA_2M_PER_1G) {
			b = find_first_zero_bit(r->fourk_used[t],
						LHP_CMA_4K_PER_2M);
			if (b < LHP_CMA_4K_PER_2M)
				goto found;
		}
	}

	/* None: carve a new 2M slice for 4K use. */
	{
		int idx;

		r = lhp_cma_region_with_free_2m(p, &idx, gfp);
		if (!r)
			return NULL;
		r->fourk_used[idx] = bitmap_zalloc(LHP_CMA_4K_PER_2M, gfp);
		if (!r->fourk_used[idx])
			return NULL;
		set_bit(idx, r->twom_used);
		set_bit(idx, r->twom_split);
		r->nr_twom_used++;
		t = idx;
		b = 0;
	}

found:
	set_bit(b, r->fourk_used[t]);
	p->nr_alloc[LHP_CMA_4K]++;
	return r->base + t * lhp_cma_level_nr_pages(LHP_CMA_2M) + b;
}

struct page *lhp_cma_alloc(enum lhp_cma_level level, gfp_t gfp)
{
	struct lhp_cma_pool *p = &lhp_cma_pool;
	struct page *page = NULL;

	if (!lhp_cma_available() || level >= LHP_CMA_NR_LEVELS)
		return NULL;

	mutex_lock(&p->lock);
	switch (level) {
	case LHP_CMA_1G:
		page = lhp_cma_alloc_1g(p, gfp);
		break;
	case LHP_CMA_2M:
		page = lhp_cma_alloc_2m(p, gfp);
		break;
	default:
		page = lhp_cma_alloc_4k(p, gfp);
		break;
	}
	mutex_unlock(&p->lock);
	return page;
}
EXPORT_SYMBOL_GPL(lhp_cma_alloc);

/* --------------------------------------------------------------------------
 * Free + merge-back
 * -------------------------------------------------------------------------- */

/*
 * After a free, if the region became wholly idle, move it to the free_1g list
 * (deterministic "merged back to 1G": its bitmaps are all clear so the entire
 * 1G is contiguous and reusable as a single chunk).
 */
static void lhp_cma_maybe_merge(struct lhp_cma_pool *p,
				struct lhp_cma_region *r)
{
	if (r->whole_1g || r->nr_twom_used)
		return;

	/* Wholly idle: recycle as a mergeable 1G. */
	list_del(&r->list);
	list_add(&r->list, &p->free_1g);
}

void lhp_cma_free(struct page *page, enum lhp_cma_level level)
{
	struct lhp_cma_pool *p = &lhp_cma_pool;
	struct lhp_cma_region *r;
	unsigned long off;
	int t, b;

	if (!lhp_cma_available() || !page || level >= LHP_CMA_NR_LEVELS)
		return;

	mutex_lock(&p->lock);
	r = lhp_cma_region_of(p, page);
	if (!r) {
		WARN_ONCE(1, "lhp_cma_free: unknown page %px\n", page);
		goto out;
	}

	off = page - r->base;

	switch (level) {
	case LHP_CMA_1G:
		if (WARN_ON_ONCE(!r->whole_1g))
			break;
		r->whole_1g = false;
		p->nr_alloc[LHP_CMA_1G]--;
		lhp_cma_maybe_merge(p, r);
		break;
	case LHP_CMA_2M:
		t = off / lhp_cma_level_nr_pages(LHP_CMA_2M);
		if (WARN_ON_ONCE(!test_bit(t, r->twom_used) ||
				 test_bit(t, r->twom_split)))
			break;
		clear_bit(t, r->twom_used);
		r->nr_twom_used--;
		p->nr_alloc[LHP_CMA_2M]--;
		lhp_cma_maybe_merge(p, r);
		break;
	default:
		t = off / lhp_cma_level_nr_pages(LHP_CMA_2M);
		b = off % lhp_cma_level_nr_pages(LHP_CMA_2M);
		if (WARN_ON_ONCE(!test_bit(t, r->twom_split) ||
				 !r->fourk_used[t]))
			break;
		clear_bit(b, r->fourk_used[t]);
		p->nr_alloc[LHP_CMA_4K]--;
		/* If the whole 2M slice emptied, collapse it back to a 2M. */
		if (bitmap_empty(r->fourk_used[t], LHP_CMA_4K_PER_2M)) {
			bitmap_free(r->fourk_used[t]);
			r->fourk_used[t] = NULL;
			clear_bit(t, r->twom_split);
			clear_bit(t, r->twom_used);
			r->nr_twom_used--;
			lhp_cma_maybe_merge(p, r);
		}
		break;
	}
out:
	mutex_unlock(&p->lock);
}
EXPORT_SYMBOL_GPL(lhp_cma_free);

/* --------------------------------------------------------------------------
 * Init
 * -------------------------------------------------------------------------- */

int lhp_cma_available(void)
{
	/*
	 * Acquire load pairs with the smp_store_release() in
	 * lhp_cma_pool_init(): a caller that observes ready == true also sees
	 * the initialised p->cma and list heads.
	 */
	return smp_load_acquire(&lhp_cma_pool.ready);
}
EXPORT_SYMBOL_GPL(lhp_cma_available);

static int __init lhp_cma_pool_init(void)
{
	struct lhp_cma_pool *p = &lhp_cma_pool;

	mutex_init(&p->lock);
	INIT_LIST_HEAD(&p->regions);
	INIT_LIST_HEAD(&p->free_1g);

	if (!lhp_cma_area)
		return 0;

	p->cma = lhp_cma_area;
	/*
	 * Release store: publishes p->cma and the initialised list heads to any
	 * consumer that observes ready == true via lhp_cma_available().
	 */
	smp_store_release(&p->ready, true);
	pr_info("layered CMA view ready (%llu MiB)\n",
		(u64)cma_get_size(lhp_cma_area) / SZ_1M);
	return 0;
}
late_initcall(lhp_cma_pool_init);

/* --------------------------------------------------------------------------
 * debugfs
 * -------------------------------------------------------------------------- */

#ifdef CONFIG_DEBUG_FS
static const char *lhp_cma_level_name(enum lhp_cma_level l)
{
	switch (l) {
	case LHP_CMA_1G: return "1G";
	case LHP_CMA_2M: return "2M";
	default:	 return "4K";
	}
}

static int lhp_cma_stats_show(struct seq_file *m, void *v)
{
	struct lhp_cma_pool *p = &lhp_cma_pool;
	struct lhp_cma_region *r;
	unsigned long whole = 0, mergeable = 0;
	int l;

	mutex_lock(&p->lock);
	list_for_each_entry(r, &p->regions, list)
		if (r->whole_1g)
			whole++;
	list_for_each_entry(r, &p->free_1g, list)
		mergeable++;

	seq_printf(m, "ready: %d\nregions: %lu\nwhole-1G: %lu\nmergeable-1G(idle): %lu\n",
		   p->ready, p->nr_regions, whole, mergeable);
	for (l = LHP_CMA_NR_LEVELS - 1; l >= 0; l--)
		seq_printf(m, "%s: alloc=%lu\n",
			   lhp_cma_level_name(l), p->nr_alloc[l]);
	mutex_unlock(&p->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(lhp_cma_stats);

static ssize_t lhp_cma_alloc_write(struct file *file, const char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	unsigned int level;
	struct page *pg;
	int ret;

	ret = kstrtouint_from_user(ubuf, count, 0, &level);
	if (ret)
		return ret;
	if (level >= LHP_CMA_NR_LEVELS)
		return -EINVAL;

	pg = lhp_cma_alloc(level, GFP_KERNEL);
	if (!pg)
		return -ENOMEM;

	pr_info("debugfs alloc %s -> pfn %lu\n",
		lhp_cma_level_name(level), page_to_pfn(pg));
	return count;
}

static const struct file_operations lhp_cma_alloc_fops = {
	.write = lhp_cma_alloc_write,
	.open = simple_open,
	.llseek = default_llseek,
};

static ssize_t lhp_cma_trim_write(struct file *file, const char __user *ubuf,
				  size_t count, loff_t *ppos)
{
	struct lhp_cma_pool *p = &lhp_cma_pool;
	unsigned long trimmed;

	mutex_lock(&p->lock);
	trimmed = lhp_cma_trim(p);
	mutex_unlock(&p->lock);

	pr_info("debugfs trim: released %lu idle 1G region(s) to CMA\n",
		trimmed);
	return count;
}

static const struct file_operations lhp_cma_trim_fops = {
	.write = lhp_cma_trim_write,
	.open = simple_open,
	.llseek = default_llseek,
};

static int __init lhp_cma_debugfs_init(void)
{
	struct dentry *dir;

	dir = debugfs_create_dir("lhp_cma", NULL);
	debugfs_create_file("stats", 0400, dir, NULL, &lhp_cma_stats_fops);
	debugfs_create_file("alloc", 0200, dir, NULL, &lhp_cma_alloc_fops);
	debugfs_create_file("trim", 0200, dir, NULL, &lhp_cma_trim_fops);
	return 0;
}
late_initcall(lhp_cma_debugfs_init);
#endif /* CONFIG_DEBUG_FS */

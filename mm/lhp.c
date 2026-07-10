// SPDX-License-Identifier: GPL-2.0
/*
 * LHP - Large HugePage pool (static 2M memzones).
 *
 * See include/linux/lhp.h for the high-level design.  In short: reserve a chunk
 * of physically contiguous memory at boot, carve it into 1G regions, and treat
 * every region purely as a container of 512 2M chunks.  2M is the one and only
 * allocation granularity -- there is no split/merge and no 4K path, so a region
 * never changes shape and the backing pages never return to the buddy
 * allocator.  Physical contiguity within a region is guaranteed by the 1G CMA
 * block itself.
 *
 * Two layers:
 *   phys pool (2M chunks) -> memzone (named contiguous zone).
 */
#define pr_fmt(fmt) "lhp: " fmt

#include <linux/lhp.h>
#include <linux/cma.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/bitmap.h>
#include <linux/init.h>
#include <linux/sizes.h>
#include <linux/memblock.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/string.h>
#include <linux/err.h>
#include <linux/sysfs.h>

/*
 * A single 1G region: a fixed container of LHP_CHUNKS_PER_REGION (512) 2M
 * chunks.  This is the unit of locking.  A region owns its own spinlock and a
 * bitmap of allocated chunks; because 2M is the only granularity and chunks
 * never split or merge, allocation and free are pure O(1) bitmap operations.
 * Different CPUs operating on different regions never contend.
 */
struct lhp_region {
	spinlock_t		lock;
	struct page		*base;		/* head page of the 1G block */
	unsigned long		nr_alloc;	/* 2M chunks currently handed out */
	bool			whole_1g;	/* region handed out as one 1G block */
	bool			reserved_1g;	/* dedicated to 1G allocation only */
	DECLARE_BITMAP(alloc_map, LHP_CHUNKS_PER_REGION);
};

/*
 * The pool: an immutable-after-init array of regions plus a round-robin cursor
 * used to spread allocations across regions and reduce per-region contention.
 */
struct lhp_phys_pool {
	struct cma		*cma;
	unsigned long		nr_regions;
	unsigned long		nr_reserved_1g;		/* regions dedicated to 1G */
	struct lhp_region	*regions;		/* [nr_regions] */
	atomic_t		cursor;			/* round-robin start hint */
	bool			ready;
};

static struct lhp_phys_pool lhp_phys_pool;

/*
 * Next round-robin start region.  atomic_t increment wraps with well-defined
 * semantics under the kernel's -fno-strict-overflow; reading the result as
 * unsigned int avoids sign-extending a negative int into the unsigned long
 * index once the counter passes INT_MAX.
 */
static inline unsigned long lhp_next_start(struct lhp_phys_pool *p)
{
	return (unsigned int)atomic_fetch_inc(&p->cursor);
}

/* Boot-time reservation state. */
static phys_addr_t lhp_reserve_size __initdata;
static unsigned long lhp_reserve_1g __initdata;	/* regions dedicated to 1G */
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
 * "lhp_1g=<n>": dedicate N 1G regions exclusively to lhp_alloc_1g().  These
 * regions are never carved into 2M chunks, guaranteeing that many 1G
 * allocations can always succeed regardless of 2M load.  Clamped to the number
 * of regions actually obtained.
 */
static int __init lhp_1g_cmdline(char *p)
{
	if (!p)
		return -EINVAL;
	if (kstrtoul(p, 0, &lhp_reserve_1g))
		return -EINVAL;
	return 0;
}
early_param("lhp_1g", lhp_1g_cmdline);

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

	pr_info("reserved %llu MiB contiguous for large hugepage pool\n",
		(u64)size / SZ_1M);
}

/* --------------------------------------------------------------------------
 * 2M chunk allocator
 * -------------------------------------------------------------------------- */

/*
 * Try to allocate one 2M chunk from region @rg.  Returns the head page or NULL
 * if the region is full.  Caller holds the region lock.
 */
static struct page *lhp_region_alloc(struct lhp_region *rg)
{
	unsigned int idx;

	/*
	 * A region handed out whole as 1G, or dedicated to 1G via lhp_1g=, is
	 * off-limits for 2M carving.
	 */
	if (rg->whole_1g || rg->reserved_1g)
		return NULL;

	idx = find_first_zero_bit(rg->alloc_map, LHP_CHUNKS_PER_REGION);
	if (idx >= LHP_CHUNKS_PER_REGION)
		return NULL;

	__set_bit(idx, rg->alloc_map);
	rg->nr_alloc++;
	return rg->base + idx * LHP_CHUNK_PAGES;
}

/**
 * lhp_pool_available - test whether the LHP pool is initialised and usable
 *
 * The pool is brought up by a late initcall once the 1G blocks have been
 * pulled out of CMA.  All allocation entry points also fail gracefully before
 * that point, so callers only need this when they want to probe explicitly.
 *
 * Return: non-zero once the pool is ready, 0 otherwise.
 */
int lhp_pool_available(void)
{
	/*
	 * Acquire load pairs with the smp_store_release() in lhp_pool_init():
	 * a caller that observes ready == true is guaranteed to also see the
	 * fully initialised regions array.
	 */
	return smp_load_acquire(&lhp_phys_pool.ready);
}
EXPORT_SYMBOL_GPL(lhp_pool_available);

/**
 * lhp_alloc_2m - allocate one 2M chunk from the pool
 * @gfp: flags for incidental bookkeeping only; the chunk itself is never
 *       allocated from the buddy allocator
 *
 * Hands out a single 2M chunk, spreading allocations across regions with a
 * round-robin cursor.  Regions handed out whole as 1G, or dedicated to 1G via
 * "lhp_1g=", are skipped.
 *
 * Return: the head &struct page of the chunk, or NULL if the pool is not ready
 * or has no free chunk.
 */
struct page *lhp_alloc_2m(gfp_t gfp)
{
	struct lhp_phys_pool *p = &lhp_phys_pool;
	struct page *page = NULL;
	unsigned long i, start;

	if (!lhp_pool_available())
		return NULL;

	/*
	 * Round-robin across regions from a shared cursor to spread contention,
	 * then linear-probe.  Only one region lock is held at a time, so CPUs
	 * hitting different regions never contend.
	 */
	start = lhp_next_start(p);
	for (i = 0; i < p->nr_regions; i++) {
		struct lhp_region *rg = &p->regions[(start + i) % p->nr_regions];

		spin_lock(&rg->lock);
		page = lhp_region_alloc(rg);
		spin_unlock(&rg->lock);
		if (page)
			break;
	}

	return page;
}
EXPORT_SYMBOL_GPL(lhp_alloc_2m);

/*
 * Find the region owning @page.  The regions[] array is immutable after init,
 * so this needs no lock.
 */
static struct lhp_region *lhp_find_region(struct lhp_phys_pool *p,
					  struct page *page)
{
	unsigned long i;

	for (i = 0; i < p->nr_regions; i++) {
		struct lhp_region *rg = &p->regions[i];

		if (page >= rg->base && page < rg->base + LHP_REGION_PAGES)
			return rg;
	}
	return NULL;
}

/**
 * lhp_free_2m - return a 2M chunk to the pool
 * @page: head page previously returned by lhp_alloc_2m()
 *
 * NULL is ignored.  A page that is not a 2M-aligned chunk of a known region,
 * or that is already free, is rejected with a warning.
 */
void lhp_free_2m(struct page *page)
{
	struct lhp_phys_pool *p = &lhp_phys_pool;
	struct lhp_region *rg;
	unsigned long off;
	unsigned int idx;

	if (!lhp_pool_available() || !page)
		return;

	rg = lhp_find_region(p, page);
	if (WARN_ONCE(!rg, "lhp_free_2m: page %px not in any region\n", page))
		return;

	off = page - rg->base;
	if (WARN_ONCE(off % LHP_CHUNK_PAGES,
		      "lhp_free_2m: page %px not 2M-aligned\n", page))
		return;
	idx = off / LHP_CHUNK_PAGES;

	spin_lock(&rg->lock);
	if (!test_bit(idx, rg->alloc_map)) {
		WARN_ONCE(1, "lhp_free_2m: double free of chunk %u\n", idx);
		goto out;
	}
	__clear_bit(idx, rg->alloc_map);
	rg->nr_alloc--;
out:
	spin_unlock(&rg->lock);
}
EXPORT_SYMBOL_GPL(lhp_free_2m);

/**
 * lhp_alloc_1g - allocate a whole 1G region as one contiguous block
 * @gfp: flags for incidental bookkeeping only
 *
 * A region is eligible only if it is completely idle (no 2M chunks carved out
 * and not already handed out as 1G).  Once taken it is off-limits to the 2M
 * carver until returned with lhp_free_1g().
 *
 * Regions dedicated via "lhp_1g=<n>" are never carved into 2M chunks, so they
 * stay permanently idle and available here; configuring lhp_1g therefore
 * guarantees 1G allocations can succeed regardless of 2M load.  Non-dedicated
 * regions are also used opportunistically when fully idle.
 *
 * Return: the region's head &struct page, or NULL if no region is fully free.
 */
struct page *lhp_alloc_1g(gfp_t gfp)
{
	struct lhp_phys_pool *p = &lhp_phys_pool;
	struct page *page = NULL;
	unsigned long i, start;

	if (!lhp_pool_available())
		return NULL;

	start = lhp_next_start(p);
	for (i = 0; i < p->nr_regions; i++) {
		struct lhp_region *rg = &p->regions[(start + i) % p->nr_regions];

		spin_lock(&rg->lock);
		if (!rg->whole_1g && rg->nr_alloc == 0) {
			rg->whole_1g = true;
			page = rg->base;
		}
		spin_unlock(&rg->lock);
		if (page)
			break;
	}

	return page;
}
EXPORT_SYMBOL_GPL(lhp_alloc_1g);

/**
 * lhp_free_1g - return a whole 1G region to the pool
 * @page: region head page previously returned by lhp_alloc_1g()
 *
 * NULL is ignored.  A page that is not a region base, or a region that was not
 * handed out as 1G, is rejected with a warning.
 */
void lhp_free_1g(struct page *page)
{
	struct lhp_phys_pool *p = &lhp_phys_pool;
	struct lhp_region *rg;

	if (!lhp_pool_available() || !page)
		return;

	rg = lhp_find_region(p, page);
	if (WARN_ONCE(!rg, "lhp_free_1g: page %px not in any region\n", page))
		return;
	if (WARN_ONCE(page != rg->base,
		      "lhp_free_1g: page %px is not a 1G region base\n", page))
		return;

	spin_lock(&rg->lock);
	if (WARN_ONCE(!rg->whole_1g, "lhp_free_1g: region not 1G-allocated\n"))
		goto out;
	rg->whole_1g = false;
out:
	spin_unlock(&rg->lock);
}
EXPORT_SYMBOL_GPL(lhp_free_1g);

/* --------------------------------------------------------------------------
 * Pool init: pull 1G compound blocks out of the reserved CMA area
 * -------------------------------------------------------------------------- */

static void lhp_region_init(struct lhp_region *rg, struct page *base)
{
	spin_lock_init(&rg->lock);
	rg->base = base;
	rg->nr_alloc = 0;
	rg->whole_1g = false;
	rg->reserved_1g = false;
	bitmap_zero(rg->alloc_map, LHP_CHUNKS_PER_REGION);
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

	{
	unsigned long got = 0;

	for (i = 0; i < nr_1g; i++) {
		struct page *base;

		/* One 1G contiguous block == order LHP_REGION_ORDER. */
		base = cma_alloc(lhp_cma, LHP_REGION_PAGES, LHP_REGION_ORDER,
				 true);
		if (!base) {
			/*
			 * A single 1G block may be temporarily unmigratable;
			 * keep trying the rest rather than giving up, and pack
			 * the successes contiguously in regions[].
			 */
			pr_warn("skipping unavailable 1G block %lu/%lu\n",
				i, nr_1g);
			continue;
		}

		lhp_region_init(&p->regions[got++], base);
	}
	p->nr_regions = got;
	}

	if (!p->nr_regions) {
		kfree(p->regions);
		p->regions = NULL;
		return 0;
	}

	/* Dedicate up to lhp_1g= regions exclusively to 1G allocation. */
	p->nr_reserved_1g = min(lhp_reserve_1g, p->nr_regions);
	for (i = 0; i < p->nr_reserved_1g; i++)
		p->regions[i].reserved_1g = true;

	/*
	 * Release store: publishes the fully initialised regions array to any
	 * consumer that observes ready == true via lhp_pool_available().
	 */
	smp_store_release(&p->ready, true);
	pr_info("pool ready: %lu x 1G regions (%lu dedicated to 1G), %lu 2M chunks each\n",
		p->nr_regions, p->nr_reserved_1g,
		(unsigned long)LHP_CHUNKS_PER_REGION);
	return 0;
}
/* After buddy/CMA are fully up. */
late_initcall(lhp_pool_init);

/* --------------------------------------------------------------------------
 * memzone: named contiguous backing range carved as a set of 2M chunks
 * -------------------------------------------------------------------------- */

struct lhp_memzone_chunk {
	struct list_head	list;
	struct page		*base;
};

struct lhp_memzone {
	struct list_head	list;
	char			name[LHP_NAME_MAX];
	size_t			len;			/* rounded up to 2M */
	int			nid;
	unsigned long		flags;
	unsigned long		bytes_used;
	struct list_head	chunks;
	/*
	 * All memzone operations run in process context, and the chunk list is
	 * only mutated at reserve/free time.  A mutex (not a spinlock) lets
	 * lhp_memzone_for_each_chunk() invoke callbacks that may sleep (e.g. a
	 * heap laying out arenas with GFP_KERNEL allocations).
	 */
	struct mutex		lock;
	refcount_t		refs;
};

static LIST_HEAD(lhp_memzone_list);
static DEFINE_MUTEX(lhp_registry_lock);

static int lhp_memzone_attach_chunk(struct lhp_memzone *mz, struct page *page,
				    gfp_t gfp)
{
	struct lhp_memzone_chunk *c;

	c = kmalloc(sizeof(*c), gfp);
	if (!c)
		return -ENOMEM;

	INIT_LIST_HEAD(&c->list);
	c->base = page;
	list_add_tail(&c->list, &mz->chunks);
	mz->bytes_used += LHP_CHUNK_BYTES;
	return 0;
}

static void lhp_memzone_detach_chunk(struct lhp_memzone *mz,
				     struct lhp_memzone_chunk *c)
{
	list_del(&c->list);
	mz->bytes_used -= LHP_CHUNK_BYTES;
	lhp_free_2m(c->base);
	kfree(c);
}

static int lhp_memzone_grow(struct lhp_memzone *mz, size_t need, gfp_t gfp)
{
	size_t got = 0;

	while (got < need) {
		struct page *page;
		int ret;

		page = lhp_alloc_2m(gfp);
		if (!page)
			return -ENOMEM;

		ret = lhp_memzone_attach_chunk(mz, page, gfp);
		if (ret) {
			lhp_free_2m(page);
			return ret;
		}
		got += LHP_CHUNK_BYTES;
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

/**
 * lhp_memzone_lookup - find a memzone by name and take a reference
 * @name: memzone name
 *
 * The caller owns the returned reference and must drop it with
 * lhp_memzone_free().
 *
 * Return: the memzone, or NULL if no zone with that name exists.
 */
struct lhp_memzone *lhp_memzone_lookup(const char *name)
{
	struct lhp_memzone *mz;

	if (!name)
		return NULL;

	mutex_lock(&lhp_registry_lock);
	mz = lhp_memzone_find_locked(name);
	if (mz)
		refcount_inc(&mz->refs);
	mutex_unlock(&lhp_registry_lock);
	return mz;
}
EXPORT_SYMBOL_GPL(lhp_memzone_lookup);

/**
 * lhp_memzone_reserve - reserve a named contiguous zone from the pool
 * @name: unique zone name (must be shorter than %LHP_NAME_MAX)
 * @len:  requested length in bytes, rounded up to a 2M multiple
 * @nid:  preferred NUMA node, or %NUMA_NO_NODE
 * @flags: reserved for future use (%LHP_MEMZONE_F_NONE)
 *
 * Builds a zone from a set of 2M chunks, analogous to rte_memzone_reserve().
 * The chunks are not guaranteed to be physically contiguous with one another.
 * The zone is created with one reference; drop it with lhp_memzone_free().
 *
 * Return: the memzone on success, or an ERR_PTR: -EINVAL for bad arguments or
 * a pool that is not ready, -EEXIST if the name is taken, -ENOMEM on
 * allocation failure.
 */
struct lhp_memzone *lhp_memzone_reserve(const char *name, size_t len,
					int nid, unsigned long flags)
{
	struct lhp_memzone *mz;
	size_t reserve_len;
	int ret;

	if (!lhp_pool_available() || !name || !*name || !len)
		return ERR_PTR(-EINVAL);
	if (strnlen(name, LHP_NAME_MAX) >= LHP_NAME_MAX)
		return ERR_PTR(-EINVAL);

	/* Everything is 2M-granular. */
	reserve_len = ALIGN(len, LHP_CHUNK_BYTES);

	/*
	 * Optimistically build the zone outside the registry lock (grow() may
	 * sleep), then re-check for a name collision under the lock before
	 * publishing.  A concurrent reserve of the same name is resolved here,
	 * closing the check-then-insert TOCTOU window.
	 */
	mz = kzalloc(sizeof(*mz), GFP_KERNEL);
	if (!mz)
		return ERR_PTR(-ENOMEM);

	strscpy(mz->name, name, LHP_NAME_MAX);
	mz->len = reserve_len;
	mz->nid = nid;
	mz->flags = flags;
	INIT_LIST_HEAD(&mz->chunks);
	mutex_init(&mz->lock);
	refcount_set(&mz->refs, 1);

	ret = lhp_memzone_grow(mz, reserve_len, GFP_KERNEL);
	if (ret)
		goto err_free;

	mutex_lock(&lhp_registry_lock);
	if (lhp_memzone_find_locked(name)) {
		mutex_unlock(&lhp_registry_lock);
		ret = -EEXIST;
		goto err_free;
	}
	list_add_tail(&mz->list, &lhp_memzone_list);
	mutex_unlock(&lhp_registry_lock);

	pr_info("memzone %s: reserved %zu bytes (%lu 2M chunks)\n",
		mz->name, reserve_len, mz->bytes_used / LHP_CHUNK_BYTES);
	return mz;

err_free:
	{
		struct lhp_memzone_chunk *c, *tmp;

		list_for_each_entry_safe(c, tmp, &mz->chunks, list)
			lhp_memzone_detach_chunk(mz, c);
	}
	mutex_destroy(&mz->lock);
	kfree(mz);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(lhp_memzone_reserve);

/**
 * lhp_memzone_free - drop a reference on a memzone
 * @zone: memzone from lhp_memzone_reserve() or lhp_memzone_lookup()
 *
 * Drops one reference.  The zone and its 2M chunks are released back to the
 * pool only when the last reference is dropped.  NULL is ignored.
 */
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

	mutex_lock(&zone->lock);
	list_for_each_entry_safe(c, tmp, &zone->chunks, list)
		lhp_memzone_detach_chunk(zone, c);
	mutex_unlock(&zone->lock);

	mutex_destroy(&zone->lock);
	kfree(zone);
}
EXPORT_SYMBOL_GPL(lhp_memzone_free);

/**
 * lhp_memzone_for_each_chunk - iterate a memzone's backing 2M chunks
 * @zone: memzone to iterate
 * @fn:   callback invoked with each chunk's kernel virtual base and length
 *        (always %LHP_CHUNK_BYTES); iteration stops early if it returns nonzero
 * @priv: opaque pointer passed through to @fn
 *
 * The callback runs in process context and may sleep.  Used by the heap layer
 * to lay out arenas over each chunk.
 *
 * Return: 0 on full iteration, -EINVAL for bad arguments, or the first nonzero
 * value returned by @fn.
 */
int lhp_memzone_for_each_chunk(struct lhp_memzone *zone, lhp_chunk_fn fn,
			       void *priv)
{
	struct lhp_memzone_chunk *c;
	int ret = 0;

	if (!zone || !fn)
		return -EINVAL;

	mutex_lock(&zone->lock);
	list_for_each_entry(c, &zone->chunks, list) {
		ret = fn(page_address(c->base), LHP_CHUNK_BYTES, priv);
		if (ret)
			break;
	}
	mutex_unlock(&zone->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(lhp_memzone_for_each_chunk);

/* --------------------------------------------------------------------------
 * debugfs: observe and drive the pool
 *
 *   /sys/kernel/debug/lhp/stats      - read aggregated per-region counters
 *   /sys/kernel/debug/lhp/alloc      - write anything to test-alloc one 2M chunk
 *   /sys/kernel/debug/lhp/layer_test - write "run" to self-test the memzone layer
 *
 * The microbenchmark lives in the separate lhp_bench module (CONFIG_LHP_BENCH).
 * -------------------------------------------------------------------------- */

#ifdef CONFIG_DEBUG_FS
static int lhp_stats_show(struct seq_file *m, void *v)
{
	struct lhp_phys_pool *p = &lhp_phys_pool;
	unsigned long nr_alloc = 0, free_regions = 0, whole_1g = 0, i;
	struct lhp_memzone *mz;

	if (!smp_load_acquire(&p->ready)) {
		seq_puts(m, "ready: 0\n");
		return 0;
	}

	/* Sum per-region counters, briefly locking each region in turn. */
	for (i = 0; i < p->nr_regions; i++) {
		struct lhp_region *rg = &p->regions[i];

		spin_lock(&rg->lock);
		if (rg->whole_1g)
			whole_1g++;
		else if (!rg->nr_alloc && !rg->reserved_1g)
			free_regions++;
		nr_alloc += rg->nr_alloc;
		spin_unlock(&rg->lock);
	}

	/* Reached only after the acquire-load above saw ready == true. */
	seq_printf(m, "ready: 1\nregions: %lu\nidle 2M-capable regions: %lu\n",
		   p->nr_regions, free_regions);
	seq_printf(m, "1G regions: in-use=%lu dedicated=%lu\n",
		   whole_1g, p->nr_reserved_1g);
	seq_printf(m, "2M chunks: total=%lu alloc=%lu free=%lu\n",
		   p->nr_regions * LHP_CHUNKS_PER_REGION, nr_alloc,
		   p->nr_regions * LHP_CHUNKS_PER_REGION - nr_alloc);

	mutex_lock(&lhp_registry_lock);
	seq_puts(m, "\nmemzones:\n");
	list_for_each_entry(mz, &lhp_memzone_list, list) {
		seq_printf(m, "  %s: len=%zu used=%lu chunks=%u refs=%d\n",
			   mz->name, mz->len, mz->bytes_used,
			   (unsigned int)list_count_nodes(&mz->chunks),
			   refcount_read(&mz->refs));
	}
	mutex_unlock(&lhp_registry_lock);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(lhp_stats);

static ssize_t lhp_alloc_write(struct file *file, const char __user *ubuf,
			       size_t count, loff_t *ppos)
{
	struct page *pg;

	/*
	 * A smoke test of the 2M alloc/free path.  Free immediately so
	 * repeated writes cannot drain the pool.
	 */
	pg = lhp_alloc_2m(GFP_KERNEL);
	if (!pg)
		return -ENOMEM;

	pr_info("debugfs alloc 2M -> pfn %lu (freed)\n", page_to_pfn(pg));
	lhp_free_2m(pg);
	return count;
}

static const struct file_operations lhp_alloc_fops = {
	.write = lhp_alloc_write,
	.open = simple_open,
	.llseek = default_llseek,
};

/*
 * Layer self-test via debugfs.  Write "run" to exercise memzone
 * reserve/lookup/free.
 */
static int lhp_layer_test_run(void)
{
	struct lhp_memzone *mz, *found;

	/*
	 * Clean up any leftover from a previous run.  lhp_memzone_lookup()
	 * takes a reference, so drop it here and drop the reserve reference too
	 * (two puts) to actually destroy a stale zone.
	 */
	mz = lhp_memzone_lookup("dbgtest");
	if (mz) {
		lhp_memzone_free(mz);	/* drop lookup ref */
		lhp_memzone_free(mz);	/* drop reserve ref -> destroy */
	}

	mz = lhp_memzone_reserve("dbgtest", 4 * LHP_CHUNK_BYTES,
				 NUMA_NO_NODE, LHP_MEMZONE_F_NONE);
	if (IS_ERR(mz)) {
		pr_err("layer_test: memzone_reserve failed %ld\n", PTR_ERR(mz));
		return PTR_ERR(mz);
	}

	found = lhp_memzone_lookup("dbgtest");	/* takes a ref */
	if (found != mz) {
		pr_err("layer_test: memzone_lookup mismatch\n");
		if (found)
			lhp_memzone_free(found);
		lhp_memzone_free(mz);
		return -EFAULT;
	}
	lhp_memzone_free(found);	/* drop lookup ref */

	pr_info("layer_test: memzone reserve/lookup/free cycle ok\n");
	lhp_memzone_free(mz);	/* drop reserve ref -> destroy */
	return 0;
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
	debugfs_create_file("layer_test", 0200, dir, NULL, &lhp_layer_test_fops);
	return 0;
}
late_initcall(lhp_debugfs_init);
#endif /* CONFIG_DEBUG_FS */

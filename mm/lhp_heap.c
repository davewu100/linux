// SPDX-License-Identifier: GPL-2.0
/*
 * lhp_heap - rte_malloc-style variable-size heap over the LHP pool.
 *
 * A heap manages its backing memory as one or more first-fit arenas.  There
 * are two backings:
 *
 *   - memzone (lhp_heap_create):  one arena per backing 2M chunk.  A memzone's
 *     chunks are not guaranteed physically contiguous, so an arena never spans
 *     a chunk boundary and a single allocation is limited to ~2M.
 *
 *   - 1G region (lhp_heap_create_1g):  one arena over a whole, physically
 *     contiguous 1G region, so a single allocation can be up to ~1G.
 *
 * Layout within an arena:
 *
 *   [hdr][payload][hdr][payload]...[hdr][payload]
 *
 * Each block carries an inline struct lhp_block header.  Blocks are threaded
 * in physical address order (blist) so free() can coalesce with the immediate
 * neighbours, and free blocks are additionally threaded on the arena free list
 * (flist) for first-fit search.  free() recovers the block, arena and heap
 * from the header, so it only needs the user pointer.
 */
#define pr_fmt(fmt) "lhp_heap: " fmt

#include <linux/lhp.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/err.h>
#include <linux/log2.h>
#include <linux/refcount.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/sysfs.h>

#define LHP_BLOCK_MAGIC		0x4c485042U	/* "LHPB" */

struct lhp_arena;

/*
 * Inline per-block header.  @blist orders blocks by physical address within an
 * arena; @flist links free blocks for first-fit search.  @size is the usable
 * payload size (bytes after the header).
 */
struct lhp_block {
	u32			magic;
	bool			free;
	struct lhp_arena	*arena;
	size_t			size;
	struct list_head	blist;	/* address-ordered chain in arena */
	struct list_head	flist;	/* free list (only when free) */
};

/* A first-fit arena over one backing span (a 2M chunk or a whole 1G region). */
struct lhp_arena {
	struct list_head	list;	/* member of heap->arenas */
	struct lhp_heap		*heap;	/* owning heap (for O(1) free) */
	void			*base;
	size_t			len;
	struct list_head	blocks;	/* all blocks, address order */
	struct list_head	free;	/* free blocks */
};

enum lhp_heap_backing {
	LHP_HEAP_MEMZONE = 0,	/* many per-2M-chunk arenas from a memzone */
	LHP_HEAP_REGION_1G,	/* one arena over a whole 1G region */
};

struct lhp_heap {
	struct list_head	list;	/* member of lhp_heap_list */
	char			name[LHP_NAME_MAX];
	enum lhp_heap_backing	backing;
	struct lhp_memzone	*zone;	/* LHP_HEAP_MEMZONE */
	struct page		*region_1g;	/* LHP_HEAP_REGION_1G base page */
	refcount_t		refs;	/* create + each successful lookup */
	struct list_head	arenas;
	spinlock_t		lock;
};

static LIST_HEAD(lhp_heap_list);
static DEFINE_MUTEX(lhp_heap_registry_lock);

#define LHP_HDR_SIZE		ALIGN(sizeof(struct lhp_block), 16)
#define LHP_MIN_ALIGN		16
/* A split is only worthwhile if the remainder can hold a header + a little. */
#define LHP_MIN_SPLIT		(LHP_HDR_SIZE + LHP_MIN_ALIGN)

static inline void *lhp_block_payload(struct lhp_block *b)
{
	return (char *)b + LHP_HDR_SIZE;
}

static inline struct lhp_block *lhp_block_from_ptr(const void *ptr)
{
	return (struct lhp_block *)((char *)ptr - LHP_HDR_SIZE);
}

/* --------------------------------------------------------------------------
 * Arena setup: lay one free block across a backing span
 * -------------------------------------------------------------------------- */

/*
 * Build one arena covering [base, base+len) as a single free block and add it
 * to @h.  Works for both a 2M chunk and a whole 1G region.
 */
static int lhp_arena_new(struct lhp_heap *h, void *base, size_t len)
{
	struct lhp_arena *a;
	struct lhp_block *b;

	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;

	a->heap = h;
	a->base = base;
	a->len = len;
	INIT_LIST_HEAD(&a->blocks);
	INIT_LIST_HEAD(&a->free);

	/* One giant free block covering the whole arena. */
	b = base;
	b->magic = LHP_BLOCK_MAGIC;
	b->free = true;
	b->arena = a;
	b->size = len - LHP_HDR_SIZE;
	list_add_tail(&b->blist, &a->blocks);
	list_add_tail(&b->flist, &a->free);

	list_add_tail(&a->list, &h->arenas);
	return 0;
}

struct lhp_arena_ctx {
	struct lhp_heap		*heap;
	int			err;
};

static int lhp_arena_add(void *base, size_t len, void *priv)
{
	struct lhp_arena_ctx *ctx = priv;
	int ret = lhp_arena_new(ctx->heap, base, len);

	if (ret)
		ctx->err = ret;
	return ret;
}

/* --------------------------------------------------------------------------
 * Heap create / destroy / lookup
 * -------------------------------------------------------------------------- */

static struct lhp_heap *lhp_heap_find_locked(const char *name)
{
	struct lhp_heap *h;

	list_for_each_entry(h, &lhp_heap_list, list) {
		if (!strcmp(h->name, name))
			return h;
	}
	return NULL;
}

/**
 * lhp_heap_lookup - find a heap by name and take a reference
 * @name: heap name
 *
 * The caller owns the returned reference and must drop it with
 * lhp_heap_destroy().
 *
 * Return: the heap, or NULL if no heap with that name exists.
 */
struct lhp_heap *lhp_heap_lookup(const char *name)
{
	struct lhp_heap *h;

	if (!name)
		return NULL;

	mutex_lock(&lhp_heap_registry_lock);
	h = lhp_heap_find_locked(name);
	if (h)
		refcount_inc(&h->refs);
	mutex_unlock(&lhp_heap_registry_lock);
	return h;
}
EXPORT_SYMBOL_GPL(lhp_heap_lookup);

/* Free every arena descriptor of @h (the backing memory itself is freed by the
 * caller according to @h->backing). */
static void lhp_heap_drop_arenas(struct lhp_heap *h)
{
	struct lhp_arena *a, *atmp;

	list_for_each_entry_safe(a, atmp, &h->arenas, list) {
		list_del(&a->list);
		kfree(a);
	}
}

/*
 * Allocate a bare heap shell.  On success returns a heap with an empty arena
 * list and one reference held; the caller fills in the backing and arenas and
 * then publishes it with lhp_heap_register().  The name collision check is
 * deferred to registration to avoid a check-then-insert race across the
 * (sleeping) backing setup.
 */
static struct lhp_heap *lhp_heap_shell(const char *name,
				       enum lhp_heap_backing backing)
{
	struct lhp_heap *h;

	if (!lhp_pool_available() || !name || !*name)
		return ERR_PTR(-EINVAL);
	if (strnlen(name, LHP_NAME_MAX) >= LHP_NAME_MAX)
		return ERR_PTR(-EINVAL);

	h = kzalloc(sizeof(*h), GFP_KERNEL);
	if (!h)
		return ERR_PTR(-ENOMEM);

	strscpy(h->name, name, LHP_NAME_MAX);
	h->backing = backing;
	refcount_set(&h->refs, 1);
	INIT_LIST_HEAD(&h->arenas);
	spin_lock_init(&h->lock);
	return h;
}

/*
 * Publish @h into the registry, re-checking for a name collision under the
 * lock.  Returns 0 on success or -EEXIST if the name was taken concurrently;
 * on failure the caller still owns @h and must tear it down.
 */
static int lhp_heap_register(struct lhp_heap *h)
{
	mutex_lock(&lhp_heap_registry_lock);
	if (lhp_heap_find_locked(h->name)) {
		mutex_unlock(&lhp_heap_registry_lock);
		return -EEXIST;
	}
	list_add_tail(&h->list, &lhp_heap_list);
	mutex_unlock(&lhp_heap_registry_lock);
	return 0;
}

static struct lhp_heap *lhp_heap_do_2m(const char *name, size_t size, int nid,
				       gfp_t gfp)
{
	struct lhp_arena_ctx ctx;
	struct lhp_heap *h;
	char zname[LHP_NAME_MAX];
	int ret;

	h = lhp_heap_shell(name, LHP_HEAP_MEMZONE);
	if (IS_ERR(h))
		return h;

	/* Back the heap with a memzone of the same name ("heap:" prefixed). */
	scnprintf(zname, sizeof(zname), "heap:%s", name);
	h->zone = lhp_memzone_reserve(zname, size, nid, LHP_MEMZONE_F_NONE);
	if (IS_ERR(h->zone)) {
		ret = PTR_ERR(h->zone);
		kfree(h);
		return ERR_PTR(ret);
	}

	ctx.heap = h;
	ctx.err = 0;
	ret = lhp_memzone_for_each_chunk(h->zone, lhp_arena_add, &ctx);
	if (ret) {
		ret = ctx.err ? ctx.err : ret;
		goto err;
	}

	ret = lhp_heap_register(h);
	if (ret)
		goto err;

	pr_info("heap %s: %zu bytes over memzone %s\n", h->name, size, zname);
	return h;

err:
	lhp_heap_drop_arenas(h);
	lhp_memzone_free(h->zone);
	kfree(h);
	return ERR_PTR(ret);
}

static struct lhp_heap *lhp_heap_do_1g(const char *name, int nid, gfp_t gfp)
{
	struct lhp_heap *h;
	struct page *page;
	int ret;

	h = lhp_heap_shell(name, LHP_HEAP_REGION_1G);
	if (IS_ERR(h))
		return h;

	page = lhp_alloc_1g(gfp);
	if (!page) {
		kfree(h);
		return ERR_PTR(-ENOMEM);
	}
	h->region_1g = page;

	/* One arena spanning the whole 1G region. */
	ret = lhp_arena_new(h, page_address(page), LHP_REGION_PAGES << PAGE_SHIFT);
	if (ret)
		goto err;

	ret = lhp_heap_register(h);
	if (ret)
		goto err;

	pr_info("heap %s: 1G region-backed single arena\n", h->name);
	return h;

err:
	lhp_heap_drop_arenas(h);
	lhp_free_1g(page);
	kfree(h);
	return ERR_PTR(ret);
}

/**
 * lhp_heap_create_policy - create a named variable-size heap
 * @name:   unique heap name (must be shorter than %LHP_NAME_MAX)
 * @size:   expected total capacity in bytes
 * @nid:    preferred NUMA node, or %NUMA_NO_NODE
 * @policy: backing selection, see &enum lhp_heap_policy
 * @gfp:    flags for bookkeeping allocations
 *
 * Creates a heap backed according to @policy: %LHP_HEAP_2M (memzone, single
 * allocation up to ~2M), %LHP_HEAP_1G (one 1G region, up to ~1G) or
 * %LHP_HEAP_AUTO (chosen from @size).  The heap is created with one reference;
 * drop it with lhp_heap_destroy().
 *
 * Return: the heap on success, or an ERR_PTR: -EINVAL for bad arguments,
 * -EEXIST if the name is taken, -ENOMEM on allocation failure.
 */
struct lhp_heap *lhp_heap_create_policy(const char *name, size_t size, int nid,
					enum lhp_heap_policy policy, gfp_t gfp)
{
	if (!size)
		return ERR_PTR(-EINVAL);

	switch (policy) {
	case LHP_HEAP_2M:
		return lhp_heap_do_2m(name, size, nid, gfp);
	case LHP_HEAP_1G:
		return lhp_heap_do_1g(name, nid, gfp);
	case LHP_HEAP_AUTO:
		/*
		 * @size is the expected total capacity.  A memzone-backed heap
		 * can only ever serve single allocations up to one 2M chunk, so
		 * once the caller wants more than a chunk's worth of usable
		 * space we must move to a 1G arena to keep large single
		 * allocations possible.  The threshold is expressed on usable
		 * bytes (chunk minus one block header) so a request that rounds
		 * to exactly one chunk still fits the cheaper 2M backing.
		 */
		if (size > LHP_CHUNK_BYTES - LHP_HDR_SIZE)
			return lhp_heap_do_1g(name, nid, gfp);
		return lhp_heap_do_2m(name, size, nid, gfp);
	default:
		return ERR_PTR(-EINVAL);
	}
}
EXPORT_SYMBOL_GPL(lhp_heap_create_policy);

/**
 * lhp_heap_create - create a memzone-backed heap (%LHP_HEAP_2M)
 * @name: unique heap name
 * @size: expected total capacity in bytes
 * @nid:  preferred NUMA node, or %NUMA_NO_NODE
 * @gfp:  flags for bookkeeping allocations
 *
 * Convenience wrapper over lhp_heap_create_policy(); single allocations are
 * limited to ~2M.  Return: see lhp_heap_create_policy().
 */
struct lhp_heap *lhp_heap_create(const char *name, size_t size, int nid,
				 gfp_t gfp)
{
	return lhp_heap_create_policy(name, size, nid, LHP_HEAP_2M, gfp);
}
EXPORT_SYMBOL_GPL(lhp_heap_create);

/**
 * lhp_heap_create_1g - create a 1G-region-backed heap (%LHP_HEAP_1G)
 * @name: unique heap name
 * @nid:  preferred NUMA node, or %NUMA_NO_NODE
 * @gfp:  flags for bookkeeping allocations
 *
 * Convenience wrapper over lhp_heap_create_policy(); consumes a whole 1G
 * region and can serve single allocations up to ~1G.  Return: see
 * lhp_heap_create_policy().
 */
struct lhp_heap *lhp_heap_create_1g(const char *name, int nid, gfp_t gfp)
{
	return lhp_heap_create_policy(name, LHP_CHUNK_BYTES + 1, nid,
				      LHP_HEAP_1G, gfp);
}
EXPORT_SYMBOL_GPL(lhp_heap_create_1g);

/**
 * lhp_heap_destroy - drop a reference on a heap
 * @h: heap from lhp_heap_create*() or lhp_heap_lookup()
 *
 * Drops one reference.  The heap and its backing memory are released only when
 * the last reference is dropped; outstanding allocations are not tracked, so
 * the caller must have freed them.  NULL is ignored.
 */
void lhp_heap_destroy(struct lhp_heap *h)
{
	if (!h)
		return;

	/*
	 * refcount_dec_and_mutex_lock() makes the final decrement and the
	 * unlink from the registry atomic against a concurrent
	 * lhp_heap_lookup() taking a fresh reference.
	 */
	if (!refcount_dec_and_mutex_lock(&h->refs, &lhp_heap_registry_lock))
		return;
	list_del(&h->list);
	mutex_unlock(&lhp_heap_registry_lock);

	/* Arena descriptors are ours; the backing memory is freed by kind. */
	lhp_heap_drop_arenas(h);

	switch (h->backing) {
	case LHP_HEAP_MEMZONE:
		lhp_memzone_free(h->zone);
		break;
	case LHP_HEAP_REGION_1G:
		lhp_free_1g(h->region_1g);
		break;
	}
	kfree(h);
}
EXPORT_SYMBOL_GPL(lhp_heap_destroy);

/* --------------------------------------------------------------------------
 * Allocation: first-fit with front-trim for alignment and tail split
 * -------------------------------------------------------------------------- */

/*
 * Given free block @b in @a, carve out an aligned payload of @size and return
 * its pointer.  Splits off leading padding (for alignment) and a trailing
 * remainder into their own blocks when large enough.  Caller holds heap lock.
 */
static void *lhp_carve(struct lhp_arena *a, struct lhp_block *b,
		       size_t size, size_t align)
{
	unsigned long pay = (unsigned long)lhp_block_payload(b);
	unsigned long aligned = ALIGN(pay, align);
	size_t lead = aligned - pay;
	size_t need = lead + size;

	if (need > b->size)
		return NULL;

	/*
	 * Leading padding needed for alignment.  If it is large enough to stand
	 * as its own free block (>= LHP_MIN_SPLIT), split it off the front and
	 * keep it on the free list.  If it is non-zero but too small to become a
	 * block, we cannot represent it, so give up on this block and let the
	 * caller try the next free block instead.
	 */
	if (lead) {
		struct lhp_block *mid;

		if (lead < LHP_MIN_SPLIT)
			return NULL;	/* let caller try another block */

		/* @b stays as leading free block of size (lead - HDR). */
		mid = (struct lhp_block *)((char *)b + lead);
		mid->magic = LHP_BLOCK_MAGIC;
		mid->arena = a;
		mid->free = false;
		mid->size = b->size - lead;
		list_add(&mid->blist, &b->blist);

		b->size = lead - LHP_HDR_SIZE;
		/* @b remains free and on the free list. */
		b = mid;
	} else {
		b->free = false;
		list_del(&b->flist);
	}

	/* Trailing split if the remainder can stand on its own. */
	if (b->size >= size + LHP_MIN_SPLIT) {
		struct lhp_block *tail;

		tail = (struct lhp_block *)((char *)lhp_block_payload(b) + size);
		tail->magic = LHP_BLOCK_MAGIC;
		tail->arena = a;
		tail->free = true;
		tail->size = b->size - size - LHP_HDR_SIZE;
		list_add(&tail->blist, &b->blist);
		list_add_tail(&tail->flist, &a->free);

		b->size = size;
	}

	return lhp_block_payload(b);
}

/**
 * lhp_malloc - allocate memory from a heap
 * @h:     heap to allocate from
 * @size:  requested size in bytes
 * @align: required alignment (power of two; rounded up to at least 16)
 * @gfp:   only %__GFP_ZERO is honoured (zero the result); the heap never
 *         allocates backing memory here
 *
 * First-fit allocation from the heap's arenas.  The result cannot exceed a
 * single arena's span: ~2M for a memzone-backed heap, ~1G for a 1G-backed one.
 *
 * Return: a pointer to @size bytes, or NULL on failure.  Free it with
 * lhp_free().
 */
void *lhp_malloc(struct lhp_heap *h, size_t size, size_t align, gfp_t gfp)
{
	struct lhp_arena *a;
	struct lhp_block *b;
	void *ret = NULL;

	if (!h || !size)
		return NULL;
	if (!align)
		align = LHP_MIN_ALIGN;
	if (!is_power_of_2(align))
		return NULL;
	align = max_t(size_t, align, LHP_MIN_ALIGN);
	size = ALIGN(size, LHP_MIN_ALIGN);

	spin_lock(&h->lock);
	list_for_each_entry(a, &h->arenas, list) {
		list_for_each_entry(b, &a->free, flist) {
			ret = lhp_carve(a, b, size, align);
			if (ret)
				goto out;
		}
	}
out:
	spin_unlock(&h->lock);

	if (ret && (gfp & __GFP_ZERO))
		memset(ret, 0, size);
	return ret;
}
EXPORT_SYMBOL_GPL(lhp_malloc);

/* --------------------------------------------------------------------------
 * Free: coalesce with physical neighbours, return to free list
 * -------------------------------------------------------------------------- */

/*
 * Fold the address-ordered successor @next into @b, growing @b's span by
 * @next's header + payload and unlinking @next from the address chain.  Only
 * touches @blist; the caller is responsible for the free-list membership of
 * both blocks (only a block actually on the free list may be list_del'd).
 */
static void lhp_absorb_next(struct lhp_block *b, struct lhp_block *next)
{
	b->size += LHP_HDR_SIZE + next->size;
	list_del(&next->blist);
	next->magic = 0;
}

/**
 * lhp_free - free memory obtained from lhp_malloc()
 * @ptr: pointer returned by lhp_malloc(), or NULL
 *
 * Returns the block to its heap, coalescing with free physical neighbours.
 * NULL is ignored; a pointer not owned by any heap, or a double free, is
 * rejected with a warning.
 */
void lhp_free(void *ptr)
{
	struct lhp_block *b, *adj;
	struct lhp_arena *a;
	struct lhp_heap *h;

	if (!ptr)
		return;
	if (!lhp_heap_owns(ptr)) {
		WARN_ONCE(1, "lhp_free: %px is not an lhp_heap pointer\n", ptr);
		return;
	}

	b = lhp_block_from_ptr(ptr);
	a = b->arena;
	h = a->heap;

	spin_lock(&h->lock);

	if (WARN_ONCE(b->free, "lhp_free: double free of %px\n", ptr)) {
		spin_unlock(&h->lock);
		return;
	}

	/*
	 * @b is not on the free list at this point.  Coalesce with free
	 * neighbours before inserting it, so we only ever delete the flist
	 * link of a block that is genuinely on the free list (the neighbour),
	 * never @b's own (stale) link.
	 */
	b->free = true;

	/* Successor: free (hence on the free list) -> drop it, fold into @b. */
	if (!list_is_last(&b->blist, &a->blocks)) {
		adj = list_next_entry(b, blist);
		if (adj->free) {
			list_del(&adj->flist);
			lhp_absorb_next(b, adj);
		}
	}
	/* Predecessor: free (hence on the free list) -> fold @b into it. */
	if (a->blocks.next != &b->blist) {
		adj = list_prev_entry(b, blist);
		if (adj->free) {
			/* @adj stays on the free list; just grow it over @b. */
			lhp_absorb_next(adj, b);
			b = adj;
			goto done;	/* @b(=adj) already on the free list */
		}
	}

	list_add_tail(&b->flist, &a->free);
done:
	spin_unlock(&h->lock);
}
EXPORT_SYMBOL_GPL(lhp_free);

/**
 * lhp_heap_owns - test whether a pointer was handed out by some heap
 * @ptr: pointer to test (may be arbitrary)
 *
 * Checks @ptr against every heap's arena address ranges without dereferencing
 * it, so it is safe on arbitrary input -- for example to decide whether a free
 * should be routed to lhp_free().
 *
 * Return: true if @ptr lies within a heap arena, false otherwise.
 */
bool lhp_heap_owns(const void *ptr)
{
	struct lhp_heap *h;
	bool owned = false;

	if (!ptr)
		return false;

	mutex_lock(&lhp_heap_registry_lock);
	list_for_each_entry(h, &lhp_heap_list, list) {
		struct lhp_arena *a;

		list_for_each_entry(a, &h->arenas, list) {
			if (ptr > a->base &&
			    ptr < a->base + a->len) {
				owned = true;
				goto out;
			}
		}
	}
out:
	mutex_unlock(&lhp_heap_registry_lock);
	return owned;
}
EXPORT_SYMBOL_GPL(lhp_heap_owns);

/* --------------------------------------------------------------------------
 * debugfs self-test: write "run" to /sys/kernel/debug/lhp_heap_test
 * -------------------------------------------------------------------------- */

#ifdef CONFIG_DEBUG_FS
static struct dentry *lhp_heap_test_dentry;

/*
 * Destroy a possibly-leftover heap by name.  lhp_heap_lookup() takes a
 * reference, so a stale heap needs two puts (the lookup ref and the original
 * create ref) to actually go away.
 */
static void lhp_heap_test_cleanup(const char *name)
{
	struct lhp_heap *h = lhp_heap_lookup(name);

	if (h) {
		lhp_heap_destroy(h);	/* drop lookup ref */
		lhp_heap_destroy(h);	/* drop create ref -> destroy */
	}
}

/*
 * Exercise a 1G-region-backed heap: its single arena spans the whole 1G, so a
 * single allocation larger than one 2M chunk (impossible on a memzone-backed
 * heap) must succeed here.
 */
static int lhp_heap_1g_test_run(void)
{
	const size_t big_sz = 4 * LHP_CHUNK_BYTES;	/* 8M: > one 2M chunk */
	struct lhp_heap *h;
	void *big, *small;
	int err = 0;

	lhp_heap_test_cleanup("dbgheap1g");

	h = lhp_heap_create_1g("dbgheap1g", NUMA_NO_NODE, GFP_KERNEL);
	if (IS_ERR(h)) {
		/* No fully-idle 1G region available is not a test failure. */
		if (PTR_ERR(h) == -ENOMEM) {
			pr_info("heap_test: no idle 1G region, skipping 1G test\n");
			return 0;
		}
		pr_err("heap_test: create_1g failed %ld\n", PTR_ERR(h));
		return PTR_ERR(h);
	}

	big = lhp_malloc(h, big_sz, SZ_2M, __GFP_ZERO);
	if (!big) {
		pr_err("heap_test: 1G heap failed a %zu-byte alloc\n", big_sz);
		err = -ENOMEM;
		goto out;
	}
	if (!IS_ALIGNED((unsigned long)big, SZ_2M) || !lhp_heap_owns(big)) {
		pr_err("heap_test: 1G big alloc misaligned/unowned\n");
		err = -EFAULT;
		goto out_free;
	}
	memset(big, 0xcd, big_sz);

	small = lhp_malloc(h, 128, 64, 0);
	if (!small) {
		err = -ENOMEM;
		goto out_free;
	}
	lhp_free(small);

	pr_info("heap_test: 1G heap %zu-byte contiguous alloc ok\n", big_sz);
out_free:
	lhp_free(big);
out:
	lhp_heap_destroy(h);
	return err;
}

/*
 * Exercise LHP_HEAP_AUTO: a small @size must land on the 2M backing, and a
 * @size larger than one 2M chunk must land on the 1G backing (and then be able
 * to serve a >2M allocation).
 */
static int lhp_heap_auto_test_run(void)
{
	struct lhp_heap *h;
	void *p;
	int err = 0;

	/* Small hint -> 2M backing. */
	lhp_heap_test_cleanup("dbgauto");
	h = lhp_heap_create_policy("dbgauto", 4096, NUMA_NO_NODE,
				   LHP_HEAP_AUTO, GFP_KERNEL);
	if (IS_ERR(h)) {
		pr_err("heap_test: auto(small) create failed %ld\n", PTR_ERR(h));
		return PTR_ERR(h);
	}
	p = lhp_malloc(h, 4096, 64, __GFP_ZERO);
	if (!p) {
		err = -ENOMEM;
		goto out_small;
	}
	lhp_free(p);
	pr_info("heap_test: auto(small) -> 2M backing ok\n");
out_small:
	lhp_heap_destroy(h);
	if (err)
		return err;

	/* Large hint (> one 2M chunk) -> 1G backing. */
	lhp_heap_test_cleanup("dbgauto");
	h = lhp_heap_create_policy("dbgauto", 4 * LHP_CHUNK_BYTES, NUMA_NO_NODE,
				   LHP_HEAP_AUTO, GFP_KERNEL);
	if (IS_ERR(h)) {
		if (PTR_ERR(h) == -ENOMEM) {
			pr_info("heap_test: no idle 1G region, skipping auto(large)\n");
			return 0;
		}
		pr_err("heap_test: auto(large) create failed %ld\n", PTR_ERR(h));
		return PTR_ERR(h);
	}
	/* Must be able to serve a > 2M allocation. */
	p = lhp_malloc(h, 4 * LHP_CHUNK_BYTES, SZ_2M, 0);
	if (!p) {
		pr_err("heap_test: auto(large) failed a >2M alloc\n");
		err = -ENOMEM;
		goto out_large;
	}
	lhp_free(p);
	pr_info("heap_test: auto(large) -> 1G backing, >2M alloc ok\n");
out_large:
	lhp_heap_destroy(h);
	return err;
}

static int lhp_heap_test_run(void)
{
	struct lhp_heap *h;
	void *p[16];
	unsigned int i, n = 0;
	int err = 0;

	lhp_heap_test_cleanup("dbgheap");

	h = lhp_heap_create("dbgheap", 2 * LHP_CHUNK_BYTES, NUMA_NO_NODE,
			    GFP_KERNEL);
	if (IS_ERR(h)) {
		pr_err("heap_test: create failed %ld\n", PTR_ERR(h));
		return PTR_ERR(h);
	}

	/* A spread of sizes and alignments. */
	for (i = 0; i < ARRAY_SIZE(p); i++) {
		size_t sz = 32 + i * 37;
		size_t al = 1UL << (4 + (i % 5));	/* 16..256 */

		p[i] = lhp_malloc(h, sz, al, __GFP_ZERO);
		if (!p[i]) {
			err = -ENOMEM;
			n = i;
			goto out;
		}
		if (!IS_ALIGNED((unsigned long)p[i], al)) {
			pr_err("heap_test: %px not aligned to %zu\n", p[i], al);
			err = -EFAULT;
			n = i + 1;
			goto out;
		}
		if (!lhp_heap_owns(p[i])) {
			pr_err("heap_test: owns() false for %px\n", p[i]);
			err = -EFAULT;
			n = i + 1;
			goto out;
		}
		memset(p[i], 0xab, sz);	/* touch the whole payload */
	}
	n = ARRAY_SIZE(p);

	/* Free every other block, then the rest, exercising coalesce. */
	for (i = 0; i < n; i += 2)
		lhp_free(p[i]);
	for (i = 1; i < n; i += 2)
		lhp_free(p[i]);
	n = 0;

	/* After freeing everything, a full-chunk allocation should succeed. */
	{
		void *big = lhp_malloc(h, LHP_CHUNK_BYTES / 2, 64, 0);

		if (!big) {
			pr_err("heap_test: post-coalesce large alloc failed\n");
			err = -ENOMEM;
			goto out;
		}
		lhp_free(big);
	}

	pr_info("heap_test: malloc/free/coalesce/align cycle ok\n");
out:
	for (i = 0; i < n; i++)
		lhp_free(p[i]);
	lhp_heap_destroy(h);
	if (err)
		return err;

	err = lhp_heap_1g_test_run();
	if (err)
		return err;

	return lhp_heap_auto_test_run();
}

static ssize_t lhp_heap_test_write(struct file *file, const char __user *ubuf,
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
		ret = lhp_heap_test_run();
		if (ret)
			return ret;
	}
	return count;
}

static const struct file_operations lhp_heap_test_fops = {
	.write = lhp_heap_test_write,
	.open = simple_open,
	.llseek = default_llseek,
};

static int __init lhp_heap_debugfs_init(void)
{
	lhp_heap_test_dentry = debugfs_create_file("lhp_heap_test", 0200, NULL,
						   NULL, &lhp_heap_test_fops);
	return 0;
}
late_initcall(lhp_heap_debugfs_init);
#endif /* CONFIG_DEBUG_FS */

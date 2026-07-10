.. SPDX-License-Identifier: GPL-2.0

=========================
LHP - Large HugePage pool
=========================

Overview
========

LHP is a self-contained large-page memory pool.  It reserves a physically
contiguous area at boot via CMA, carves it into 1G regions, and hands memory
out at two granularities on top of a DPDK-style layering.

The pool never returns its pages to the buddy allocator for its lifetime, so
physical contiguity within a region is preserved: 2M is the small allocation
unit and a whole 1G region is the large one.  There is no split/merge machinery
and no 4K path -- chunks are handed out and returned whole, so a region never
changes shape and its contiguity is guaranteed by the underlying 1G CMA block.

This targets static, DPDK-style consumers that reserve their backing memory up
front (memzones or heaps) and lay it out themselves, rather than a general
kmalloc replacement.

Layering
========

::

    physical memory (CMA reserve at boot)
         |
         v
    lhp_phys_pool           N x 1G regions, each a container of 512 2M chunks
         |
         +--> lhp_alloc_2m()    one 2M chunk        (O(1) per-region bitmap)
         +--> lhp_alloc_1g()    one whole 1G region (single contiguous block)
         |
         +--> lhp_memzone       named contiguous zone built from 2M chunks
         |                      (analogous to rte_memzone)
         |
         +--> lhp_heap          rte_malloc-style variable-size allocator,
                                backed by a memzone or by a whole 1G region

phys pool
---------

Each 1G region is a fixed container of 512 2M chunks tracked by a per-region
allocation bitmap.  Allocation and free of a 2M chunk are O(1) bitmap
operations under a per-region spinlock; a shared round-robin cursor spreads
allocations across regions so CPUs hitting different regions do not contend.

A whole region can instead be handed out as a single 1G block
(``lhp_alloc_1g()``).  A region is eligible for a 1G allocation only when it is
completely idle (no 2M chunks carved out); while held as 1G it is off-limits to
the 2M carver, so the two granularities never overlap within a region.

Because 2M load can leave every region with at least one chunk in use and thus
ineligible for 1G, the ``lhp_1g=<n>`` command line option can dedicate ``n``
regions exclusively to 1G allocation.  Dedicated regions are never carved into
2M chunks and therefore stay available for 1G allocation regardless of 2M load.

memzone
-------

A memzone is a named, contiguous-by-request backing range carved as a set of 2M
chunks, analogous to ``rte_memzone_reserve()``.  It is intended for static
consumers that reserve a region of memory once and manage its contents
themselves.  The requested length is rounded up to a 2M multiple.  Note that a
memzone's individual 2M chunks are not guaranteed to be physically contiguous
with one another.

Memzones are kept in a named registry.  ``lhp_memzone_lookup()`` takes a
reference on the returned zone; the caller drops it with
``lhp_memzone_free()``, which destroys the zone once the last reference is
gone.

heap
----

The heap layer is an ``rte_malloc``-style variable-size allocator built on the
pool.  A heap manages its backing memory as one or more first-fit *arenas* with
coalescing free lists.  Each allocation carries a small inline header, so
``lhp_free()`` needs only the pointer -- the owning block, arena and heap are
recovered from the header.

The backing determines the maximum single allocation:

``LHP_HEAP_2M``
    Memzone-backed.  Each backing 2M chunk becomes an independent arena.
    Because the chunks are not mutually contiguous, an arena never spans a
    chunk boundary, so a single allocation cannot exceed one chunk (~2M minus
    the block header).  The heap only consumes as many 2M chunks as its size
    needs.

``LHP_HEAP_1G``
    Backed by one whole 1G region as a single contiguous arena, so a single
    allocation can be up to ~1G.  The heap consumes an entire 1G region
    regardless of its size.

``LHP_HEAP_AUTO``
    Picks the backing from the requested total size: once the size exceeds one
    chunk's worth of usable bytes, a 1G region is used so that large single
    allocations remain possible; otherwise the cheaper memzone backing is used.

Heaps are kept in a named registry and reference counted the same way as
memzones: ``lhp_heap_lookup()`` takes a reference and ``lhp_heap_destroy()``
drops one, tearing the heap down on the last put.

Boot-time reservation
=====================

LHP does not draw from the system hugetlb pool; it reserves its own CMA area at
boot.  Enable it on the kernel command line:

``lhp=<size>``
    Reserve ``<size>`` bytes (rounded up to a 1G multiple) as the LHP pool.
    Without this option LHP stays inert.

``lhp_1g=<n>``
    Dedicate ``n`` of the reserved 1G regions exclusively to whole-1G
    allocation.  Clamped to the number of regions actually obtained.  Use this
    when 1G allocations must always succeed regardless of 2M load.

The reservation is wired into the architecture memory-reserve path alongside
``hugetlb_cma_reserve()`` (memblock up, buddy allocator not yet active).

Programming interface
=====================

The pool becomes usable only after a late initcall has pulled the 1G blocks out
of CMA.  Callers must check ``lhp_pool_available()`` (all allocation entry
points also fail gracefully before the pool is ready).

Chunk / region level::

    int              lhp_pool_available(void);
    struct page     *lhp_alloc_2m(gfp_t gfp);
    void             lhp_free_2m(struct page *page);
    struct page     *lhp_alloc_1g(gfp_t gfp);
    void             lhp_free_1g(struct page *page);

The ``gfp`` argument only controls incidental bookkeeping allocations; the
backing pages themselves are never allocated from the buddy allocator.

Memzone level::

    struct lhp_memzone *lhp_memzone_reserve(const char *name, size_t len,
                                            int nid, unsigned long flags);
    struct lhp_memzone *lhp_memzone_lookup(const char *name);
    void                lhp_memzone_free(struct lhp_memzone *zone);
    int  lhp_memzone_for_each_chunk(struct lhp_memzone *zone,
                                    lhp_chunk_fn fn, void *priv);

Heap level::

    struct lhp_heap *lhp_heap_create_policy(const char *name, size_t size,
                                            int nid,
                                            enum lhp_heap_policy policy,
                                            gfp_t gfp);
    struct lhp_heap *lhp_heap_create(const char *name, size_t size,
                                     int nid, gfp_t gfp);      /* 2M */
    struct lhp_heap *lhp_heap_create_1g(const char *name, int nid, gfp_t gfp);
    struct lhp_heap *lhp_heap_lookup(const char *name);
    void             lhp_heap_destroy(struct lhp_heap *h);

    void *lhp_malloc(struct lhp_heap *h, size_t size, size_t align, gfp_t gfp);
    void  lhp_free(void *ptr);
    bool  lhp_heap_owns(const void *ptr);

``lhp_heap_create()`` and ``lhp_heap_create_1g()`` are thin wrappers over
``lhp_heap_create_policy()`` with ``LHP_HEAP_2M`` and ``LHP_HEAP_1G``
respectively.  ``lhp_heap_owns()`` range-checks a pointer against the heap
arenas and is safe to call on arbitrary input (for example, to route a free).

Concurrency and lifetime
========================

* Locking is per-region: each region has its own spinlock, so allocations on
  different regions never contend.  Only one region lock is held at a time.
* The pool is published to consumers with release/acquire ordering on a ready
  flag, so a reader that sees the pool ready also sees the fully initialised
  region array.
* Named registries (memzones, heaps) are reference counted, so a lookup is safe
  against a concurrent destroy, and create paths re-check for a name collision
  under the registry lock before publishing.

debugfs
=======

When ``CONFIG_DEBUG_FS`` is enabled, ``/sys/kernel/debug/lhp/`` provides:

``stats`` (read)
    Aggregated per-region counters: number of regions, idle 2M-capable
    regions, 1G regions (in use and dedicated), 2M chunk totals, and a list of
    memzones.

``alloc`` (write)
    Smoke test that allocates and immediately frees one 2M chunk.

``layer_test`` (write ``run``)
    Self-test of the memzone reserve/lookup/free cycle.

With ``CONFIG_LHP_HEAP``, ``/sys/kernel/debug/lhp_heap_test`` (write ``run``)
exercises the heap malloc/free/coalesce/alignment paths, including a 1G-backed
heap serving an allocation larger than a 2M chunk.

Configuration
=============

``CONFIG_LHP``
    Build the pool and memzone layer.  Depends on ``CMA``, ``CONTIG_ALLOC`` and
    ``64BIT``.

``CONFIG_LHP_HEAP``
    Build the ``rte_malloc``-style heap layer on top of the pool.

``CONFIG_LHP_BENCH``
    Build a standalone, loadable microbenchmark module that drives the 2M chunk
    allocator through its public API and reports ns/op (optionally with
    concurrent threads) via ``/sys/kernel/debug/lhp_bench``.

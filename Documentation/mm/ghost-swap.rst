.. SPDX-License-Identifier: GPL-2.0

==========
Ghost swap
==========

Overview
========

A *ghost* swap area is a virtual swap area that has only a header page on disk
and no data section.  Its swap slots are allocated dynamically and their
metadata is kept in memory rather than on a physical device.  A ghost area
gives zswap (or zram) a virtual swap address space without reserving physical
swap slots, and is meant to be the top tier of a layered swap setup: pages live
compressed in the zswap pool while occupying only ghost slots, and are spilled
to a real device only when memory pressure forces a writeback.

Ghost swap is experimental and guarded by ``CONFIG_SWAP_GHOST`` (depends on
``SWAP`` and ``64BIT``, default n).

This document describes the in-tree prototype.  It is a milestone-based
implementation of the "swap table phase IV / virtual swap space" direction and
is not yet a complete, upstream-ready feature (see `Limitations`_).

Detecting a ghost area
======================

``read_swap_header()`` recognises a ghost area as one whose backing file is a
single page (``i_size == PAGE_SIZE``) yet whose header advertises a larger
``last_page``.  Such an area is tagged ``SWP_GHOST`` instead of being rejected
with *"Swap area shorter than signature indicates"*.

At ``swapon`` time a ghost area is treated as backing-device-less: any bdev
derived from the containing filesystem is dropped, ``SWP_BLKDEV`` is cleared,
and the area is marked ``SWP_SOLIDSTATE | SWP_SYNCHRONOUS_IO`` (it is never
rotational).  It has no physical extents, so extent setup is skipped.

A ``nr_real_swapfiles`` counter tracks how many non-ghost areas are active.
``swap_has_real_swapfile()`` exposes it so zswap can suppress writeback when the
only swap is ghost (there is nowhere to write back to).

Dynamic clusters
================

A normal swap area keeps a flat ``cluster_info[]`` array covering its whole
size.  A ghost area instead allocates clusters on demand and stores them in an
xarray, ``swap_info_struct.cluster_info_pool``, keyed by cluster index.  This
keeps per-slot overhead at zero for normal areas while letting a ghost area
present a large virtual capacity that only consumes memory for clusters that
actually hold data.

Each dynamic cluster is a ``struct swap_cluster_info_dynamic``::

    struct swap_cluster_info_dynamic {
        struct swap_cluster_info ci;      /* first member: shared paths */
        unsigned int            index;    /* cluster index in the area */
        struct rcu_head         rcu;      /* deferred free */
        unsigned long          *virtual_table; /* PHYS_COMPRESSED payload */
    };

The embedded ``swap_cluster_info`` is the first member, so a pointer to it is
interchangeable with a normal cluster pointer on the shared allocation and
locking paths.  ``__swap_offset_to_cluster()`` dispatches to an xarray lookup
for ghost areas, and ``cluster_index()`` reads the index from the wrapper
instead of doing pointer arithmetic into a base array.

``alloc_swap_scan_dynamic()`` is the ghost counterpart of
``cluster_alloc_swap_entry()``: it walks the index space, materialising fresh
clusters as needed, and allocates a slot through the shared
``alloc_swap_scan_cluster()``.

IO behaviour
============

Because a ghost area has no backing store of its own:

* ``swap_writeout()``: if zswap does not take a ghost-backed folio, there is
  nowhere to write it, so the folio is marked dirty and activated
  (``AOP_WRITEPAGE_ACTIVATE``) to keep it in memory.

* ``swap_read_folio()``: a ghost slot that misses both the zeromap and zswap
  either was spilled to a real device (see below) or never had on-disk data.
  In the latter case the folio is simply marked uptodate.

Spilling to a real device
==========================

When a ghost-backed folio must leave memory and a real device is available, it
is *spilled*: its data moves to a physical slot while its metadata stays on the
ghost side, linked by a reverse map.

* ``folio_realloc_swap()`` allocates an order-0 backing slot on a real
  (non-ghost) device via ``swap_alloc_real_backing()``, records a reverse map
  from the physical slot to the ghost slot, and repoints ``folio->swap`` at the
  physical slot for the write.

* Two xarrays link the tiers: a reverse map (physical entry -> ghost entry) and
  a forward map (ghost entry -> physical entry).  The reverse map lets a
  physical-side operation find the ghost metadata; the forward map lets a
  swapin/swapoff of the ghost slot find where its data was spilled.

* ``zswap_writeback_entry()`` calls ``folio_realloc_swap()`` for ghost-backed
  entries before issuing the write; if no real device has room, the folio is
  kept in memory.

* On swapin, ``swap_read_folio()`` consults the forward map, retargets the
  folio at the physical slot, and reads from the real device.

* When a ghost slot is finally freed, ``swap_range_free()`` releases the
  physical backing slot and both map directions.  Freeing a physical slot
  erases any stale reverse map so a recycled slot is never misread.

Compressed writeback (PHYS_COMPRESSED)
======================================

To avoid decompressing a zswap entry on writeback only to have a later swapin
recompress it, a spilled ghost slot can store the zswap compressed blob
*verbatim* on the backing device.

* Each dynamic cluster's ``virtual_table`` holds one tag-encoded descriptor
  word per slot: a *present* flag, the compressed length, and a codec id.  It
  is allocated lazily and freed with the cluster.

* On writeback, ``zswap_copy_blob_to_folio()`` copies the entry's compressed
  bytes into the folio as-is (zero-padded) instead of decompressing.  The
  descriptor ``{clen, algo_id}`` is recorded on the ghost slot *before* the
  spill, so a blob never exists on disk without its descriptor.  Only genuinely
  compressed sub-page blobs qualify; others fall back to decompression.

* The codec id comes from a small name registry that interns crypto tfm names
  (``zswap_copy_blob_to_folio()`` records the id, ``zswap_decompress_blob()``
  resolves it back), so a blob is always decoded with the exact codec that
  produced it.

* On swapin, ``swap_read_folio()`` reads the blob synchronously without marking
  the folio uptodate, then ``zswap_decompress_blob()`` decodes it in place with
  the recorded codec; only then is the folio marked uptodate and unlocked.

Limitations
===========

The prototype is intentionally simple and has not been runtime tested:

* Order-0 pages and block-backed backing devices only.
* The reverse/forward maps and the PHYS_COMPRESSED descriptors use standalone
  xarrays / a per-cluster ``virtual_table`` as an interim.  In the full virtual
  swap design this state moves into the physical cluster's swap table; the
  interfaces are shaped so callers do not change when it does.
* Swapin decompression allocates a transient acomp per call rather than reusing
  a per-CPU codec context.
* The codec name registry is bounded (31 entries) and never freed.

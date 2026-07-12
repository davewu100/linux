# RFC design: compressed writeback in core swap via the ghost/virtual swap layer

Status: design draft (for discussion on linux-mm)
Author: Jianyue Wu
Supersedes (approach): "mm/zram: route block swap I/O through swap_ops" and the
zswap->zram `REQ_COMPRESSED`/`REQ_NOCOMPRESS` passthrough series.

## 0. TL;DR

Move "write zswap's already-compressed bytes to the backing device without
decompressing" out of the block layer / zram and into the *core swap* code,
by making it a property of the ghost/virtual swap layer (Kairui Song's swap
table phase IV "ghost swapfile" + Nhat Pham's virtual swap space).

The compressed blob is produced and consumed only by zswap's own (a)comp
codec, so there is no cross-component on-disk format contract, no new bio
flag, and no reliance on a compressing ramdisk. This directly follows
Christoph Hellwig's position that "compression fundamentally belongs into the
core swap code, not a virtual block device" and "stop adding hacks to the
block layer".

## 1. Motivation

zswap today writes back by *decompressing* a compressed entry into a full
page and issuing a normal `__swap_writepage()`. When the backing device is a
compressing ramdisk (zram), that page is then *recompressed*. Even for a real
SSD, decompress-on-writeback costs CPU and writes a full uncompressed page.

The previously proposed fix added `REQ_COMPRESSED`/`REQ_NOCOMPRESS` bio flags
and taught zram to accept zswap's compressed blob verbatim. That approach was
rejected in direction: it deepens zswap<->zram coupling, adds block-layer
flags, and keeps compression inside a virtual block device.

The same *functional* win (skip decompress+recompress; write compressed bytes;
decompress on swapin) can be obtained entirely inside core swap once swap
entries are decoupled from physical backing. That decoupling is exactly what
the ghost/virtual swap work provides.

## 2. Background: what the three in-flight series give us

### 2.1 Swap table phase IV / dynamic ghost swapfile (Kairui Song)
- `struct swap_cluster_info_dynamic` (`ci_dyn`) extends `swap_cluster_info`
  with a per-slot `virtual_table` (tag-encoded `atomic_long_t`, 8 B/slot),
  allocated on demand via an xarray. Plain swap keeps its existing layout and
  pays zero extra overhead.
- `SWP_GHOST` marks a device whose clusters are dynamic; the cover letter
  states the `virtual_table` is "ready to be used ... for example, storing
  zswap's metadata", and quotes overhead tiers:
  - 8 B/slot: plain / NBD tier (bypass everything),
  - 16 B/slot: ghost + zswap only,
  - 24 B/slot: ghost + physical writeback (with reverse map).
- Physical clusters keep pointer-tagged rmap entries in the swap table for
  reverse lookup back to the virtual cluster.

### 2.2 Virtual swap space (Nhat Pham, "Swap Table Edition")
- A page gets a *virtual* swap entry on a ghost device with no backing.
- Backend states in the virtual table: `NONE`, `PHYS`, `ZERO`,
  `ZSWAP(entry*)`, `FOLIO(folio*)`.
- Physical backing is allocated *lazily*, only at writeback/reclaim time,
  *after* the rmap step. Backend transitions are in-place vtable updates: no
  PTE/rmap rewriting, so a `ZSWAP -> PHYS` transition is cheap and invisible
  to page tables.
- The cover letter already names the target: "we can even perform compressed
  writeback (writing these pages without decompressing them)".

### 2.3 Swap tiers (Youngjun Park)
- Groups swap devices into performance classes; per-memcg selection via
  `memory.swap.tiers.max`, config under `/sys/kernel/mm/swap/tiers`.
- The ghost/virtual file "can be just a tier". So "compressed writeback" can
  be scoped to a tier: workloads routed to a raw NBD tier take the 8 B/slot
  fast path; workloads on the ghost tier get compression.

Key consequence: with (2.1)+(2.2), a swapped page can live as
`ZSWAP(entry*)` and later become `PHYS(compressed blob)` by a single in-place
`virtual_table` update, with room to stash the metadata needed to read it
back. That is all compressed writeback needs.

## 3. Design

### 3.1 New backend state: `PHYS_COMPRESSED`

Add one virtual-table backend encoding alongside `PHYS`:

```
PHYS            -> raw page at a physical slot (existing)
PHYS_COMPRESSED -> compressed blob at a physical slot + codec metadata
```

Per-slot metadata required to read the blob back (the 24 B/slot "physical
writeback" tier):

```c
struct swp_compressed_backing {
    swp_entry_t     phys;      /* physical slot(s) holding the blob   */
    u16             clen;      /* compressed length in bytes          */
    u8              algo_id;   /* zswap pool codec id (see 3.5)        */
    u8              flags;     /* e.g. multi-slot, order for THP       */
};
```

This is stored/encoded in `ci_dyn->virtual_table` for the virtual slot. No
zswap pool object survives after writeback; only these bytes + the on-device
blob remain. The decompressor is zswap's, so there is *no* on-disk format
contract with any other component.

### 3.2 Writeback path (replaces decompress-to-page)

Current `zswap_writeback_entry()` (post-vswap) allocates a folio, decompresses
into it, and calls `__swap_writepage()`. New path, gated on the backing tier
supporting compressed writeback:

1. The virtual entry is `ZSWAP(entry)` with `0 < entry->length < PAGE_SIZE`
   (genuinely compressed) and the target tier advertises compressed writeback.
2. Allocate physical backing sized for `entry->length` (see 3.4). For the
   minimal RFC this is one physical slot; batching/packing is future work.
3. Issue a raw block write of the compressed bytes (rounded up to the device
   sector) via the normal swap bio path — a plain data write, *no* new bio
   flag, the bytes are opaque.
4. On write completion, atomically update the virtual slot:
   `ZSWAP(entry) -> PHYS_COMPRESSED{phys, clen=entry->length, algo_id}`.
5. Free the zswap pool object and account the physical slot to the memcg
   (vswap already charges physical slots lazily at writeback).

Incompressible / `length == PAGE_SIZE` entries: keep the existing behaviour —
copy the raw page and write it normally as `PHYS` (a compressing backing tier
may still store it compactly on its own, but that is the tier's business, not
ours).

Large folios / THP: first cut stores order-0 only; a compressed THP either
falls back to per-page or to decompress-and-write. Encodable in `flags`.

### 3.3 Swapin path

1. Fault resolves the virtual entry to `PHYS_COMPRESSED{phys, clen, algo_id}`.
2. Read `clen` bytes from `phys` into a scratch buffer (single bio, partial
   page read is fine).
3. Decompress with the zswap pool codec identified by `algo_id` into the
   destination folio. This uses zswap's existing `acomp_ctx` / crypto_acomp
   path — including hardware offload (e.g. QAT) if that pool is configured for
   it. Ownership stays in core swap.
4. Populate the folio, resolve the fault; free the physical slot per normal
   swapin-frees-slot semantics.

Because the same zswap codec compressed and decompresses, cross-format
compatibility (the sticking point of the zram passthrough) simply does not
arise.

### 3.4 Physical slot allocation & on-device layout

Three options, in increasing complexity; RFC lands (A), notes (B)/(C):

- (A) *Page-granular, one blob per slot* (minimal, mergeable). Each compressed
  blob consumes one physical slot but we write only `clen` bytes (sector
  rounded). Wins: no decompress+recompress CPU, reduced write bandwidth / flash
  wear. Slot accounting unchanged. Fits the existing cluster allocator.
- (B) *Sub-page packing*. Pack several blobs into one physical page;
  `virtual_table` stores {page slot, offset, clen}. Wins disk space too, but
  needs a packing allocator + compaction/GC (essentially zsmalloc-on-disk).
- (C) *Batched contiguous writeback*. Leverage vswap's planned batching: gather
  N compressed entries, pack into `ceil(sum/PAGE_SIZE)` contiguous physical
  pages, one bio. (C) is (B) applied to a writeback batch and is the natural
  end state.

### 3.5 Codec identity (`algo_id`)

`algo_id` indexes the zswap pool codec used to compress the entry (zswap
already tracks `pool->tfm_name`). Constraints:
- The codec must remain registered for the lifetime of any on-device blob.
  Enforce by refcounting pools that have live `PHYS_COMPRESSED` slots, and
  rejecting/decompressing-out entries on pool teardown.
- Decompression need not run on the *same* implementation that compressed
  (e.g. compress on QAT, decompress in software) as long as the format matches
  the named algorithm — this is already true within a single zswap pool.

### 3.6 Fallback / safety

If any precondition fails (metadata can't be stored, tier does not support
compressed writeback, THP not handled, allocation failure), fall back to the
existing decompress-into-folio + `__swap_writepage()` path. This is the
behaviour restored by the "restore decompress-to-backing fallback" change and
remains the correctness backstop.

### 3.7 swapoff / migration / compaction

- swapoff of a ghost/physical device: for each `PHYS_COMPRESSED` slot, read +
  decompress into a folio and re-insert per the standard swapoff loop (the
  ghost reverse-map already locates the virtual slot).
- Future: a `PHYS_COMPRESSED -> ZSWAP` promotion (read blob back into the pool
  without full decompress) enables cheap tier migration; out of scope for RFC.

### 3.8 Config / tier gating

- Gated by `CONFIG_VSWAP` (ghost/virtual layer) — zero impact when off.
- Exposed as a per-tier capability, selected via `memory.swap.tiers.max`, so
  operators opt specific cgroups into compressed writeback while others take
  the 8 B/slot raw fast path.

## 4. Why this satisfies the review direction

- Compression lives in core swap (zswap codec + ghost virtual_table), not in a
  virtual block device.
- No new block-layer flags; the backing write is opaque data through the
  normal swap bio path.
- No zswap<->zram coupling; zram is not involved at all.
- Rides the agreed-upon infrastructure (swap table P4, vswap, swap tiers)
  instead of competing with it.

## 5. Comparison to the zram passthrough approach

| Aspect | zram passthrough (`REQ_COMPRESSED`) | ghost compressed writeback |
|---|---|---|
| Where compression lives | zram (virtual block device) | core swap (zswap codec) |
| Block-layer changes | new `REQ_COMPRESSED`/`REQ_NOCOMPRESS` | none |
| Cross-component format contract | required (zswap fmt == zram fmt) | none (same codec both ends) |
| Backend | zram only | any physical/ghost tier |
| Decompressor on swapin | zram | zswap (incl. QAT offload) |
| Alignment with merged direction | against | with |

## 6. Incremental patch plan (proposed)

1. Add `PHYS_COMPRESSED` backend encoding + `swp_compressed_backing` in the
   virtual table; helpers to get/set/erase.
2. Writeback: emit compressed blob + record metadata (option A), with fallback
   to decompress-to-folio.
3. Swapin: read blob + decompress via zswap codec.
4. Pool codec refcount for live `PHYS_COMPRESSED` slots; teardown handling.
5. swapoff handling for `PHYS_COMPRESSED`.
6. Tier capability flag + `memory.swap.tiers.max` gating.
7. Selftests: force zswap writeback to a ghost-backed SSD tier; verify no
   decompress on writeback, correct data on swapin, swapoff correctness.
8. (Later) batched contiguous writeback / sub-page packing (options B/C).

## 7. Open questions for the list

- Should `PHYS_COMPRESSED` be a first-class vtable state or a flag bit on
  `PHYS`? (encoding budget in the 8 B tag vs. the 24 B physical-writeback tier)
- Minimum viable metadata: is `algo_id` per-slot needed, or can the tier /
  device pin a single codec for all its compressed slots (cheaper encoding)?
- Partial-page (sector-granular) swap writes: acceptable on all backing types,
  or gate to block devices with sane sector semantics?
- Interaction with zswap's exclusive-load: after compressed writeback the pool
  object is gone; confirm the invalidation/reuse story matches vswap's.
```

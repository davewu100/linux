# RFC design: compressed-blob passthrough on top of core-swap shared zcomp

Status: design draft (for discussion)
Author: Jianyue Wu
Base: `christoph-core-compress_v0.0.2`
  (`lib/zcomp` shared compressor + `mm/swap_compress.c` core helpers)

## 0. TL;DR

The base branch already moved the software compressor into the core swap layer
(`lib/zcomp` + `mm/swap_compress.c`) so that zswap and zram compress and
decompress through the *same* implementation. What it does **not** yet do is
avoid the decompress-on-writeback / recompress-on-store cycle: on writeback
`zswap_writeback_entry()` still calls `zswap_decompress()` into a full page and
`__swap_writepage()` writes that page to zram, which then calls
`swap_compress()` again.

This draft adds **compressed-blob passthrough**: on writeback zswap hands its
already-compressed blob to core swap, which stores it into zram verbatim as a
compressed slot, skipping both the decompress and the recompress. The blob
carries the identity of the codec that produced it (a stable lib/zcomp backend
id), so the two tiers do **not** have to use the same algorithm; zram decodes
each passthrough slot with the codec the id names. This requires:

- **no new `REQ_COMPRESSED` bio flag**,
- **no `->swap_comp_algo()` negotiation / format contract**,
- **no `bi_iter.bi_size` length hack**.

It is the "same functional win" as the old zram `REQ_COMPRESSED` series, but
obtained on the compliant core-swap foundation instead of a block-layer flag.

As a small extension in the same spirit, a page zswap already found
**incompressible** (`entry->length == PAGE_SIZE`) is passed through as a whole
page too, so zram stores it verbatim (a `ZRAM_HUGE` slot) instead of retrying —
and almost certainly failing — the compression zswap already gave up on.

## 1. Why this is now clean

Two facts from the base branch remove everything that made the old passthrough
non-upstreamable:

1. **Shared codec library, id-tagged blobs.** zswap's software algorithms and
   zram's primary slot both run on `lib/zcomp`. A blob is decodable by any tier
   as long as it is decoded with the *same backend* that produced it. Rather
   than force both tiers onto one global algorithm, each passthrough blob is
   tagged with the producing backend's **stable id** (its index in lib/zcomp's
   `backends[]`, see `zcomp_lookup_backend_id()`). `mm/swap_compress.c` keeps a
   small id-indexed cache of zcomp instances and decodes a blob with
   `swap_decompress_by_id()`. zswap and zram may therefore use different
   algorithms; there is still no cross-component format contract to negotiate or
   version, only a one-integer codec id travelling with the blob.

2. **zram already stores the compressed length.** `zram_write_page()` stores
   `comp_len` via `set_slot_size()`, and `read_compressed_page()` reads it back
   with `get_slot_size()` before calling `swap_decompress()`. So the *read*
   path needs nothing new: a passthrough blob stored as an ordinary compressed
   slot is read back exactly like any other zram compressed slot.

The only remaining problem is the **write** direction: how does zswap tell zram
"this folio already holds `comp_len` bytes of a shared-codec blob, store it
as-is" without a bio flag and without abusing `bi_iter.bi_size`?

## 2. comp_len transport (the one real design question)

### 2.1 Rejected options

- **bio flag (`REQ_COMPRESSED`) + `bi_iter.bi_size` length** — this is exactly
  the v0.0.8 approach the base branch was built to avoid. Rejected.
- **New `block_device_operations` callback** — reintroduces block-layer
  coupling. Rejected.

### 2.2 Chosen: a per-task core-swap precompressed write descriptor

Add a small, swap-owned length descriptor that travels with the write request
through the core swap layer, not through the block layer. The backing driver
consults it via a core swap helper, so it is not a bio property. The descriptor
carries **only** the compressed length; the algorithm is implied by the shared
codec, so no id/version is needed.

Transport mechanism (in preference order; RFC lands (A)):

- **(A) Per-task current-write descriptor, scoped to the sync write.**
  The length lives on the writing task as `current->swap_precompressed_len`
  (0 when no passthrough is in flight). `swap_writepage_precompressed(folio,
  comp_len)` is a new core-swap write helper (sibling of `__swap_writepage()`),
  used only on the synchronous bdev path. It:
    1. records `comp_len` on the task via `swap_precompressed_write_begin()`,
    2. issues a normal **full-folio, block-aligned** synchronous bio (the folio
       is zero-padded past the blob; a bio sized to `comp_len` would be rejected
       for a non-block-aligned length), a plain data write with no new flag,
    3. clears the descriptor via `swap_precompressed_write_end()` on completion.
  zram's write path calls `swap_precompressed_write_len()` to learn whether the
  in-flight write is a passthrough and its length, and stores only that many
  bytes. Because the backing store runs `->submit_bio` synchronously **in the
  same task**, the descriptor is unambiguous for the duration of the bio. The
  helper refuses the passthrough when `current->bio_list` is non-NULL (a nested
  submission, where the block layer would defer the bio and another bio could
  observe the descriptor). This keeps the length transport entirely inside core
  swap, never in `struct bio`.

  *Trade-off*: relies on the synchronous, order-0 write path (same PoC scope as
  before). Async/plugged writeback would need (B).

- **(B) swap-entry–anchored descriptor (future, async-capable).**
  Store `{comp_len}` keyed by the target `swp_entry_t` in a swap-side xarray
  populated before submit and consumed by the backing store. Survives async
  completion and multiple in-flight writes; heavier. This is the natural
  home once the write path is generalized beyond sync bdev, and mirrors where
  a virtual-swap layer would keep per-slot descriptors.

The descriptor carries `comp_len` **and** the producing codec's backend id
(`current->swap_precompressed_len` / `_alg_id`). It carries no format version:
the id names a lib/zcomp backend whose on-disk format is fixed for a kernel
image, so the id alone is enough to decode the blob. The id is what lets the two
tiers use different algorithms.

## 3. Write path (zswap → core swap → zram)

`zswap_writeback_entry()` gains a passthrough branch taken, for an order-0 folio
on the synchronous bdev path, in one of two cases:

- **Compressed blob** (`0 < entry->length < PAGE_SIZE`) from a lib/zcomp backend
  (`ZSWAP_BACKEND_ZCOMP`), so its algorithm has a stable backend id.  There is
  **no** requirement that the backing zram's primary codec match the zswap
  codec: the blob is tagged with its own codec id.
- **Incompressible page** (`entry->length == PAGE_SIZE`): the entry already holds
  a raw page zswap could not compress.  It is handed over as a whole page so zram
  stores it verbatim instead of pointlessly trying to compress it again.  No
  codec is involved, so this case is independent of the backend.

Steps (compressed blob):

1. Copy `entry->length` blob bytes to folio offset 0 (zero-padded); no
   `swap_decompress()`.  Resolve the producing codec id with
   `zcomp_lookup_backend_id(pool->tfm_name)`.
2. `swap_writepage_precompressed(folio, entry->length, alg_id)` records the
   length and codec id in the core-swap descriptor and submits the sync write.
3. zram's `zram_bvec_write()` asks core swap for the precompressed length and
   id; if present and `comp_len < PAGE_SIZE` it calls
   `zram_store_precompressed(page, index, comp_len, alg_id)` which:
     - `zs_malloc(comp_len)` + `zs_obj_write(handle, blob, comp_len)`,
     - `set_slot_size(index, comp_len)`, `set_slot_comp_priority(index,
       ZRAM_PRIMARY_COMP)`,
     - `set_slot_flag(index, ZRAM_PRECOMP)` and `set_slot_precomp_alg(index,
       alg_id)` to record that this slot is an external blob and which codec
       produced it.
   One new `ZRAM_PRECOMP` flag plus a 3-bit codec id, so the read path can pick
   the right decoder.
4. On success zswap frees the entry and drops the folio from the swap cache
   (it holds compressed bytes, not a page), so a later swapin re-reads and
   decompresses it from zram.

Steps (incompressible page):

The full page is copied to the folio and `swap_writepage_precompressed(folio,
PAGE_SIZE, 0)` marks the descriptor length as `PAGE_SIZE`.  zram sees
`comp_len == PAGE_SIZE` and stores the page through the existing
`write_incompressible_page()` path — a normal `ZRAM_HUGE` slot, read back with a
plain `copy_page()` and no decompression.  This avoids the compression attempt
zram would otherwise make on a page zswap already knows is incompressible.

## 4. Read path (swapin)

A passthrough blob is a `ZRAM_PRIMARY_COMP` slot with a correct `slot_size` and
the `ZRAM_PRECOMP` flag. `read_compressed_page()` (and `decompress_bdev_page()`
for the compressed-writeback tier) detect `ZRAM_PRECOMP`, read `get_slot_size()`
bytes, and call `swap_decompress_by_id(get_slot_precomp_alg(...), ...)` — i.e.
decode with the codec that produced the blob, not zram's own primary codec.
Non-passthrough primary slots keep using `swap_decompress()` as before.

An incompressible passthrough page is stored as an ordinary `ZRAM_HUGE` slot, so
it needs no special read handling at all: `read_incompressible_page()` returns it
with a plain `copy_page()`, exactly like any other huge/incompressible zram
slot.

## 5. swapoff / fallback / safety

- **Fallback**: if any precondition fails (not compressed, large folio, async
  path, allocation failure, no stable backend id), take the existing path —
  `swap_decompress()` into the folio + `__swap_writepage()`. Correctness
  backstop unchanged.
- **swapoff**: passthrough slots are normal zram compressed slots (with the
  `ZRAM_PRECOMP` tag), so the standard swapoff read loop decompresses them via
  `read_compressed_page()`. Nothing special.
- **Primary-codec change while blobs live on disk**: no longer a concern for
  passthrough. Each blob records its own codec id and is decoded through the
  id-indexed cache, so changing zram's primary codec
  (`swap_compress_set_algorithm()`) does not affect stored passthrough blobs.
  The id→format mapping is stable for a kernel image, so no drain/gate is
  required.

## 6. Why this satisfies the review direction

- Compression stays in core swap (`lib/zcomp` + `mm/swap_compress.c`); zram only
  stores an opaque blob plus the codec id needed to read it back.
- No block-layer flags; the write is plain data, length and codec id ride a
  core-swap descriptor, not `struct bio`.
- No zswap↔zram format *contract* to negotiate or version: byte compatibility is
  guaranteed by decoding each blob with the exact lib/zcomp backend that produced
  it. Only `comp_len` and a one-integer codec id are transported, and the two
  tiers may use different algorithms.

## 7. Comparison

| Aspect | v0.0.8 (`REQ_COMPRESSED`) | v0.0.11 (contract layer) | this draft (on base branch) |
|---|---|---|---|
| Block-layer flag | new `REQ_COMPRESSED` | new `REQ_COMPRESSED` | none |
| Length transport | `bi_iter.bi_size` hack | `bi_iter.bi_size` hack | core-swap descriptor |
| Format contract | implicit name match | explicit + versioned | codec id per blob (no negotiation) |
| Heterogeneous zswap/zram codecs | no | no | yes |
| Extra zram slot flag | `ZRAM_NOCOMP` | `ZRAM_NOCOMP` | `ZRAM_PRECOMP` + 3-bit codec id |
| Read path change | via zram | via zram | decode by codec id |
| Avoids 2nd compression | yes | yes | yes |
| Compliant w/ Christoph | no | no | yes |

## 8. Incremental patch plan

1. `lib/zcomp`: expose `zcomp_lookup_backend_id()` / `zcomp_backend_name_by_id()`
   (stable backend index as a codec id).
2. `mm/swap_compress.{c,h}` + `include/linux/sched.h`: per-task descriptor
   carrying `{comp_len, alg_id}`; id-indexed zcomp cache with
   `swap_decompress_by_id()`; `swap_compress_alg_id()`.
3. `mm/page_io.c`: `swap_writepage_precompressed(folio, comp_len, alg_id)`.
4. `drivers/block/zram`: `ZRAM_PRECOMP` flag + 3-bit codec id in the slot;
   `zram_store_precompressed()` stamps them; `read_compressed_page()` and
   `decompress_bdev_page()` decode via `swap_decompress_by_id()`; `slot_free()`
   clears the tag.
5. `mm/zswap.c`: passthrough branch in `zswap_writeback_entry()` resolves the
   codec id from `pool->tfm_name`; keep decompress fallback.
6. Docs + limitations (order-0, sync bdev only for RFC; async via descriptor
   option (B) as follow-up).

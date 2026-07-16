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
compressed slot, skipping both the decompress and the recompress. Because both
tiers now share one codec (base branch), this requires:

- **no new `REQ_COMPRESSED` bio flag**,
- **no `->swap_comp_algo()` negotiation / format contract**,
- **no `bi_iter.bi_size` length hack**.

It is the "same functional win" as the old zram `REQ_COMPRESSED` series, but
obtained on the compliant core-swap foundation instead of a block-layer flag.

## 1. Why this is now clean

Two facts from the base branch remove everything that made the old passthrough
non-upstreamable:

1. **One codec, both tiers.** `swap_compress()`/`swap_decompress()` in
   `mm/swap_compress.c` are the single compressor for zswap's software
   algorithms and for zram's primary slot (`ZRAM_PRIMARY_COMP`). A blob
   produced by `swap_compress()` is by construction decodable by
   `swap_decompress()`. There is no cross-component format contract to
   negotiate or version.

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

The descriptor deliberately carries **only** `comp_len`. No algorithm id and no
format version, because the shared codec makes them redundant — the key
simplification the base branch buys us.

## 3. Write path (zswap → core swap → zram)

`zswap_writeback_entry()` gains a passthrough branch taken when:

- the entry is genuinely compressed (`0 < entry->length < PAGE_SIZE`),
- the folio is order-0,
- the backing device stores primary slots through the shared codec
  (i.e. `CONFIG_SWAP_COMPRESS` backing == zram primary), and
- the write is on the synchronous bdev path.

Steps:

1. `swap_copy_blob_to_folio(entry, folio)` copies `entry->length` blob bytes to
   folio offset 0 (zero-padded); no `swap_decompress()`.
2. `swap_writepage_precompressed(folio, entry->length)` records the length in
   the core-swap descriptor and submits the sync write.
3. zram's `zram_bvec_write()` asks core swap for the precompressed length; if
   present it calls `zram_store_precompressed(page, index, comp_len)` which:
     - `zs_malloc(comp_len)` + `zs_obj_write(handle, blob, comp_len)`,
     - `set_slot_size(index, comp_len)`, `set_slot_comp_priority(index,
       ZRAM_PRIMARY_COMP)`,
   i.e. a normal compressed slot — **no** `ZRAM_HUGE`, **no** extra slot flag,
   because the codec matches (unlike v0.0.8 which needed `ZRAM_NOCOMP`).
4. On success zswap frees the entry and drops the folio from the swap cache
   (it holds compressed bytes, not a page), so a later swapin re-reads and
   decompresses it from zram.

`length == PAGE_SIZE` entries (stored raw) are copied out and written normally,
letting zram compress them with the shared codec — same as today.

## 4. Read path (swapin)

**No change.** A passthrough blob is an ordinary `ZRAM_PRIMARY_COMP` slot with a
correct `slot_size`. `read_compressed_page()` reads `get_slot_size()` bytes and
calls `swap_decompress()` — the existing shared-codec read path. This is the
biggest simplification versus every prior approach: the read side is already
done by the base branch.

## 5. swapoff / fallback / safety

- **Fallback**: if any precondition fails (not compressed, large folio, async
  path, allocation failure, backing not shared-codec), take the existing path —
  `swap_decompress()` into the folio + `__swap_writepage()`. Correctness
  backstop unchanged.
- **swapoff**: passthrough slots are normal zram compressed slots, so the
  standard swapoff read loop decompresses them via `read_compressed_page()`.
  Nothing special.
- **Codec change while blobs live on disk**: because the codec is the single
  core-swap compressor, changing it (`swap_compress_set_algorithm()`) must be
  gated the same way zram already gates a live pool — reject or drain while
  passthrough slots exist. This is a pre-existing concern of the shared codec,
  not new to passthrough, but passthrough makes it load-bearing and must be
  documented/enforced.

## 6. Why this satisfies the review direction

- Compression stays in core swap (`lib/zcomp` + `mm/swap_compress.c`); zram only
  stores an opaque blob it can already read back.
- No block-layer flags; the write is plain data, length rides a core-swap
  descriptor, not `struct bio`.
- No zswap↔zram format contract; one shared codec makes byte compatibility a
  fact, not an assumption. `comp_len` is the only thing transported.

## 7. Comparison

| Aspect | v0.0.8 (`REQ_COMPRESSED`) | v0.0.11 (contract layer) | this draft (on base branch) |
|---|---|---|---|
| Block-layer flag | new `REQ_COMPRESSED` | new `REQ_COMPRESSED` | none |
| Length transport | `bi_iter.bi_size` hack | `bi_iter.bi_size` hack | core-swap descriptor |
| Format contract | implicit name match | explicit + versioned | none (shared codec) |
| Extra zram slot flag | `ZRAM_NOCOMP` | `ZRAM_NOCOMP` | none |
| Read path change | via zram | via zram | none (base branch) |
| Avoids 2nd compression | yes | yes | yes |
| Compliant w/ Christoph | no | no | yes |

## 8. Incremental patch plan

1. `mm/swap_compress.{c,h}`: add `struct swap_precompressed`, per-CPU
   descriptor, `swap_writepage_precompressed()`, `swap_read_precompressed_len()`,
   `swap_copy_blob_to_folio()`.
2. `drivers/block/zram/zram_drv.c`: `zram_store_precompressed()`; in
   `zram_bvec_write()` consult the descriptor and route to it.
3. `mm/zswap.c`: passthrough branch in `zswap_writeback_entry()` +
   preconditions; keep decompress fallback.
4. Enforce codec-change safety while passthrough slots exist.
5. Docs + limitations (order-0, sync bdev only for RFC; async via descriptor
   option (B) as follow-up).

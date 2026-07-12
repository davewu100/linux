/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _MM_SWAP_COMPRESSED_H
#define _MM_SWAP_COMPRESSED_H

#include <linux/errno.h>
#include <linux/mm_types.h>	/* swp_entry_t */
#include <linux/types.h>

struct folio;

/*
 * Compressed writeback: record that a genuinely compressed zswap blob was
 * written to a physical swap slot verbatim (without decompression), so a later
 * swapin can read the blob back and decompress it with zswap's own codec.
 *
 * This is the core-swap replacement for the zswap->zram REQ_COMPRESSED
 * passthrough: the compressed bytes are produced and consumed only by zswap,
 * the backing device stores opaque data, and no block-layer flag is involved.
 *
 * Storage model
 * -------------
 * Each compressed physical slot needs a small descriptor {clen, algo_id,
 * flags}.  In the ghost/virtual swap design (swap table phase IV + virtual
 * swap space) this descriptor is the backend payload of a virtual slot in a
 * PHYS_COMPRESSED state, tag-encoded into ci_dyn->virtual_table (~8 bytes per
 * slot).  That infrastructure is not present in this tree yet, so the
 * descriptor is kept here in a standalone tag-encoded store keyed by the
 * physical swp_entry_t.  The value encoding mirrors the intended virtual_table
 * layout, so switching the backing store later is a self-contained change that
 * does not touch callers.
 */

/* Descriptor bit budget, packed into one tagged xarray value (see .c). */
#define SWP_COMP_FLAG_BITS	8	/* SWP_COMP_F_* */
#define SWP_COMP_ALGO_BITS	8	/* zswap pool codec id */
#define SWP_COMP_CLEN_BITS	16	/* compressed length in bytes */

#define SWP_COMP_ALGO_MAX	((1u << SWP_COMP_ALGO_BITS) - 1)
#define SWP_COMP_CLEN_MAX	((1u << SWP_COMP_CLEN_BITS) - 1)

/* desc->flags */
#define SWP_COMP_F_NONE		0x0u

/**
 * struct swp_compressed_desc - metadata for a compressed-writeback slot
 * @clen:    compressed length in bytes (0 < clen < PAGE_SIZE)
 * @algo_id: zswap pool codec id used to compress, and to decompress on swapin
 * @flags:   SWP_COMP_F_* bits
 */
struct swp_compressed_desc {
	u32 clen;
	u16 algo_id;
	u16 flags;
};

#ifdef CONFIG_SWAP_COMPRESSED_WRITEBACK

/* Whether compressed writeback may be used (per-tier gating lands later). */
bool swap_compressed_writeback_enabled(void);

/*
 * Intern a zswap codec name and return a stable small id in [0, algo max], or
 * a negative errno.  The id is what a descriptor stores; the name is resolved
 * back on swapin to pick the decompressor.
 */
int swap_compressed_algo_id(const char *tfm_name);

/* Resolve an id previously returned by swap_compressed_algo_id(). */
const char *swap_compressed_algo_name(u16 algo_id);

/* Record @desc for the physical slot @phys.  Returns 0 or a negative errno. */
int swap_compressed_record(swp_entry_t phys,
			   const struct swp_compressed_desc *desc);

/* Look up @phys; fills @desc and returns true if a descriptor is recorded. */
bool swap_compressed_lookup(swp_entry_t phys,
			    struct swp_compressed_desc *desc);

/* Drop any descriptor recorded for @phys. */
void swap_compressed_erase(swp_entry_t phys);

/*
 * Decompress @folio in place: it must currently hold @desc's blob in its first
 * @desc->clen bytes.  Uses the codec named by @desc->algo_id.  Returns true on
 * success (folio now holds the raw page), false on error.
 */
bool swap_compressed_decompress_folio(struct folio *folio,
				      const struct swp_compressed_desc *desc);

#else /* !CONFIG_SWAP_COMPRESSED_WRITEBACK */

static inline bool swap_compressed_writeback_enabled(void)
{
	return false;
}

static inline int swap_compressed_algo_id(const char *tfm_name)
{
	return -EOPNOTSUPP;
}

static inline const char *swap_compressed_algo_name(u16 algo_id)
{
	return NULL;
}

static inline int swap_compressed_record(swp_entry_t phys,
					 const struct swp_compressed_desc *desc)
{
	return -EOPNOTSUPP;
}

static inline bool swap_compressed_lookup(swp_entry_t phys,
					  struct swp_compressed_desc *desc)
{
	return false;
}

static inline void swap_compressed_erase(swp_entry_t phys)
{
}

static inline bool swap_compressed_decompress_folio(struct folio *folio,
					const struct swp_compressed_desc *desc)
{
	return false;
}

#endif /* CONFIG_SWAP_COMPRESSED_WRITEBACK */

#endif /* _MM_SWAP_COMPRESSED_H */

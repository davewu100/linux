// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Compressed-writeback descriptor store.
 *
 * Skeleton for storing the per-slot metadata needed to write a zswap
 * compressed blob to a physical swap slot without decompressing it, and to
 * read+decompress it on swapin using zswap's own codec.  See
 * mm/swap_compressed.h for the design and the ghost/virtual swap mapping.
 *
 * The backing store here is a standalone xarray keyed by the physical
 * swp_entry_t value.  It is a stand-in for ghost swap's
 * ci_dyn->virtual_table (swap table phase IV); the callers and the value
 * encoding are shaped so that swapping the store for virtual_table access is
 * a self-contained change.
 */

#include <linux/build_bug.h>
#include <linux/gfp.h>
#include <linux/swap.h>
#include <linux/xarray.h>

#include "swap_compressed.h"

static DEFINE_XARRAY(swap_compressed_store);

#define SWP_COMP_FLAG_MASK	((1u << SWP_COMP_FLAG_BITS) - 1)
#define SWP_COMP_ALGO_SHIFT	SWP_COMP_FLAG_BITS
#define SWP_COMP_CLEN_SHIFT	(SWP_COMP_FLAG_BITS + SWP_COMP_ALGO_BITS)

static unsigned long swp_comp_encode(const struct swp_compressed_desc *desc)
{
	unsigned long v;

	/* Must fit in a tagged xarray value (xa_mk_value reserves one bit). */
	BUILD_BUG_ON(SWP_COMP_FLAG_BITS + SWP_COMP_ALGO_BITS +
		     SWP_COMP_CLEN_BITS > BITS_PER_LONG - 1);

	v = desc->flags & SWP_COMP_FLAG_MASK;
	v |= (unsigned long)(desc->algo_id & SWP_COMP_ALGO_MAX) << SWP_COMP_ALGO_SHIFT;
	v |= (unsigned long)(desc->clen & SWP_COMP_CLEN_MAX) << SWP_COMP_CLEN_SHIFT;
	return v;
}

static void swp_comp_decode(unsigned long v, struct swp_compressed_desc *desc)
{
	desc->flags = v & SWP_COMP_FLAG_MASK;
	desc->algo_id = (v >> SWP_COMP_ALGO_SHIFT) & SWP_COMP_ALGO_MAX;
	desc->clen = (v >> SWP_COMP_CLEN_SHIFT) & SWP_COMP_CLEN_MAX;
}

bool swap_compressed_writeback_enabled(void)
{
	/*
	 * Placeholder: once compressed writeback binds to a ghost/virtual swap
	 * tier, this becomes a per-tier (and per-memcg, via swap tiers)
	 * capability check.  Nothing wires the record/lookup path yet, so the
	 * store stays empty regardless.
	 */
	return true;
}

int swap_compressed_record(swp_entry_t phys,
			   const struct swp_compressed_desc *desc)
{
	void *old;

	if (!desc->clen || desc->clen > SWP_COMP_CLEN_MAX)
		return -EINVAL;
	if (desc->algo_id > SWP_COMP_ALGO_MAX)
		return -EINVAL;
	if (desc->flags & ~SWP_COMP_FLAG_MASK)
		return -EINVAL;

	old = xa_store(&swap_compressed_store, phys.val,
		       xa_mk_value(swp_comp_encode(desc)), GFP_KERNEL);
	return xa_err(old);
}

bool swap_compressed_lookup(swp_entry_t phys, struct swp_compressed_desc *desc)
{
	void *entry = xa_load(&swap_compressed_store, phys.val);

	if (!xa_is_value(entry))
		return false;

	swp_comp_decode(xa_to_value(entry), desc);
	return true;
}

void swap_compressed_erase(swp_entry_t phys)
{
	xa_erase(&swap_compressed_store, phys.val);
}

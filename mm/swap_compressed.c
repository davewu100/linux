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
#include <linux/cleanup.h>
#include <linux/gfp.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/swap.h>
#include <linux/xarray.h>

#include <crypto/acompress.h>

#include "swap_compressed.h"

static DEFINE_XARRAY(swap_compressed_store);

/*
 * Codec name registry.  A descriptor only has room for a small id, so codec
 * names are interned here and resolved back on swapin.  Entries are never
 * removed (the set of active compressors is tiny and bounded by the id space),
 * so a resolved name is valid for the lifetime of the kernel without a lock.
 */
static DEFINE_MUTEX(swap_compressed_algo_lock);
static const char *swap_compressed_algos[SWP_COMP_ALGO_MAX + 1];
static unsigned int swap_compressed_nr_algos;

int swap_compressed_algo_id(const char *tfm_name)
{
	unsigned int i;
	char *dup;
	int ret;

	guard(mutex)(&swap_compressed_algo_lock);

	for (i = 0; i < swap_compressed_nr_algos; i++)
		if (!strcmp(swap_compressed_algos[i], tfm_name))
			return i;

	if (swap_compressed_nr_algos > SWP_COMP_ALGO_MAX)
		return -ENOSPC;

	dup = kstrdup(tfm_name, GFP_KERNEL);
	if (!dup)
		return -ENOMEM;

	ret = swap_compressed_nr_algos;
	swap_compressed_algos[swap_compressed_nr_algos++] = dup;
	return ret;
}

const char *swap_compressed_algo_name(u16 algo_id)
{
	guard(mutex)(&swap_compressed_algo_lock);

	if (algo_id < swap_compressed_nr_algos)
		return swap_compressed_algos[algo_id];
	return NULL;
}

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

bool swap_compressed_decompress_folio(struct folio *folio,
				      const struct swp_compressed_desc *desc)
{
	const char *name = swap_compressed_algo_name(desc->algo_id);
	struct crypto_acomp *acomp;
	struct acomp_req *req;
	struct scatterlist ssg, dsg;
	struct crypto_wait wait;
	struct page *scratch;
	bool ok = false;
	int ret;

	if (!name || !desc->clen || desc->clen >= PAGE_SIZE)
		return false;

	/*
	 * Decompress into a scratch page and copy back, since @folio is both
	 * the compressed source and the raw destination.  Allocating a
	 * transient acomp per swapin is deliberately simple for the prototype;
	 * a real implementation would reuse a per-CPU codec context.
	 */
	scratch = alloc_page(GFP_KERNEL);
	if (!scratch)
		return false;

	acomp = crypto_alloc_acomp(name, 0, 0);
	if (IS_ERR(acomp))
		goto out_page;

	req = acomp_request_alloc(acomp);
	if (!req)
		goto out_acomp;

	crypto_init_wait(&wait);
	acomp_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				   crypto_req_done, &wait);

	sg_init_table(&ssg, 1);
	sg_set_folio(&ssg, folio, desc->clen, 0);
	sg_init_table(&dsg, 1);
	sg_set_page(&dsg, scratch, PAGE_SIZE, 0);
	acomp_request_set_params(req, &ssg, &dsg, desc->clen, PAGE_SIZE);

	ret = crypto_wait_req(crypto_acomp_decompress(req), &wait);
	if (!ret && req->dlen == PAGE_SIZE) {
		void *s = kmap_local_page(scratch);

		memcpy_to_folio(folio, 0, s, PAGE_SIZE);
		kunmap_local(s);
		flush_dcache_folio(folio);
		ok = true;
	}

	acomp_request_free(req);
out_acomp:
	crypto_free_acomp(acomp);
out_page:
	__free_page(scratch);
	return ok;
}

// SPDX-License-Identifier: GPL-2.0
/*
 * Virtual swap layer: route swap I/O through tiers.
 *
 *   write: zswap (RAM cache) → backend_ops (compressed blob) → block/fs
 *   read:  zswap → backend_ops → block/fs
 *
 * Compression policy lives in mm core; backends such as zram only store
 * opaque blobs.
 */
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/swap_compress.h>
#include <linux/zswap.h>

#include "swap.h"

bool swap_virtual_try_store(struct folio *folio)
{
#ifdef CONFIG_ZSWAP
	if (zswap_store(folio)) {
		count_mthp_stat(folio_order(folio), MTHP_STAT_ZSWPOUT);
		return true;
	}
#endif
	return false;
}

#if IS_ENABLED(CONFIG_SWAP_COMPRESS)
bool swap_virtual_backend_write(struct folio *folio,
				struct swap_info_struct *sis)
{
	struct zcomp_strm *zstrm;
	unsigned long offset = swp_offset(folio->swap);
	unsigned int comp_len;
	void *mem, *buf;
	int ret;

	if (!sis->backend_ops || !sis->backend_ops->store)
		return false;

	zstrm = swap_compress_stream_get();
	if (!zstrm)
		return false;

	buf = swap_compress_buffer(zstrm);
	mem = kmap_local_folio(folio, 0);
	ret = swap_compress(zstrm, mem, &comp_len);
	if (!ret) {
		if (comp_len >= PAGE_SIZE)
			ret = sis->backend_ops->store(sis, offset, mem, PAGE_SIZE);
		else
			ret = sis->backend_ops->store(sis, offset, buf, comp_len);
	}
	kunmap_local(mem);
	swap_compress_stream_put(zstrm);

	if (ret)
		return false;

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	if (unlikely(folio_test_pmd_mappable(folio))) {
		count_memcg_folio_events(folio, THP_SWPOUT, 1);
		count_vm_event(THP_SWPOUT);
	}
#endif
	count_mthp_stat(folio_order(folio), MTHP_STAT_SWPOUT);
	count_memcg_folio_events(folio, PSWPOUT, folio_nr_pages(folio));
	count_vm_events(PSWPOUT, folio_nr_pages(folio));
	folio_start_writeback(folio);
	folio_unlock(folio);
	return true;
}

bool swap_virtual_backend_read(struct folio *folio,
			       struct swap_info_struct *sis)
{
	struct zcomp_strm *zstrm;
	unsigned long offset = swp_offset(folio->swap);
	unsigned int comp_len;
	void *mem, *buf;
	int ret;

	if (!sis->backend_ops || !sis->backend_ops->load)
		return false;

	zstrm = swap_compress_stream_get();
	if (!zstrm)
		return false;

	buf = swap_compress_buffer(zstrm);
	ret = sis->backend_ops->load(sis, offset, buf, &comp_len);
	if (ret)
		goto out;

	mem = kmap_local_folio(folio, 0);
	if (comp_len >= PAGE_SIZE)
		memcpy(mem, buf, PAGE_SIZE);
	else
		ret = swap_decompress(zstrm, buf, comp_len, mem);
	kunmap_local(mem);

	if (!ret) {
		count_mthp_stat(folio_order(folio), MTHP_STAT_SWPIN);
		count_memcg_folio_events(folio, PSWPIN, folio_nr_pages(folio));
		count_vm_events(PSWPIN, folio_nr_pages(folio));
		folio_mark_uptodate(folio);
		folio_unlock(folio);
	}
out:
	swap_compress_stream_put(zstrm);
	return !ret;
}
#else
bool swap_virtual_backend_write(struct folio *folio,
				struct swap_info_struct *sis)
{
	return false;
}

bool swap_virtual_backend_read(struct folio *folio,
			       struct swap_info_struct *sis)
{
	return false;
}
#endif

bool swap_virtual_try_read(struct folio *folio, struct swap_info_struct *sis)
{
#ifdef CONFIG_ZSWAP
	if (zswap_load(folio) != -ENOENT)
		return true;
#endif
	return swap_virtual_backend_read(folio, sis);
}

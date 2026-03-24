/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM shmem

#if !defined(_TRACE_SHMEM_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_SHMEM_H

#include <linux/tracepoint.h>
#include <linux/fs.h>

TRACE_EVENT(shmem_undo_range_stats,

	TP_PROTO(struct inode *inode, loff_t lstart, uoff_t lend, bool unfalloc,
		 unsigned int loop1_batches, unsigned int loop1_entries,
		 unsigned int loop1_swap_entries, unsigned int loop1_folios,
		 unsigned int loop2_batches, unsigned int loop2_entries,
		 unsigned int loop2_swap_entries, unsigned int loop2_folios,
		 unsigned int loop2_full_restarts,
		 unsigned int loop2_swap_retries,
		 unsigned int loop2_mapping_retries,
		 unsigned int loop2_thp_split_restarts,
		 long nr_swaps_freed),

	TP_ARGS(inode, lstart, lend, unfalloc, loop1_batches, loop1_entries,
		loop1_swap_entries, loop1_folios, loop2_batches, loop2_entries,
		loop2_swap_entries, loop2_folios, loop2_full_restarts,
		loop2_swap_retries, loop2_mapping_retries,
		loop2_thp_split_restarts, nr_swaps_freed),

	TP_STRUCT__entry(
		__field(dev_t, s_dev)
		__field(unsigned long, ino)
		__field(loff_t, lstart)
		__field(loff_t, lend)
		__field(bool, unfalloc)
		__field(unsigned int, loop1_batches)
		__field(unsigned int, loop1_entries)
		__field(unsigned int, loop1_swap_entries)
		__field(unsigned int, loop1_folios)
		__field(unsigned int, loop2_batches)
		__field(unsigned int, loop2_entries)
		__field(unsigned int, loop2_swap_entries)
		__field(unsigned int, loop2_folios)
		__field(unsigned int, loop2_full_restarts)
		__field(unsigned int, loop2_swap_retries)
		__field(unsigned int, loop2_mapping_retries)
		__field(unsigned int, loop2_thp_split_restarts)
		__field(long, nr_swaps_freed)
	),

	TP_fast_assign(
		__entry->s_dev = inode->i_sb ? inode->i_sb->s_dev : inode->i_rdev;
		__entry->ino = inode->i_ino;
		__entry->lstart = lstart;
		__entry->lend = lend;
		__entry->unfalloc = unfalloc;
		__entry->loop1_batches = loop1_batches;
		__entry->loop1_entries = loop1_entries;
		__entry->loop1_swap_entries = loop1_swap_entries;
		__entry->loop1_folios = loop1_folios;
		__entry->loop2_batches = loop2_batches;
		__entry->loop2_entries = loop2_entries;
		__entry->loop2_swap_entries = loop2_swap_entries;
		__entry->loop2_folios = loop2_folios;
		__entry->loop2_full_restarts = loop2_full_restarts;
		__entry->loop2_swap_retries = loop2_swap_retries;
		__entry->loop2_mapping_retries = loop2_mapping_retries;
		__entry->loop2_thp_split_restarts = loop2_thp_split_restarts;
		__entry->nr_swaps_freed = nr_swaps_freed;
	),

	TP_printk("dev=%d:%d ino=%lu range=%lld-%lld unfalloc=%d "
		  "loop1[batches=%u entries=%u swap=%u folios=%u] "
		  "loop2[batches=%u entries=%u swap=%u folios=%u restarts=%u "
		  "swap_retries=%u mapping_retries=%u thp_split_restarts=%u] "
		  "swaps_freed=%ld",
		  MAJOR(__entry->s_dev), MINOR(__entry->s_dev), __entry->ino,
		  __entry->lstart, __entry->lend, __entry->unfalloc,
		  __entry->loop1_batches, __entry->loop1_entries,
		  __entry->loop1_swap_entries, __entry->loop1_folios,
		  __entry->loop2_batches, __entry->loop2_entries,
		  __entry->loop2_swap_entries, __entry->loop2_folios,
		  __entry->loop2_full_restarts, __entry->loop2_swap_retries,
		  __entry->loop2_mapping_retries,
		  __entry->loop2_thp_split_restarts, __entry->nr_swaps_freed)
);

#endif /* _TRACE_SHMEM_H */

/* This part must be outside protection */
#include <trace/define_trace.h>

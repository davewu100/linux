/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM shmem

#if !defined(_TRACE_SHMEM_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_SHMEM_H

#include <linux/tracepoint.h>
#include <linux/fs.h>

TRACE_EVENT(shmem_undo_range_stats,

	TP_PROTO(struct inode *inode, loff_t lstart, uoff_t lend, bool unfalloc,
		 const struct shmem_undo_range_trace_stats *stats),

	TP_ARGS(inode, lstart, lend, unfalloc, stats),

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
		__entry->loop1_batches = stats->loop1_batches;
		__entry->loop1_entries = stats->loop1_entries;
		__entry->loop1_swap_entries = stats->loop1_swap_entries;
		__entry->loop1_folios = stats->loop1_folios;
		__entry->loop2_batches = stats->loop2_batches;
		__entry->loop2_entries = stats->loop2_entries;
		__entry->loop2_swap_entries = stats->loop2_swap_entries;
		__entry->loop2_folios = stats->loop2_folios;
		__entry->loop2_full_restarts = stats->loop2_full_restarts;
		__entry->loop2_swap_retries = stats->loop2_swap_retries;
		__entry->loop2_mapping_retries = stats->loop2_mapping_retries;
		__entry->loop2_thp_split_restarts = stats->loop2_thp_split_restarts;
		__entry->nr_swaps_freed = stats->nr_swaps_freed;
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

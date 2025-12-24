#ifndef _MM_TLV_STATS_H
#define _MM_TLV_STATS_H

#include <linux/memcontrol.h>

/* Shared TLV layout constants */
#define TLV_HEADER_SIZE     4   /* Entry headers use 2-byte type + 2-byte length */
#define TLV_CONTAINER_HEADER_SIZE 6 /* Container headers use 2-byte type + 4-byte length */
#define TLV_U64_DATA_SIZE   8   /* u64 data size */
#define TLV_ENTRY_SIZE      (TLV_HEADER_SIZE + TLV_U64_DATA_SIZE)  /* 12 bytes */

struct memory_stat {
	const char *name;
	unsigned int idx;
};

extern const struct memory_stat memory_stats[];
extern const int memory_stats_count;

/* VM event helpers for TLV encoding */
enum vm_event_item memcg_tlv_event_item(int idx);
unsigned long memcg_events(struct mem_cgroup *memcg, int event);

/* TLV encoding functions for memory cgroup statistics */
int encode_memory_stats_tlv(struct mem_cgroup *memcg, void *buffer, size_t buffer_size);
#ifdef CONFIG_NUMA
int encode_memory_numa_stats_tlv(struct mem_cgroup *memcg, void *buffer, size_t buffer_size, int num_nodes);
#endif

/* Helpers to query stat counts (computed from memory_stats[]) */
int tlv_mem_stat_count(void);
int tlv_numa_stat_count(void);

#endif /* _MM_TLV_STATS_H */

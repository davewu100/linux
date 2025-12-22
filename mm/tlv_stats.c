#include <linux/memcontrol.h>
#include <linux/slab.h>
#include <linux/nodemask.h>
#include "tlv_stats.h"

// TLV type constants
#define TLV_TYPE_MEMORY_STATS_CONTAINER 0xFFFE
#define TLV_TYPE_NUMA_STATS_CONTAINER    0xFFFF
#define TLV_TYPE_MEMORY_SLAB            199

// Helper functions for TLV encoding
static inline void put_u16_be(uint8_t *buf, uint16_t val)
{
	buf[0] = (val >> 8) & 0xFF;
	buf[1] = val & 0xFF;
}

static inline void put_u64_be(uint8_t *buf, uint64_t val)
{
	buf[0] = (val >> 56) & 0xFF;
	buf[1] = (val >> 48) & 0xFF;
	buf[2] = (val >> 40) & 0xFF;
	buf[3] = (val >> 32) & 0xFF;
	buf[4] = (val >> 24) & 0xFF;
	buf[5] = (val >> 16) & 0xFF;
	buf[6] = (val >> 8) & 0xFF;
	buf[7] = val & 0xFF;
}

// TLV entry encoding helpers
struct tlv_encoder {
	uint8_t *buf;
	size_t pos;
	size_t size;
};

static inline bool tlv_can_write(struct tlv_encoder *enc, size_t bytes)
{
	return enc->pos + bytes <= enc->size;
}

static inline void tlv_write_u16(struct tlv_encoder *enc, uint16_t type, uint16_t len)
{
	put_u16_be(&enc->buf[enc->pos], type);
	enc->pos += 2;
	put_u16_be(&enc->buf[enc->pos], len);
	enc->pos += 2;
}

static inline void tlv_write_u64(struct tlv_encoder *enc, uint64_t value)
{
	put_u64_be(&enc->buf[enc->pos], value);
	enc->pos += 8;
}

// Helper functions to determine stat properties
static bool is_numa_aware_stat(int idx)
{
	// NUMA-aware stats are those that vary by memory node
	// Most memory accounting stats are NUMA-aware, except some global ones
	switch (idx) {
	case MEMCG_KMEM:      // Kernel memory allocation
	case MEMCG_PERCPU_B:  // Per-CPU buffers
	case MEMCG_SOCK:      // Socket memory
	case MEMCG_VMALLOC:   // Vmalloc allocations
#ifdef CONFIG_ZSWAP
	case MEMCG_ZSWAP_B:   // Zswap compressed pages
	case MEMCG_ZSWAPPED:  // Zswap stored pages
#endif
		return false;    // These are global, not per-node
	default:
		return true;     // Most stats are NUMA-aware
	}
}

static bool is_memory_event_stat(int idx)
{
	// Memory events have special indices
	return idx >= WORKINGSET_REFAULT_ANON && idx <= PGDEMOTE_PROACTIVE;
}

static bool is_slab_stat(int idx)
{
	return idx == NR_SLAB_RECLAIMABLE_B || idx == NR_SLAB_UNRECLAIMABLE_B;
}

static uint16_t get_tlv_type_for_stat(int stat_idx, int array_idx)
{
	// Simple mapping: most stats use their index + 1 as TLV type
	// But we can customize this if needed
	return stat_idx + 1;
}

// Combined slab stat getter
static u64 get_combined_slab_stat(struct mem_cgroup *memcg, int unused)
{
	u64 reclaimable = memcg_page_state(memcg, NR_SLAB_RECLAIMABLE_B);
	u64 unreclaimable = memcg_page_state(memcg, NR_SLAB_UNRECLAIMABLE_B);
	return reclaimable + unreclaimable;
}

int tlv_mem_stat_count(void)
{
	return memory_stats_count;
}

int tlv_numa_stat_count(void)
{
	int i, count = 0;

	for (i = 0; i < memory_stats_count; i++) {
		const struct memory_stat *stat = &memory_stats[i];

		if (!is_numa_aware_stat(stat->idx) ||
		    is_memory_event_stat(stat->idx) ||
		    is_slab_stat(stat->idx))
			continue;
		count++;
	}

	return count;
}

// Encode standard memory statistics using existing memory_stats array
static int encode_standard_memory_stats(struct tlv_encoder *enc, struct mem_cgroup *memcg)
{
	int i;

	for (i = 0; i < memory_stats_count; i++) {
		const struct memory_stat *stat = &memory_stats[i];
		u64 value;

		// Skip NUMA-aware stats in global encoding (handled separately)
		if (is_numa_aware_stat(stat->idx))
			continue;

		// Skip memory events (handled separately)
		if (is_memory_event_stat(stat->idx))
			continue;

		// Skip slab stats (handled as combined stat)
		if (is_slab_stat(stat->idx))
			continue;

		// Get value
		value = memcg_page_state(memcg, stat->idx);

		if (!tlv_can_write(enc, 12))
			return -1;

		tlv_write_u16(enc, get_tlv_type_for_stat(stat->idx, i), 8);
		tlv_write_u64(enc, value);
	}

	// Add combined slab statistic
	{
		u64 slab_value = get_combined_slab_stat(memcg, 0);
		if (!tlv_can_write(enc, 12))
			return -1;
		tlv_write_u16(enc, TLV_TYPE_MEMORY_SLAB, 8);
		tlv_write_u64(enc, slab_value);
	}

	return 0;
}

// Encode VM events (dynamic count, handled separately)
static int encode_vm_events(struct tlv_encoder *enc, struct mem_cgroup *memcg)
{
	int i;

	for (i = 0; i < memcg_nr_vm_events(); i++) {
		int event = memcg_tlv_event_item(i);
		u64 value = memcg_events(memcg, event);
		uint16_t tlv_type = 200 + i; // VM events start at 200

		if (!tlv_can_write(enc, 12))
			break;

		tlv_write_u16(enc, tlv_type, 8);
		tlv_write_u64(enc, value);
	}

	return 0;
}

// Encode NUMA-specific memory statistics for one node using existing memory_stats array
static int encode_numa_node_stats(struct tlv_encoder *enc, struct mem_cgroup *memcg, int node_id)
{
	struct lruvec *lruvec = mem_cgroup_lruvec(memcg, NODE_DATA(node_id));
	int i;

	if (!lruvec)
		return 0; // Skip inactive nodes

	for (i = 0; i < memory_stats_count; i++) {
		const struct memory_stat *stat = &memory_stats[i];

		// Only encode NUMA-aware stats that are not memory events or slab stats
		if (!is_numa_aware_stat(stat->idx) ||
		    is_memory_event_stat(stat->idx) ||
		    is_slab_stat(stat->idx))
			continue;

		u64 value = lruvec_page_state(lruvec, stat->idx);

		if (!tlv_can_write(enc, 14))
			return -1;

		// Write TLV entry: type(2) + length(2) + node_id(2) + value(8)
		tlv_write_u16(enc, get_tlv_type_for_stat(stat->idx, i), 10);  // Length includes node_id + value
		put_u16_be(&enc->buf[enc->pos], node_id);  // Node ID
		enc->pos += 2;
		tlv_write_u64(enc, value);
	}

	return 0;
}

int encode_memory_stats_tlv(struct mem_cgroup *memcg, void *buffer, size_t buffer_size)
{
	struct tlv_encoder enc = {
		.buf = buffer,
		.pos = 0,
		.size = buffer_size
	};
	size_t container_start;

	// Write container header (type + length placeholder)
	if (!tlv_can_write(&enc, 4))
		return -1;

	put_u16_be(&enc.buf[enc.pos], TLV_TYPE_MEMORY_STATS_CONTAINER);
	enc.pos += 2;
	put_u16_be(&enc.buf[enc.pos], 0); // length placeholder
	enc.pos += 2;
	container_start = enc.pos;

	mem_cgroup_flush_stats(memcg);

	// Encode different types of statistics
	if (encode_standard_memory_stats(&enc, memcg) < 0)
		return -1;

	if (encode_vm_events(&enc, memcg) < 0)
		return -1;

	// Fill in container length
	size_t data_len = enc.pos - container_start;
	if (data_len > 65535)
		return -1;

	put_u16_be(&enc.buf[container_start - 2], data_len);

	return enc.pos;
}

#ifdef CONFIG_NUMA
int encode_memory_numa_stats_tlv(struct mem_cgroup *memcg, void *buffer, size_t buffer_size, int num_nodes)
{
	struct tlv_encoder enc = {
		.buf = buffer,
		.pos = 0,
		.size = buffer_size
	};
	size_t container_start;
	int i;

	// Write container header
	if (!tlv_can_write(&enc, 4))
		return -1;

	put_u16_be(&enc.buf[enc.pos], TLV_TYPE_NUMA_STATS_CONTAINER);
	enc.pos += 2;
	put_u16_be(&enc.buf[enc.pos], 0); // length placeholder
	enc.pos += 2;
	container_start = enc.pos;

	mem_cgroup_flush_stats(memcg);

	// Encode statistics for each NUMA node
	for (i = 0; i < num_nodes; i++) {
		if (encode_numa_node_stats(&enc, memcg, i) < 0)
			return -1;
	}

	// Fill in container length
	size_t data_len = enc.pos - container_start;
	if (data_len > 65535)
		return -1;

	put_u16_be(&enc.buf[container_start - 2], data_len);

	return enc.pos;
}
#endif

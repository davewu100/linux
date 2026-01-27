// SPDX-License-Identifier: GPL-2.0
/*
 * Helper program to find the correct array index for mem_cgroup vmstats.state[]
 * 
 * This calculates the index mapping based on memcg_node_stat_items[] and
 * memcg_stat_items[] arrays from mm/memcontrol.c
 */

#include <stdio.h>

/* From include/linux/vmstat.h - approximate enum values */
enum node_stat_item {
	NR_INACTIVE_ANON = 0,
	NR_ACTIVE_ANON = 1,
	NR_INACTIVE_FILE = 2,
	NR_ACTIVE_FILE = 3,
	NR_UNEVICTABLE = 4,
	NR_SLAB_RECLAIMABLE_B = 5,
	NR_SLAB_UNRECLAIMABLE_B = 6,
	WORKINGSET_REFAULT_ANON = 7,
	WORKINGSET_REFAULT_FILE = 8,
	WORKINGSET_ACTIVATE_ANON = 9,
	WORKINGSET_ACTIVATE_FILE = 10,
	WORKINGSET_RESTORE_ANON = 11,
	WORKINGSET_RESTORE_FILE = 12,
	WORKINGSET_NODERECLAIM = 13,
	NR_ANON_MAPPED = 14,
	NR_FILE_MAPPED = 15,
	NR_FILE_PAGES = 16,
	NR_FILE_DIRTY = 17,
	NR_WRITEBACK = 18,
	NR_SHMEM = 19,
	NR_SHMEM_THPS = 20,
	NR_FILE_THPS = 21,
	NR_ANON_THPS = 22,
	NR_KERNEL_STACK_KB = 23,
	NR_PAGETABLE = 24,
	NR_SECONDARY_PAGETABLE = 25,
	NR_SWAPCACHE = 26,
	PGPROMOTE_SUCCESS = 27,
	PGDEMOTE_KSWAPD = 28,
	PGDEMOTE_DIRECT = 29,
	PGDEMOTE_KHUGEPAGED = 30,
	PGDEMOTE_PROACTIVE = 31,
	NR_HUGETLB = 32,
};

enum memcg_stat_item {
	MEMCG_SWAP = 100,  /* Approximate */
	MEMCG_SOCK = 101,
	MEMCG_PERCPU_B = 102,
	MEMCG_VMALLOC = 103,
	MEMCG_KMEM = 104,
	MEMCG_ZSWAP_B = 105,
	MEMCG_ZSWAPPED = 106,
};

/* From mm/memcontrol.c - memcg_node_stat_items[] */
static const unsigned int memcg_node_stat_items[] = {
	NR_INACTIVE_ANON,
	NR_ACTIVE_ANON,
	NR_INACTIVE_FILE,
	NR_ACTIVE_FILE,
	NR_UNEVICTABLE,
	NR_SLAB_RECLAIMABLE_B,
	NR_SLAB_UNRECLAIMABLE_B,
	WORKINGSET_REFAULT_ANON,
	WORKINGSET_REFAULT_FILE,
	WORKINGSET_ACTIVATE_ANON,
	WORKINGSET_ACTIVATE_FILE,
	WORKINGSET_RESTORE_ANON,
	WORKINGSET_RESTORE_FILE,
	WORKINGSET_NODERECLAIM,
	NR_ANON_MAPPED,      /* Index 13 in array = vmstats.state[13] */
	NR_FILE_MAPPED,
	NR_FILE_PAGES,       /* Index 15 in array = vmstats.state[15] */
	NR_FILE_DIRTY,
	NR_WRITEBACK,
	NR_SHMEM,
	NR_SHMEM_THPS,
	NR_FILE_THPS,
	NR_ANON_THPS,
	NR_KERNEL_STACK_KB,  /* Index 23 in array = vmstats.state[23] */
	NR_PAGETABLE,
	NR_SECONDARY_PAGETABLE,
#ifdef CONFIG_SWAP
	NR_SWAPCACHE,
#endif
#ifdef CONFIG_NUMA_BALANCING
	PGPROMOTE_SUCCESS,
#endif
	PGDEMOTE_KSWAPD,
	PGDEMOTE_DIRECT,
	PGDEMOTE_KHUGEPAGED,
	PGDEMOTE_PROACTIVE,
#ifdef CONFIG_HUGETLB_PAGE
	NR_HUGETLB,
#endif
};

/* From mm/memcontrol.c - memcg_stat_items[] */
static const unsigned int memcg_stat_items[] = {
	MEMCG_SWAP,
	MEMCG_SOCK,
	MEMCG_PERCPU_B,
	MEMCG_VMALLOC,
	MEMCG_KMEM,          /* Index 4 in this array */
	MEMCG_ZSWAP_B,
	MEMCG_ZSWAPPED,
};

#define NR_MEMCG_NODE_STAT_ITEMS (sizeof(memcg_node_stat_items) / sizeof(memcg_node_stat_items[0]))
#define NR_MEMCG_STAT_ITEMS (sizeof(memcg_stat_items) / sizeof(memcg_stat_items[0]))

int find_index_in_array(unsigned int item, const unsigned int *arr, int size)
{
	int i;
	for (i = 0; i < size; i++) {
		if (arr[i] == item)
			return i;
	}
	return -1;
}

int main(void)
{
	int idx;
	int node_stat_idx, memcg_stat_idx;
	
	printf("=== mem_cgroup vmstats.state[] Index Mapping ===\n\n");
	
	/* Find NR_ANON_MAPPED */
	node_stat_idx = find_index_in_array(NR_ANON_MAPPED, memcg_node_stat_items, NR_MEMCG_NODE_STAT_ITEMS);
	if (node_stat_idx >= 0) {
		printf("NR_ANON_MAPPED (anon):\n");
		printf("  Enum value: %d\n", NR_ANON_MAPPED);
		printf("  Array index in memcg_node_stat_items: %d\n", node_stat_idx);
		printf("  vmstats.state[%d] = anon\n\n", node_stat_idx);
	}
	
	/* Find NR_FILE_PAGES */
	node_stat_idx = find_index_in_array(NR_FILE_PAGES, memcg_node_stat_items, NR_MEMCG_NODE_STAT_ITEMS);
	if (node_stat_idx >= 0) {
		printf("NR_FILE_PAGES (file):\n");
		printf("  Enum value: %d\n", NR_FILE_PAGES);
		printf("  Array index in memcg_node_stat_items: %d\n", node_stat_idx);
		printf("  vmstats.state[%d] = file\n\n", node_stat_idx);
	}
	
	/* Find NR_KERNEL_STACK_KB */
	node_stat_idx = find_index_in_array(NR_KERNEL_STACK_KB, memcg_node_stat_items, NR_MEMCG_NODE_STAT_ITEMS);
	if (node_stat_idx >= 0) {
		printf("NR_KERNEL_STACK_KB (kernel_stack):\n");
		printf("  Enum value: %d\n", NR_KERNEL_STACK_KB);
		printf("  Array index in memcg_node_stat_items: %d\n", node_stat_idx);
		printf("  vmstats.state[%d] = kernel_stack\n\n", node_stat_idx);
	}
	
	/* Find MEMCG_KMEM */
	memcg_stat_idx = find_index_in_array(MEMCG_KMEM, memcg_stat_items, NR_MEMCG_STAT_ITEMS);
	if (memcg_stat_idx >= 0) {
		idx = NR_MEMCG_NODE_STAT_ITEMS + memcg_stat_idx;
		printf("MEMCG_KMEM (kernel):\n");
		printf("  Enum value: %d\n", MEMCG_KMEM);
		printf("  Array index in memcg_stat_items: %d\n", memcg_stat_idx);
		printf("  vmstats.state[%d] = kernel (NR_MEMCG_NODE_STAT_ITEMS + %d)\n\n", idx, memcg_stat_idx);
	}
	
	/* Find NR_SHMEM */
	node_stat_idx = find_index_in_array(NR_SHMEM, memcg_node_stat_items, NR_MEMCG_NODE_STAT_ITEMS);
	if (node_stat_idx >= 0) {
		printf("NR_SHMEM (shmem):\n");
		printf("  Enum value: %d\n", NR_SHMEM);
		printf("  Array index in memcg_node_stat_items: %d\n", node_stat_idx);
		printf("  vmstats.state[%d] = shmem\n\n", node_stat_idx);
	}
	
	/* Find NR_PAGETABLE */
	node_stat_idx = find_index_in_array(NR_PAGETABLE, memcg_node_stat_items, NR_MEMCG_NODE_STAT_ITEMS);
	if (node_stat_idx >= 0) {
		printf("NR_PAGETABLE (pagetables):\n");
		printf("  Enum value: %d\n", NR_PAGETABLE);
		printf("  Array index in memcg_node_stat_items: %d\n", node_stat_idx);
		printf("  vmstats.state[%d] = pagetables\n\n", node_stat_idx);
	}
	
	printf("=== Summary ===\n");
	printf("NR_MEMCG_NODE_STAT_ITEMS = %zu\n", NR_MEMCG_NODE_STAT_ITEMS);
	printf("NR_MEMCG_STAT_ITEMS = %zu\n", NR_MEMCG_STAT_ITEMS);
	printf("\n");
	printf("Use these indices with: vmstats.state[index]\n");
	printf("Example: vmstats.state[13] for anon\n");
	
	return 0;
}

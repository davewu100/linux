// SPDX-License-Identifier: GPL-2.0
/*
 * sizeof_check.c - static proof of the memory win from shrinking
 * struct zswap_entry (pool pointer -> u16 pool_idx).
 *
 * This replicates the exact field layout of struct zswap_entry before and
 * after the patch (x86_64 types/alignment) and prints sizeof() for each,
 * plus the per-entry and aggregate savings. It needs no kernel, no root, and
 * no liburcu -- it is a compile-time fact check that matches the pahole
 * numbers quoted in the commit message (56 -> 48 bytes).
 *
 * The authoritative number on a real build is the 'objsize' column for the
 * 'zswap_entry' line in /proc/slabinfo; this program shows you what that
 * column should read on each side.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

/* Minimal stand-ins with the same size/alignment as the kernel types. */
typedef struct { unsigned long val; } swp_entry_t;	/* 8 bytes */
struct list_head { struct list_head *next, *prev; };	/* 16 bytes */
struct obj_cgroup;					/* opaque pointer */
struct zswap_pool;					/* opaque pointer */

/* ---- before the patch ---- */
struct zswap_entry_before {
	swp_entry_t swpentry;		/* 8  */
	unsigned int length;		/* 4  */
	_Bool referenced;		/* 1 (+3 pad) */
	struct zswap_pool *pool;	/* 8  */
	unsigned long handle;		/* 8  */
	struct obj_cgroup *objcg;	/* 8  */
	struct list_head lru;		/* 16 */
};

/* ---- after the patch ---- */
struct zswap_entry_after {
	swp_entry_t swpentry;		/* 8  */
	unsigned int length;		/* 4  */
	_Bool referenced;		/* 1  */
	uint16_t pool_idx;		/* 2  -> lands in the old padding */
	unsigned long handle;		/* 8  */
	struct obj_cgroup *objcg;	/* 8  */
	struct list_head lru;		/* 16 */
};

int main(int argc, char **argv)
{
	size_t before = sizeof(struct zswap_entry_before);
	size_t after  = sizeof(struct zswap_entry_after);
	size_t saved  = before - after;

	/* Optional: pass an entry count to see the aggregate saving. */
	unsigned long n = (argc > 1) ? strtoul(argv[1], NULL, 0) : 0;

	printf("struct zswap_entry size:\n");
	printf("  before patch : %zu bytes\n", before);
	printf("  after  patch : %zu bytes\n", after);
	printf("  saved/entry  : %zu bytes (%.1f%%)\n",
	       saved, before ? 100.0 * saved / before : 0.0);

	if (n) {
		printf("\nfor %lu live entries:\n", n);
		printf("  before : %.2f MiB\n", (double)before * n / (1024*1024));
		printf("  after  : %.2f MiB\n", (double)after  * n / (1024*1024));
		printf("  saved  : %.2f MiB\n", (double)saved  * n / (1024*1024));
	} else {
		printf("\nTip: pass a live-entry count to size the aggregate win,\n"
		       "e.g. './sizeof_check 1000000' for 1M stored pages.\n");
	}

	/* Sanity: this must match the commit message (56 -> 48 on x86_64). */
	if (before != 56 || after != 48)
		printf("\nNOTE: sizes differ from the x86_64 reference (56->48); "
		       "you are likely on a different ABI/arch.\n");
	return 0;
}

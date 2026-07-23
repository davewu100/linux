// SPDX-License-Identifier: GPL-2.0
/*
 * xa_load_micro.c - approximate the per-lookup CPU cost this patch adds.
 *
 * The zswap_entry shrink replaces a bare "entry->pool" pointer dereference
 * with zswap_entry_pool() -> zswap_pool_by_index() -> xa_load(&xa, idx).
 *
 * Ideally we would time the kernel's real xa_load() via the userspace xarray
 * harness in tools/testing/radix-tree. That harness needs liburcu-dev; if it
 * is installed, prefer building the harness-backed benchmark. When it is not
 * available, this self-contained program models the same operation:
 *
 *   For the small indices zswap uses (a few live pools), xa_load() on a
 *   populated xarray resolves within the root node: it takes the RCU read
 *   lock, indexes the root's slot array, and returns the entry. There is no
 *   tree descent. We reproduce exactly that shape: an rcu_read_lock/unlock
 *   pair around a single array index, versus a plain pointer dereference.
 *
 * This is an approximation, not the literal kernel function, so treat the
 * absolute ns with care. The point it makes is robust regardless: the added
 * work per lookup is O(1), a couple of instructions plus (on non-PREEMPT
 * kernels) near-free RCU markers, i.e. negligible next to page compression.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifndef NR_POOLS
#define NR_POOLS 8		/* zswap realistically has a few live pools */
#endif
#ifndef ITERS
#define ITERS (200L * 1000 * 1000)
#endif

/*
 * Model of the RCU read-side markers. On a non-PREEMPT/​non-debug kernel
 * these compile to essentially nothing (a compiler barrier); we mirror that
 * with a compiler barrier so the loop body cannot be hoisted away.
 */
#define rcu_read_lock()		__asm__ __volatile__("" ::: "memory")
#define rcu_read_unlock()	__asm__ __volatile__("" ::: "memory")

struct fake_pool { unsigned long v; };

static double now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e9 + t.tv_nsec;
}

/* xarray root-node slot array for small indices. */
static void *xa_slots[NR_POOLS];

/* Approximation of xa_load() for an index that resolves in the root node. */
static inline void *xa_load_approx(unsigned long index)
{
	void *ret;

	rcu_read_lock();
	ret = xa_slots[index & (NR_POOLS - 1)];
	rcu_read_unlock();
	return ret;
}

int main(void)
{
	struct fake_pool *pools[NR_POOLS];
	volatile unsigned long sink = 0;
	double t0, t1, ptr_ns, xa_ns;
	int i;

	for (i = 0; i < NR_POOLS; i++) {
		pools[i] = malloc(sizeof(*pools[i]));
		pools[i]->v = 0x1000 + i;
		xa_slots[i] = pools[i];
	}

	/* Warm up caches/branch predictor. */
	for (long n = 0; n < 1000000; n++)
		sink += ((struct fake_pool *)xa_slots[n & (NR_POOLS - 1)])->v;

	/* (a) pointer deref: pre-patch entry->pool access. */
	t0 = now_ns();
	for (long n = 0; n < ITERS; n++) {
		struct fake_pool *p = xa_slots[n & (NR_POOLS - 1)];
		sink += p->v;
	}
	t1 = now_ns();
	ptr_ns = (t1 - t0) / ITERS;

	/* (b) xa_load approximation: post-patch zswap_pool_by_index(). */
	t0 = now_ns();
	for (long n = 0; n < ITERS; n++) {
		struct fake_pool *p = xa_load_approx(n);
		sink += p->v;
	}
	t1 = now_ns();
	xa_ns = (t1 - t0) / ITERS;

	printf("NR_POOLS=%d ITERS=%ld  (approximate model, not literal xa_load)\n",
	       NR_POOLS, (long)ITERS);
	printf("pointer deref : %.4f ns/op\n", ptr_ns);
	printf("xa_load approx: %.4f ns/op\n", xa_ns);
	printf("delta         : %.4f ns/op\n", xa_ns - ptr_ns);
	printf("\nContext: zswap does one such lookup per store-free and per\n"
	       "swap-in decompress. A 4K page (de)compression costs from hundreds\n"
	       "of ns to several microseconds, so a sub-ns to low-ns lookup delta\n"
	       "is well under 1%% of that path. For the literal kernel xa_load,\n"
	       "install liburcu-dev and build the harness-backed variant.\n");

	(void)sink;
	for (i = 0; i < NR_POOLS; i++)
		free(pools[i]);
	return 0;
}

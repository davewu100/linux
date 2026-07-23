# SPDX-License-Identifier: GPL-2.0
#
# Tests for "mm/zswap: reference the pool by index to shrink struct
# zswap_entry". Two self-contained programs plus one on-DUT shell script:
#
#   sizeof_check   - compile-time proof of the 56 -> 48 byte shrink and the
#                    aggregate memory saving for a given live-entry count.
#   xa_load_micro  - approximate per-lookup CPU cost added by the pool-index
#                    xa_load() vs the old bare pointer dereference.
#   zswap_entry_bench.sh - runs on a machine booted with the patched kernel
#                    (root): measures the real slab objsize/footprint from
#                    /proc/slabinfo and a swap-in CPU benchmark.
#
# The two C programs need no kernel, no root, and no liburcu; run them here.

CFLAGS := -O2 -g -Wall -Wextra

PROGS := sizeof_check xa_load_micro

all: $(PROGS)

sizeof_check: sizeof_check.c
	$(CC) $(CFLAGS) $< -o $@

xa_load_micro: xa_load_micro.c
	$(CC) $(CFLAGS) $< -o $@

# Convenience: build and run the two host-side checks.
.PHONY: run
run: $(PROGS)
	@echo "=================== MEMORY (sizeof) ==================="
	./sizeof_check 1000000
	@echo
	@echo "=================== CPU (lookup) ======================"
	./xa_load_micro

.PHONY: clean
clean:
	$(RM) $(PROGS)

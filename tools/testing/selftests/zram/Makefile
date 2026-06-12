# SPDX-License-Identifier: GPL-2.0
CFLAGS += -Wall -O2
LDLIBS += -lrt

all: zram_swap_int zram_wb_bench

zram_swap_int: zram_swap_int.c
zram_wb_bench: zram_wb_bench.c

TEST_PROGS := zram.sh
TEST_GEN_PROGS := zram_swap_int
TEST_FILES := zram01.sh zram02.sh zram03.sh zram04_loop_swap.sh zram_lib.sh zram_wb_bench.sh
EXTRA_CLEAN := err.log zram_swap_int zram_wb_bench

include ../lib.mk

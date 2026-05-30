# SPDX-License-Identifier: GPL-2.0
CFLAGS += -Wall -O2
LDLIBS += -lrt

all: zram_pageout zram_wb_bench

zram_pageout: zram_pageout.c
zram_wb_bench: zram_wb_bench.c

TEST_PROGS := zram.sh
TEST_GEN_PROGS := zram_pageout zram_wb_bench
TEST_FILES := zram01.sh zram02.sh zram03.sh zram_lib.sh zram_wb_bench.sh
EXTRA_CLEAN := err.log

include ../lib.mk

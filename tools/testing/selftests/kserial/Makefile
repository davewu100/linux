# SPDX-License-Identifier: GPL-2.0
CFLAGS += -O2 -Wall -Wextra $(KHDR_INCLUDES)

TEST_GEN_PROGS = kserial_test

kserial_test: ../../../kserial/kserial_test.c
	$(CC) $(CFLAGS) -o $@ $<

include ../lib.mk

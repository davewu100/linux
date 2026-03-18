# SPDX-License-Identifier: GPL-2.0
CFLAGS += -Wall $(KHDR_INCLUDES)

TEST_GEN_PROGS := test_kserial_dev

include ../lib.mk

# Use kernel UAPI header (single source of truth). Define __EXPORTED_HEADERS__
# so that include/uapi/linux/types.h does not emit the "kernel headers from user
# space" warning (same as headers_install does).
CFLAGS += -I$(top_srcdir)/include/uapi -D__EXPORTED_HEADERS__

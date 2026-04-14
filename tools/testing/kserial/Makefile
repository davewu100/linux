# SPDX-License-Identifier: GPL-2.0-only

CC	:= gcc
CFLAGS	:= -O2 -Wall -Wextra -g \
	   -I../../../include/uapi

# libbpf for tcp_kserial: prefer a system install, fall back to the in-tree
# library.  Build the in-tree library with:
#   make -C ../../../tools/lib/bpf OUTPUT=$(LIBBPF_OUTPUT)/
LIBBPF_OUTPUT ?= /tmp/libbpf_build
LIBBPF_SRC    := ../../../tools/lib/bpf

# Use in-tree headers; suppress the "kernel headers from user space" advisory.
LIBBPF_CFLAGS := -I$(LIBBPF_SRC)/.. -Wno-cpp
LIBBPF_LDFLAGS := $(LIBBPF_OUTPUT)/libbpf.a -lelf -lz

all: kserial_test tcp_kserial kserial_gen_proto

kserial_test: kserial_test.c
	$(CC) $(CFLAGS) -o $@ $<

# Build in-tree libbpf if the static archive is missing.
$(LIBBPF_OUTPUT)/libbpf.a:
	$(MAKE) -C $(LIBBPF_SRC) OUTPUT=$(LIBBPF_OUTPUT)/

# tcp_kserial uses vmlinux BTF (via libbpf) to resolve field names at decode
# time, so the output shows ".tcpi_rtt = 1234" instead of "field_24 = 1234".
tcp_kserial: tcp_kserial.c $(LIBBPF_OUTPUT)/libbpf.a
	$(CC) $(CFLAGS) $(LIBBPF_CFLAGS) -o $@ $< $(LIBBPF_LDFLAGS)

kserial_gen_proto: kserial_gen_proto.c $(LIBBPF_OUTPUT)/libbpf.a
	$(CC) $(CFLAGS) $(LIBBPF_CFLAGS) -o $@ $< $(LIBBPF_LDFLAGS)

clean:
	rm -f kserial_test tcp_kserial kserial_gen_proto

.PHONY: all clean

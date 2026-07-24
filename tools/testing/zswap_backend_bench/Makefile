# SPDX-License-Identifier: GPL-2.0
# Out-of-tree build for the zswap backend micro-benchmark module.
#
# From a built kernel tree:
#   make -C tools/testing/zswap_backend_bench KDIR=$PWD
# or against installed headers:
#   make -C tools/testing/zswap_backend_bench
#
# The target kernel must provide the lib/zcomp symbols, i.e. be built with
# CONFIG_ZCOMP=y (implied by CONFIG_ZSWAP=y) and the relevant
# CONFIG_ZCOMP_BACKEND_* for the algorithm under test, plus the matching
# crypto software algorithm (e.g. CONFIG_CRYPTO_LZO) so both backends exist.

ifneq ($(KERNELRELEASE),)
# kbuild part - invoked from the kernel build system
obj-m := zswap_backend_bench.o
else
# normal part - invoked from the command line
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
endif

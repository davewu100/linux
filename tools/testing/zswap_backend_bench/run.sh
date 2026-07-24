#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Sweep the zswap backend micro-benchmark over the affected (shared)
# algorithms and print a summary from dmesg.  Run as root on the target
# kernel that was built from this tree.
#
# Usage:
#   ./run.sh [iters] [npages] [alg ...]
# Examples:
#   ./run.sh
#   ./run.sh 500000 512
#   ./run.sh 200000 256 lzo lz4 zstd

set -eu

MOD=zswap_backend_bench
KO=./$MOD.ko

ITERS=${1:-200000}
NPAGES=${2:-256}
[ "$#" -ge 1 ] && shift
[ "$#" -ge 1 ] && shift

if [ "$#" -gt 0 ]; then
	ALGS="$*"
else
	# affected / shared algorithms only
	ALGS="lzo lzo-rle lz4 lz4hc zstd deflate 842"
fi

if [ "$(id -u)" -ne 0 ]; then
	echo "must run as root (insmod)" >&2
	exit 1
fi

if [ ! -f "$KO" ]; then
	echo "$KO not found; build it first: make -C . KDIR=<kernel tree>" >&2
	exit 1
fi

for alg in $ALGS; do
	# skip cleanly if the module refuses (e.g. backend/crypto alg missing)
	rmmod "$MOD" 2>/dev/null || true
	echo "== $alg =="
	# the module always returns an error to self-unload; that is expected
	insmod "$KO" alg="$alg" iters="$ITERS" npages="$NPAGES" 2>/dev/null || true
	dmesg | grep "zswap_backend_bench:" | tail -n 12
	echo
done

#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Regression: ordinary block-device swap (non-zram) still works with swap_ops
# registration present (zram registers swap_register_block_ops at module init).

TCID="zram04_loop_swap"
ERR_CODE=0

. ./zram_lib.sh

LOOP_SWAP_IMG="${LOOP_SWAP_IMG:-/var/tmp/zram04-loop-swap.img}"
LOOP_SWAP_SIZE="${LOOP_SWAP_SIZE:-256M}"

fail()
{
	echo "FAIL: $*"
	ERR_CODE=1
}

ok()
{
	echo "$*: OK"
}

check_prereqs

if ! command -v stress-ng >/dev/null 2>&1; then
	echo "skip: stress-ng not installed"
	exit $ksft_skip
fi

truncate -s "$LOOP_SWAP_SIZE" "$LOOP_SWAP_IMG"
losetup -j "$LOOP_SWAP_IMG" | cut -d: -f1 | while read -r d; do
	[ -n "$d" ] && losetup -d "$d" || true
done

loopdev=$(losetup -fP --show "$LOOP_SWAP_IMG") || {
	fail "losetup"
	exit 1
}

mkswap "$loopdev" > err.log 2>&1 || {
	cat err.log
	fail "mkswap $loopdev"
	losetup -d "$loopdev"
	rm -f "$LOOP_SWAP_IMG"
	exit 1
}

swapon "$loopdev" > err.log 2>&1 || {
	cat err.log
	fail "swapon $loopdev"
	losetup -d "$loopdev"
	rm -f "$LOOP_SWAP_IMG"
	exit 1
}

if stress-ng --vm 1 --vm-bytes 200M --vm-keep -t 10 --quiet; then
	ok "loop swap stress-ng"
else
	fail "loop swap stress-ng"
fi

swapoff "$loopdev" || fail "swapoff $loopdev"
losetup -d "$loopdev" || fail "losetup -d"
rm -f "$LOOP_SWAP_IMG"

if [ $ERR_CODE -ne 0 ]; then
	echo "$TCID : [FAIL]"
else
	echo "$TCID : [PASS]"
fi

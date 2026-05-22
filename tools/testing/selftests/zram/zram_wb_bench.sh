#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Manual zram writeback swap-in latency benchmark.

set -euo pipefail

ZRAM_DEV="${ZRAM_DEV:-/dev/zram0}"
ZRAM_SYS="${ZRAM_SYS:-/sys/block/zram0}"
BACKING_IMG="${BACKING_IMG:-/var/tmp/zram-backing.img}"
BACKING_SIZE="${BACKING_SIZE:-16G}"
COMP="${COMP:-lzo-rle}"
BENCH_BIN="${BENCH_BIN:-./zram_wb_bench}"
loopdev=""

require_root()
{
	if [ "$(id -u)" -ne 0 ]; then
		echo "Please run as root" >&2
		exit 1
	fi
}

setup_zram()
{
	require_root

	swapoff "$ZRAM_DEV" 2>/dev/null || true
	echo 1 > "$ZRAM_SYS/reset" 2>/dev/null || true

	truncate -s "$BACKING_SIZE" "$BACKING_IMG"
	loopdev=$(losetup -fP --show "$BACKING_IMG")

	echo "$loopdev" > "$ZRAM_SYS/backing_dev"
	echo "$COMP" > "$ZRAM_SYS/comp_algorithm"
	echo "$BACKING_SIZE" > "$ZRAM_SYS/disksize"

	mkswap "$ZRAM_DEV"
	swapon -p 32767 "$ZRAM_DEV"

	echo "backing loop: $loopdev"
	cat "$ZRAM_SYS/backing_dev"
	cat "$ZRAM_SYS/mm_stat"
	cat "$ZRAM_SYS/bd_stat"
}

cleanup_zram()
{
	require_root

	swapoff "$ZRAM_DEV" 2>/dev/null || true
	echo 1 > "$ZRAM_SYS/reset" 2>/dev/null || true

	losetup -j "$BACKING_IMG" | cut -d: -f1 | while read -r dev; do
		[ -n "$dev" ] && losetup -d "$dev" || true
	done

	rm -f "$BACKING_IMG"
}

show_stats()
{
	echo "== /proc/vmstat =="
	grep -E 'pswpin|pswpout' /proc/vmstat || true

	echo "== mm_stat =="
	cat "$ZRAM_SYS/mm_stat"

	echo "== bd_stat =="
	cat "$ZRAM_SYS/bd_stat"

	echo "== io_stat =="
	cat "$ZRAM_SYS/io_stat"
}

run_bench()
{
	local gb="${1:-18}"

	require_root
	[ -x "$BENCH_BIN" ] || {
		echo "Missing benchmark binary: $BENCH_BIN" >&2
		exit 1
	}

	echo "== zspool swap-in benchmark =="
	show_stats
	"$BENCH_BIN" --gb "$gb" --zram-sys "$ZRAM_SYS"
	show_stats

	echo "== ZRAM_WB swap-in benchmark =="
	show_stats
	"$BENCH_BIN" --gb "$gb" --zram-sys "$ZRAM_SYS" --writeback
	show_stats
}

case "${1:-}" in
setup)
	setup_zram
	;;
bench)
	run_bench "${2:-18}"
	;;
cleanup)
	cleanup_zram
	;;
*)
	echo "Usage:"
	echo "  sudo $0 setup"
	echo "  sudo $0 bench [GB]"
	echo "  sudo $0 cleanup"
	exit 1
	;;
esac

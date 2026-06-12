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
BENCH_FLAGS="${BENCH_FLAGS:---cold --seed 42 --stride 64 --drop-caches}"
loopdev=""

require_root()
{
	if [ "$(id -u)" -ne 0 ]; then
		echo "Please run as root" >&2
		exit 1
	fi
}

ensure_zram_dev()
{
	if [ -e "$ZRAM_SYS/disksize" ]; then
		return 0
	fi

	if [ ! -d /sys/class/zram-control ]; then
		modprobe zram || {
			echo "Failed to load zram module" >&2
			exit 1
		}
	fi

	if [ -d /sys/class/zram-control ] && [ ! -e "$ZRAM_DEV" ]; then
		cat /sys/class/zram-control/hot_add > /dev/null
	fi

	if [ ! -e "$ZRAM_SYS/disksize" ]; then
		echo "Failed to create $ZRAM_DEV" >&2
		exit 1
	fi

	for node in backing_dev writeback idle; do
		[ -e "$ZRAM_SYS/$node" ] || {
			echo "zram writeback requires $ZRAM_SYS/$node" >&2
			exit 1
		}
	done
}

configure_zram_swap()
{
	resolve_loopdev
	[ -n "$loopdev" ] || {
		echo "No backing loop device; run setup first" >&2
		exit 1
	}

	echo "$loopdev" > "$ZRAM_SYS/backing_dev"
	echo "$COMP" > "$ZRAM_SYS/comp_algorithm"
	echo "$BACKING_SIZE" > "$ZRAM_SYS/disksize"

	mkswap "$ZRAM_DEV"
	swapon -p 32767 "$ZRAM_DEV"
}

resolve_loopdev()
{
	if [ -n "$loopdev" ]; then
		return 0
	fi

	if [ -f "$BACKING_IMG" ]; then
		loopdev=$(losetup -j "$BACKING_IMG" | cut -d: -f1 | head -1)
	fi

	if [ -z "$loopdev" ] && [ -e "$ZRAM_SYS/backing_dev" ]; then
		loopdev=$(cat "$ZRAM_SYS/backing_dev")
	fi
}

reset_zram()
{
	swapoff "$ZRAM_DEV" 2>/dev/null || true
	echo 1 > "$ZRAM_SYS/reset"
	loopdev=""
	if [ -f "$BACKING_IMG" ]; then
		truncate -s "$BACKING_SIZE" "$BACKING_IMG"
	else
		truncate -s "$BACKING_SIZE" "$BACKING_IMG"
	fi
	losetup -j "$BACKING_IMG" | cut -d: -f1 | while read -r dev; do
		[ -n "$dev" ] && losetup -d "$dev" || true
	done
	loopdev=$(losetup -fP --show "$BACKING_IMG")
	configure_zram_swap
}

setup_zram()
{
	require_root
	ensure_zram_dev

	swapoff "$ZRAM_DEV" 2>/dev/null || true
	echo 1 > "$ZRAM_SYS/reset" 2>/dev/null || true

	truncate -s "$BACKING_SIZE" "$BACKING_IMG"
	loopdev=$(losetup -fP --show "$BACKING_IMG")

	configure_zram_swap

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

run_bench_once()
{
	local label="$1"
	local gb="$2"
	shift 2

	echo "== $label =="
	show_stats
	# shellcheck disable=SC2086
	"$BENCH_BIN" --gb "$gb" --zram-sys "$ZRAM_SYS" $BENCH_FLAGS "$@"
	show_stats
}

run_bench()
{
	local gb=3
	local reset_between=1

	while [ $# -gt 0 ]; do
		case "$1" in
		--reset-between)
			reset_between=1
			shift
			;;
		--no-reset-between)
			reset_between=0
			shift
			;;
		--sequential)
			BENCH_FLAGS="--drop-caches"
			shift
			;;
		--legacy)
			BENCH_FLAGS="--random --drop-caches"
			shift
			;;
		*)
			gb="$1"
			shift
			;;
		esac
	done

	require_root
	ensure_zram_dev
	resolve_loopdev
	[ -n "$loopdev" ] || {
		echo "No backing loop device; run setup first" >&2
		exit 1
	}
	[ -x "$BENCH_BIN" ] || {
		echo "Missing benchmark binary: $BENCH_BIN" >&2
		exit 1
	}

	echo "== reset zram before benchmark =="
	reset_zram
	show_stats

	echo "bench flags:${BENCH_FLAGS:+ $BENCH_FLAGS}"

	run_bench_once "zspool swap-in benchmark" "$gb"

	if [ "$reset_between" -eq 1 ]; then
		echo "== reset zram between benchmarks =="
		reset_zram
		show_stats
	fi

	run_bench_once "ZRAM_WB swap-in benchmark" "$gb" --writeback
}

case "${1:-}" in
setup)
	setup_zram
	;;
bench)
	shift
	run_bench "$@"
	;;
cleanup)
	cleanup_zram
	;;
*)
	echo "Usage:"
	echo "  sudo $0 setup"
	echo "  sudo $0 bench [GB] [--no-reset-between] [--sequential] [--legacy]"
	echo "  sudo $0 cleanup"
	echo ""
	echo "bench resets zram by default before each run and between zspool/WB."
	echo "Default bench uses --cold (--stride 64 --seed 42 --drop-caches)."
	exit 1
	;;
esac

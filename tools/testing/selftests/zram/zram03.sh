#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Test zram writeback swap-in from a backing device.
#
# This test uses swapon -p 32767 so the test zram device wins over any
# existing system swap while the test is running. Cleanup swaps it off again.

set -uo pipefail

TCID="zram03"
ERR_CODE=0

dev_makeswap=-1
dev_start=0
dev_end=-1
module_load=-1
sys_control=-1

. ./zram_lib.sh

dev_num=1
zram_max_streams="2"
zram_sizes="268435456" # 256M
backing_file=""
loopdev=""
zswap_enabled_file="/sys/module/zswap/parameters/enabled"
zswap_enabled_old=""

fail()
{
	echo "FAIL $*"
	ERR_CODE=1
}

skip()
{
	echo "$TCID : [SKIP] $*"
	exit $ksft_skip
}

cleanup()
{
	if [ "$dev_makeswap" -ge 0 ]; then
		swapoff /dev/zram$dev_makeswap 2>/dev/null || true
		dev_makeswap=-1
	fi

	if [ "$dev_end" -ge "$dev_start" ]; then
		echo 1 > /sys/block/zram${dev_start}/reset 2>/dev/null || true
	fi

	if [ -n "$loopdev" ]; then
		losetup -d "$loopdev" 2>/dev/null || true
	fi

	if [ -n "$backing_file" ]; then
		rm -f "$backing_file"
	fi

	if [ -n "$zswap_enabled_old" ]; then
		echo "$zswap_enabled_old" > "$zswap_enabled_file" 2>/dev/null ||
			true
	fi

	if [ "$sys_control" -eq 1 ]; then
		echo $dev_start > /sys/class/zram-control/hot_remove 2>/dev/null || true
	fi

	if [ "$module_load" -eq 1 ]; then
		rmmod zram > /dev/null 2>&1 || true
	fi
}

get_stat()
{
	local file="$1"
	local field="$2"

	awk -v field="$field" '{ print $field }' "$file"
}

require_writeback()
{
	local sysfs="/sys/block/zram${dev_start}"

	[ -e "$sysfs/backing_dev" ] || skip "zram backing_dev is unavailable"
	[ -e "$sysfs/writeback" ] || skip "zram writeback is unavailable"
	[ -e "$sysfs/idle" ] || skip "zram idle tracking is unavailable"
	[ -e "$sysfs/bd_stat" ] || skip "zram bd_stat is unavailable"
	[ -x "./zram_pageout" ] || skip "zram_pageout helper is missing"
	command -v losetup >/dev/null 2>&1 || skip "losetup is unavailable"
	command -v mkswap >/dev/null 2>&1 || skip "mkswap is unavailable"
	command -v swapon >/dev/null 2>&1 || skip "swapon is unavailable"
}

disable_zswap()
{
	if [ ! -e "$zswap_enabled_file" ]; then
		return 0
	fi

	zswap_enabled_old=$(cat "$zswap_enabled_file")
	echo N > "$zswap_enabled_file" || skip "failed to disable zswap"
}

setup_backing_dev()
{
	local sysfs="/sys/block/zram${dev_start}"

	backing_file=$(mktemp "${TMPDIR:-/tmp}/zram-backing.XXXXXX") ||
		skip "failed to create backing file"
	truncate -s 256M "$backing_file" || skip "failed to size backing file"

	loopdev=$(losetup -fP --show "$backing_file") ||
		skip "failed to allocate loop device"

	echo "$loopdev" > "$sysfs/backing_dev" ||
		skip "failed to set backing_dev"
}

zram_makeswap_high_prio()
{
	mkswap /dev/zram$dev_start > err.log 2>&1 || {
		cat err.log
		fail "mkswap /dev/zram$dev_start failed"
		return
	}

	swapon -p 32767 /dev/zram$dev_start > err.log 2>&1 || {
		cat err.log
		fail "swapon /dev/zram$dev_start failed"
		return
	}

	dev_makeswap=$dev_start
}

run_writeback_test()
{
	local sysfs="/sys/block/zram${dev_start}"
	local bd_stat="$sysfs/bd_stat"
	local bd_writes_before
	local bd_writes_after
	local bd_reads_before
	local bd_reads_after
	local pswpin_before
	local pswpin_after
	local pswpout_before
	local pswpout_after

	bd_writes_before=$(get_stat "$bd_stat" 3)
	bd_reads_before=$(get_stat "$bd_stat" 2)
	pswpin_before=$(awk '/^pswpin / { print $2 }' /proc/vmstat)
	pswpout_before=$(awk '/^pswpout / { print $2 }' /proc/vmstat)

	./zram_pageout 64 "$sysfs" || fail "pageout/writeback helper failed"

	bd_writes_after=$(get_stat "$bd_stat" 3)
	bd_reads_after=$(get_stat "$bd_stat" 2)
	pswpin_after=$(awk '/^pswpin / { print $2 }' /proc/vmstat)
	pswpout_after=$(awk '/^pswpout / { print $2 }' /proc/vmstat)

	[ "$bd_writes_after" -gt "$bd_writes_before" ] ||
		fail "backing device write count did not increase"
	[ "$bd_reads_after" -gt "$bd_reads_before" ] ||
		fail "backing device read count did not increase"
	[ "$pswpout_after" -gt "$pswpout_before" ] ||
		fail "pswpout did not increase"
	[ "$pswpin_after" -gt "$pswpin_before" ] ||
		fail "pswpin did not increase"
}

trap cleanup EXIT

check_prereqs
zram_load
require_writeback
disable_zswap
setup_backing_dev
zram_max_streams
zram_set_disksizes
zram_makeswap_high_prio
run_writeback_test

if [ $ERR_CODE -ne 0 ]; then
	echo "$TCID : [FAIL]"
	exit 1
else
	echo "$TCID : [PASS]"
fi

#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Functional tests for zram swap_ops path: swap integrity, slot_free_notify,
# writeback + swap-in, and repeated swapoff cycles.

TCID="zram03"
ERR_CODE=0

. ./zram_lib.sh

INT_BIN="${INT_BIN:-./zram_swap_int}"
BACKING_IMG="${BACKING_IMG:-/var/tmp/zram03-backing.img}"
BACKING_SIZE="${BACKING_SIZE:-512M}"
LOOP_SWAP_IMG="${LOOP_SWAP_IMG:-/var/tmp/zram03-loop-swap.img}"

fail()
{
	echo "FAIL: $*"
	ERR_CODE=1
}

ok()
{
	echo "$*: OK"
}

zram_dev_path()
{
	echo "/dev/zram${dev_start}"
}

zram_sys_path()
{
	echo "/sys/block/zram${dev_start}"
}

zram_read_notify_free()
{
	awk '{print $4}' "$(zram_sys_path)/io_stat"
}

zram_read_mm_compr()
{
	awk '{print $2}' "$(zram_sys_path)/mm_stat"
}

zram_read_miss_free()
{
	awk 'NR==2 {print $2}' "$(zram_sys_path)/debug_stat"
}

zram_has_writeback()
{
	[ -e "$(zram_sys_path)/writeback" ] && \
	[ -e "$(zram_sys_path)/backing_dev" ]
}

zram_setup_swap_plain()
{
	local sys="$(zram_sys_path)"
	local dev="$(zram_dev_path)"

	swapoff "$dev" 2>/dev/null || true
	echo 1 > "$sys/reset"

	zram_compress_alg
	zram_set_disksizes
	zram_set_memlimit

	mkswap "$dev" > err.log 2>&1 || {
		cat err.log
		fail "mkswap $dev"
		return 1
	}
	swapon -p 32767 "$dev" > err.log 2>&1 || {
		cat err.log
		fail "swapon $dev"
		return 1
	}
	dev_makeswap=$dev_start
	ok "zram swap configured on $dev"
}

zram_setup_swap_writeback()
{
	local sys="$(zram_sys_path)"
	local dev="$(zram_dev_path)"
	local loopdev

	swapoff "$dev" 2>/dev/null || true
	echo 1 > "$sys/reset"

	truncate -s "$BACKING_SIZE" "$BACKING_IMG"
	losetup -j "$BACKING_IMG" | cut -d: -f1 | while read -r d; do
		[ -n "$d" ] && losetup -d "$d" || true
	done
	loopdev=$(losetup -fP --show "$BACKING_IMG")

	echo "$loopdev" > "$sys/backing_dev"
	zram_compress_alg
	echo "$zram_sizes" > "$sys/disksize"
	zram_set_memlimit

	mkswap "$dev" > err.log 2>&1 || {
		cat err.log
		fail "mkswap $dev (writeback)"
		return 1
	}
	swapon -p 32767 "$dev" > err.log 2>&1 || {
		cat err.log
		fail "swapon $dev (writeback)"
		return 1
	}
	dev_makeswap=$dev_start
	ok "zram writeback swap on $dev backing $loopdev"
}

test_swap_integrity_zspool()
{
	echo "== test_swap_integrity_zspool =="

	if [ ! -x "$INT_BIN" ]; then
		fail "missing $INT_BIN (run make)"
		return
	fi

	zram_setup_swap_plain || return

	if "$INT_BIN" --mb 64 --stride 1 --seed 42 \
		--zram-sys "$(zram_sys_path)" --expect-no-bd; then
		ok "zspool swap integrity"
	else
		fail "zspool swap integrity"
	fi

	zram_swapoff
}

test_slot_free_notify()
{
	echo "== test_slot_free_notify =="

	zram_setup_swap_plain || return

	if [ ! -x "$INT_BIN" ]; then
		fail "missing $INT_BIN"
		return
	fi

	local notify_before compr_before miss_before

	notify_before=$(zram_read_notify_free)
	compr_before=$(zram_read_mm_compr)
	miss_before=$(zram_read_miss_free)

	"$INT_BIN" --mb 32 --stride 4 --seed 7 \
		--zram-sys "$(zram_sys_path)" --expect-no-bd || {
		fail "swap fill for slot_free test"
		return
	}

	local notify_after compr_after

	notify_after=$(zram_read_notify_free)
	compr_after=$(zram_read_mm_compr)

	if [ "$notify_after" -le "$notify_before" ]; then
		fail "notify_free did not increase during swap ($notify_before -> $notify_after)"
	else
		ok "notify_free increased during swap ($notify_before -> $notify_after)"
	fi

	if [ "$compr_after" -le 0 ]; then
		fail "mm_stat compr_data zero after swap ($compr_after)"
	else
		ok "mm_stat compr_data in use after swap ($compr_after)"
	fi

	zram_swapoff
	echo 1 > "$(zram_sys_path)/reset"

	if [ "$(zram_read_mm_compr)" -ne 0 ]; then
		fail "mm_stat compr_data not zero after reset"
	else
		ok "mm_stat cleared after reset"
	fi

	if [ -n "$miss_before" ] && [ "$miss_before" -gt 1000 ]; then
		fail "miss_free suspiciously high ($miss_before)"
	else
		ok "miss_free reasonable ($miss_before)"
	fi
}

test_writeback_integrity()
{
	echo "== test_writeback_integrity =="

	if ! zram_has_writeback; then
		echo "skip: CONFIG_ZRAM_WRITEBACK not available"
		return
	fi

	if [ ! -x "$INT_BIN" ]; then
		fail "missing $INT_BIN"
		return
	fi

	zram_setup_swap_writeback || return

	if "$INT_BIN" --mb 128 --stride 4 --seed 99 --writeback \
		--zram-sys "$(zram_sys_path)" --expect-bd-writes; then
		ok "writeback swap integrity"
	else
		fail "writeback swap integrity"
	fi

	zram_swapoff
	losetup -j "$BACKING_IMG" | cut -d: -f1 | while read -r d; do
		[ -n "$d" ] && losetup -d "$d" || true
	done
	rm -f "$BACKING_IMG"
}

test_swapoff_cycles()
{
	echo "== test_swapoff_cycles =="

	local cycle

	for cycle in 1 2 3; do
		zram_setup_swap_plain || return
		if [ ! -x "$INT_BIN" ]; then
			fail "missing $INT_BIN"
			return
		fi
		"$INT_BIN" --mb 16 --stride 8 --seed "$cycle" \
			--zram-sys "$(zram_sys_path)" --expect-no-bd || {
			fail "swapoff cycle $cycle integrity"
			return
		}
		zram_swapoff
		echo 1 > "$(zram_sys_path)/reset"
	done

	ok "swapoff cycles"
}

# Test will create one zram device for all subtests.
dev_num=1
zram_max_streams="2"
zram_sizes="536870912"   # 512 MB
zram_mem_limits="512M"
zram_algs="lzo-rle"

check_prereqs

if [ ! -x "$INT_BIN" ]; then
	make -s zram_swap_int 2>/dev/null || true
fi

zram_load
zram_max_streams

test_swap_integrity_zspool
test_slot_free_notify
test_writeback_integrity
test_swapoff_cycles

zram_cleanup
rm -f "$BACKING_IMG" "$LOOP_SWAP_IMG"

if [ $ERR_CODE -ne 0 ]; then
	echo "$TCID : [FAIL]"
else
	echo "$TCID : [PASS]"
fi

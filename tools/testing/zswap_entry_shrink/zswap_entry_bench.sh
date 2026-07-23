#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# zswap_entry_bench.sh - measure the effect of shrinking struct zswap_entry
# (pool pointer -> u16 pool_idx).
#
# What it measures
# ----------------
# 1. Memory: per-object size of the "zswap_entry" slab cache under a fixed,
#    reproducible zswap population.  This is the direct signal for this change:
#    struct zswap_entry shrinks 56 -> 48 bytes on x86_64.  Size is read from
#    /sys/kernel/slab/zswap_entry/object_size (works even when SLUB merges the
#    cache; /proc/slabinfo then only shows the merged :00xxxx name).
#
# 2. CPU: wall-clock cost of a swap-in-heavy workload that forces zswap
#    decompress (which now goes through zswap_entry_pool() -> xa_load()
#    instead of a bare pointer deref).  Reported as median of N runs so you
#    can compare a "before" build vs an "after" build.
#
# IMPORTANT: this script does not rebuild the kernel.  Run it once on a kernel
# built WITHOUT this patch, save the output, then again on a kernel WITH it,
# and diff the two reports.  The memory numbers are decided at compile time by
# sizeof(struct zswap_entry); the slab objsize column proves what actually
# changed.
#
# Must run as root.  Uses a file swap device it creates and removes.

set -u

# ---- tunables (override via env) -------------------------------------------
: "${ZS_MB:=512}"          # amount of compressible data to push into zswap
: "${ZS_RUNS:=5}"          # CPU benchmark repetitions
: "${ZS_COMPRESSOR:=lzo}"  # zswap compressor to pin (comparable across runs)
: "${ZS_MAX_POOL:=50}"     # zswap max_pool_percent
: "${ZS_WORKDIR:=/tmp/zswap_entry_bench}"
: "${ZS_SLAB:=/sys/kernel/slab/zswap_entry}"

die()  { echo "ERROR: $*" >&2; exit 1; }
note() { echo "[*] $*"; }

[ "$(id -u)" -eq 0 ] || die "must run as root"
[ -d /sys/module/zswap ] || die "zswap not present (CONFIG_ZSWAP?)"

# ---- helpers ---------------------------------------------------------------

# First field only: sysfs slab counters may append " N0=..." on NUMA.
slab_sysfs_num() {
	local f="$1"
	[ -r "$f" ] || return 1
	awk '{print $1; exit}' "$f"
}

# object_size is per-cache and stays correct under SLUB merge.
slab_objsize() { slab_sysfs_num "$ZS_SLAB/object_size"; }

# When the cache is merged, objects/slabs cover every alias on the merged
# slab — do not treat them as zswap-only.  aliases>0 (or a :00xxxx link
# target) means merged.
slab_merged() {
	local aliases target
	aliases=$(slab_sysfs_num "$ZS_SLAB/aliases" 2>/dev/null) || aliases=0
	target=$(readlink -f "$ZS_SLAB" 2>/dev/null || echo "$ZS_SLAB")
	[ "${aliases:-0}" -gt 0 ] || [[ "$(basename "$target")" == :* ]]
}

save_zswap_state() {
	OLD_ENABLED=$(cat /sys/module/zswap/parameters/enabled 2>/dev/null)
	OLD_COMP=$(cat /sys/module/zswap/parameters/compressor 2>/dev/null)
	OLD_MAXP=$(cat /sys/module/zswap/parameters/max_pool_percent 2>/dev/null)
}
restore_zswap_state() {
	[ -n "${OLD_COMP:-}" ]    && echo "$OLD_COMP"    > /sys/module/zswap/parameters/compressor 2>/dev/null
	[ -n "${OLD_MAXP:-}" ]    && echo "$OLD_MAXP"    > /sys/module/zswap/parameters/max_pool_percent 2>/dev/null
	[ -n "${OLD_ENABLED:-}" ] && echo "$OLD_ENABLED" > /sys/module/zswap/parameters/enabled 2>/dev/null
}

SWAPFILE=""
cleanup() {
	[ -n "$SWAPFILE" ] && swapoff "$SWAPFILE" 2>/dev/null
	[ -n "$SWAPFILE" ] && rm -f "$SWAPFILE" 2>/dev/null
	restore_zswap_state
	rm -rf "$ZS_WORKDIR" 2>/dev/null
}
trap cleanup EXIT INT TERM

# ---- setup -----------------------------------------------------------------

mkdir -p "$ZS_WORKDIR" || die "mkdir $ZS_WORKDIR"
save_zswap_state

note "configuring zswap: compressor=$ZS_COMPRESSOR max_pool_percent=$ZS_MAX_POOL"
echo "$ZS_COMPRESSOR" > /sys/module/zswap/parameters/compressor 2>/dev/null || \
	note "could not set compressor (continuing with $OLD_COMP)"
echo "$ZS_MAX_POOL"   > /sys/module/zswap/parameters/max_pool_percent 2>/dev/null || true
echo 1                > /sys/module/zswap/parameters/enabled 2>/dev/null || true

# A dedicated swap file keeps this workload off the system swap.
SWAPFILE="$ZS_WORKDIR/swapfile"
note "creating ${ZS_MB}MB swapfile at $SWAPFILE"
# Size the swap generously (2x payload) so stores land in zswap, not disk.
dd if=/dev/zero of="$SWAPFILE" bs=1M count=$((ZS_MB*2)) status=none || die "dd swapfile"
chmod 600 "$SWAPFILE"
mkswap "$SWAPFILE" >/dev/null 2>&1 || die "mkswap"
swapon "$SWAPFILE" || die "swapon (need swap accounting / priority room)"

# ---- payload generator -----------------------------------------------------
# Anonymous, highly compressible memory (repeating pattern) that we push out to
# swap by memory pressure.  We generate it in a child that mmaps, fills, then
# forces reclaim of its own pages by touching a large cgroup-limited region.
# To stay dependency-free we use a small C helper compiled on the fly.

HELPER_SRC="$ZS_WORKDIR/payload.c"
HELPER_BIN="$ZS_WORKDIR/payload"
cat > "$HELPER_SRC" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>

/* argv[1]=MB to allocate, argv[2]=passes to touch (drives swap-in/out) */
int main(int argc, char **argv)
{
	size_t mb = argc > 1 ? strtoul(argv[1], 0, 10) : 256;
	int passes = argc > 2 ? atoi(argv[2]) : 1;
	size_t n = mb << 20;
	char *p = mmap(NULL, n, PROT_READ|PROT_WRITE,
		       MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) { perror("mmap"); return 2; }

	/* Fill with a compressible pattern (not zero-filled, which zswap
	 * may special-case as same-filled and never allocate an entry for). */
	for (size_t i = 0; i < n; i += 4096)
		memset(p + i, (int)((i >> 12) & 0x3f) + 1, 4096);

	struct timespec a, b;
	clock_gettime(CLOCK_MONOTONIC, &a);
	/* Re-touch every page 'passes' times: forces decompress on swap-in. */
	volatile char sink = 0;
	for (int k = 0; k < passes; k++)
		for (size_t i = 0; i < n; i += 4096)
			sink ^= p[i];
	clock_gettime(CLOCK_MONOTONIC, &b);
	(void)sink;

	double ms = (b.tv_sec - a.tv_sec) * 1e3 +
		    (b.tv_nsec - a.tv_nsec) / 1e6;
	fprintf(stderr, "touch_ms=%.1f\n", ms);

	/* Keep pages resident briefly so sysfs slab can be sampled by parent. */
	if (getenv("ZS_HOLD")) pause();
	munmap(p, n);
	return 0;
}
EOF
cc -O2 -o "$HELPER_BIN" "$HELPER_SRC" || die "compile payload helper"

[ -d "$ZS_SLAB" ] || note "note: $ZS_SLAB not present yet (appears after first zswap store)"

# ---- 1) MEMORY measurement -------------------------------------------------
echo
echo "================ MEMORY ================"
# Populate zswap: run the payload holding its pages, under memory pressure via
# a memory cgroup so pages actually get pushed to swap/zswap.
CG=/sys/fs/cgroup/zswap_entry_bench
# cgroup.controllers is a file on cgroup v2, not a directory.
if [ -f /sys/fs/cgroup/cgroup.controllers ] &&
   grep -qw memory /sys/fs/cgroup/cgroup.controllers; then
	mkdir -p "$CG" 2>/dev/null
	# Limit well below payload so the kernel must swap the excess to zswap.
	echo $(( (ZS_MB/2) * 1024 * 1024 )) > "$CG/memory.max" 2>/dev/null || true
	echo $(( (ZS_MB/2) * 1024 * 1024 )) > "$CG/memory.high" 2>/dev/null || true
	if [ -f "$CG/memory.max" ]; then
		CG_OK=1
	else
		CG_OK=0
		note "cgroup v2 memory controller present but cgroup setup failed"
	fi
else
	CG_OK=0
	note "cgroup v2 memory unavailable; relying on global pressure (results noisier)"
fi

note "populating zswap with ${ZS_MB}MB compressible data ..."
ZS_HOLD=1 "$HELPER_BIN" "$ZS_MB" 1 &
PAYLOAD_PID=$!
[ "$CG_OK" = 1 ] && echo "$PAYLOAD_PID" > "$CG/cgroup.procs" 2>/dev/null

# Give the kernel a moment to push pages into zswap.
sleep 5

OBJSIZE=$(slab_objsize || true)
POOL_TOT=$(cat /sys/kernel/debug/zswap/pool_total_size 2>/dev/null || echo "n/a")
STORED=$(cat /sys/kernel/debug/zswap/stored_pages 2>/dev/null || echo "n/a")
SLAB_TARGET=$(readlink -f "$ZS_SLAB" 2>/dev/null || echo "n/a")
ALIASES=$(slab_sysfs_num "$ZS_SLAB/aliases" 2>/dev/null || echo "n/a")

echo "zswap_entry slab (via $ZS_SLAB):"
echo "  objsize (bytes/entry) : ${OBJSIZE:-n/a}   <-- 48 with patch, 56 without (x86_64)"
echo "  sysfs path            : $SLAB_TARGET"
echo "  aliases (merge count) : ${ALIASES:-n/a}"
if slab_merged 2>/dev/null; then
	note "SLUB-merged cache: objects/slabs in sysfs are shared; footprint uses stored_pages"
fi
echo "zswap debug counters:"
echo "  stored_pages          : $STORED"
echo "  pool_total_size (B)   : $POOL_TOT"
# zswap-only metadata: stored_pages is exact; merged slab object counts are not.
if [ -n "${OBJSIZE:-}" ] && [ -n "${STORED:-}" ] && [ "$STORED" != "n/a" ]; then
	BYTES=$(( OBJSIZE * STORED ))
	echo "  live entry footprint  : ${STORED} × ${OBJSIZE} = ${BYTES} B ($(( BYTES / 1024 )) KiB)"
fi
echo
echo "Interpretation:"
echo "  * 'objsize' is decided at compile time by sizeof(struct zswap_entry)."
echo "    Compare this single number between a before/after build: it should"
echo "    read 56 (before) and 48 (after) on x86_64. Everything else scales."
echo "  * 'live entry footprint' = the metadata overhead this patch shrinks."
echo "    At objsize 48 vs 56, that is an 8/56 ~= 14% cut on this line,"
echo "    independent of the compressed data itself (pool_total_size)."

kill "$PAYLOAD_PID" 2>/dev/null
wait "$PAYLOAD_PID" 2>/dev/null
[ "$CG_OK" = 1 ] && rmdir "$CG" 2>/dev/null

# ---- 2) CPU measurement ----------------------------------------------------
echo
echo "================ CPU ==================="
note "swap-in benchmark: $ZS_RUNS runs, each touches ${ZS_MB}MB x2 passes"
declare -a TIMES
for r in $(seq 1 "$ZS_RUNS"); do
	# 2 passes => each page is stored then swapped back in => decompress path,
	# where zswap_entry_pool()/xa_load() now sits.
	MS=$("$HELPER_BIN" "$ZS_MB" 2 2>&1 >/dev/null | sed -n 's/^touch_ms=//p')
	echo "  run $r: ${MS} ms"
	TIMES+=("$MS")
done

# median
MEDIAN=$(printf '%s\n' "${TIMES[@]}" | sort -n | awk '{a[NR]=$1} END{
	if (NR%2) print a[(NR+1)/2]; else printf "%.1f", (a[NR/2]+a[NR/2+1])/2 }')
echo "  median: ${MEDIAN} ms"
echo
echo "Interpretation:"
echo "  * Compare median before vs after. The extra cost per swap-in is one"
echo "    xa_load() (a small-index xarray lookup) instead of a pointer deref;"
echo "    on non-PREEMPT kernels rcu_read_lock/unlock inside xa_load are"
echo "    near-free. Expect the delta to be within run-to-run noise, dwarfed"
echo "    by (de)compression itself. If the median regression exceeds a few"
echo "    percent, re-run with more ZS_RUNS to rule out noise."

echo
note "done"

#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# zcomp_sw_path_bench.sh - compare zswap software (lib/zcomp) vs crypto paths,
# and the shared zcomp backends used by zram.
#
# After the zswap_use_zcomp series:
#   SW / zcomp path : lzo, lzo-rle, lz4hc
#   crypto path     : lz4, zstd, deflate, 842
# Before that series, zswap always used crypto acomp for every algorithm.
#
# What it measures
# ----------------
# 1) zswap store (pageout into pool) and load (swap-in decompress) throughput
#    for each available compressor, under a cgroup memory.reclaim push.
# 2) zram compress write throughput via a hot-added disposable zram device
#    (same algorithm names / same lib/zcomp backends after the series).
# 3) A short "zswap cache vs zram-only" contrast on the system zram swap:
#    hot-page swap-in is much cheaper when zswap absorbs the working set.
#
# Usage (root):
#   sudo bash test-logs/zcomp_sw_path_bench.sh
#   sudo MEM_MB=256 RUNS=3 bash test-logs/zcomp_sw_path_bench.sh
#
# Compare a baseline boot (pre-series) vs a patched boot (post-series) on the
# same machine; the SW-path algs (esp. lzo) are where store/load MB/s should
# improve once zswap routes through lib/zcomp.

set -euo pipefail

BASE="$(cd "$(dirname "$0")/.." && pwd)"
MEM_MB=${MEM_MB:-256}
RUNS=${RUNS:-3}
WORKDIR=${WORKDIR:-/tmp/zcomp_sw_path_bench}
LOG="$BASE/test-logs/zcomp_sw_path_bench_$(date +%Y%m%d_%H%M%S).log"
ZSWAP=/sys/module/zswap/parameters
CG=/sys/fs/cgroup/zcomp_sw_path_bench
ZRAM_ID=""
ZRAM_DEV=""

# Default matrix: SW-path first, then crypto-path. Override with ALGS="lzo lz4".
ALGS=${ALGS:-"lzo lzo-rle lz4hc lz4 zstd"}

exec > >(tee -a "$LOG") 2>&1

log()  { echo "[$(date '+%F %T')] $*"; }
die()  { echo "ERROR: $*" >&2; exit 1; }
note() { echo "[*] $*"; }

[ "$(id -u)" -eq 0 ] || die "run as root: sudo bash $0"
[ -d /sys/module/zswap ] || die "zswap missing (CONFIG_ZSWAP?)"
[ -f /sys/fs/cgroup/cgroup.controllers ] || die "need cgroup v2"
grep -qw memory /sys/fs/cgroup/cgroup.controllers || die "memory controller missing"

path_for_alg() {
	case "$1" in
	lzo|lzo-rle|lz4hc) echo "SW/zcomp (post-series)" ;;
	lz4|zstd|deflate|842) echo "crypto" ;;
	*) echo "unknown" ;;
	esac
}

mkdir -p "$WORKDIR"
OLD_ENABLED=$(cat "$ZSWAP/enabled")
OLD_COMP=$(cat "$ZSWAP/compressor")
OLD_MAXP=$(cat "$ZSWAP/max_pool_percent")
OLD_SHRINK=$(cat "$ZSWAP/shrinker_enabled" 2>/dev/null || echo "")

cleanup() {
	[ -n "${PAYLOAD_PID:-}" ] && kill "$PAYLOAD_PID" 2>/dev/null || true
	[ -n "${SWAPFILE:-}" ] && swapoff "$SWAPFILE" 2>/dev/null || true
	[ -n "${SWAPFILE:-}" ] && rm -f "$SWAPFILE" 2>/dev/null || true
	if [ -n "$ZRAM_DEV" ] && [ -b "$ZRAM_DEV" ]; then
		echo 1 > "/sys/block/$(basename "$ZRAM_DEV")/reset" 2>/dev/null || true
	fi
	if [ -n "$ZRAM_ID" ] && [ -w /sys/class/zram-control/hot_remove ]; then
		echo "$ZRAM_ID" > /sys/class/zram-control/hot_remove 2>/dev/null || true
	fi
	echo "$OLD_COMP" > "$ZSWAP/compressor" 2>/dev/null || true
	echo "$OLD_MAXP" > "$ZSWAP/max_pool_percent" 2>/dev/null || true
	[ -n "$OLD_SHRINK" ] && echo "$OLD_SHRINK" > "$ZSWAP/shrinker_enabled" 2>/dev/null || true
	echo "$OLD_ENABLED" > "$ZSWAP/enabled" 2>/dev/null || true
	rmdir "$CG" 2>/dev/null || true
	rm -rf "$WORKDIR" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

log "=== zcomp SW-path / zram shared-backend bench ==="
log "kernel: $(uname -r)"
log "cmdline: $(cat /proc/cmdline)"
log "log: $LOG"
log "MEM_MB=$MEM_MB RUNS=$RUNS ALGS=$ALGS"
echo
note "current zswap: enabled=$(cat $ZSWAP/enabled) compressor=$(cat $ZSWAP/compressor) shrinker=$(cat $ZSWAP/shrinker_enabled 2>/dev/null || echo n/a)"
note "system swap:"
swapon --show || true
echo

# Heuristic: patched series lives under zswap_use_zcomp*; running kernel name
# usually will not match until reboot. Warn loudly.
if ! uname -r | grep -qiE 'zcomp|zswap_use_zcomp'; then
	note "WARN: running kernel looks unrelated to zswap_use_zcomp series."
	note "      Numbers below are still useful as a crypto-path baseline;"
	note "      reboot into the built zswap_use_zcomp kernel to see SW-path gains."
	echo
fi

# ---- helpers ---------------------------------------------------------------
HELPER="$WORKDIR/payload"
cat > "$WORKDIR/payload.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>

int main(int argc, char **argv)
{
	size_t mb = argc > 1 ? strtoul(argv[1], 0, 10) : 256;
	int passes = argc > 2 ? atoi(argv[2]) : 1;
	size_t n = mb << 20;
	char *p = mmap(NULL, n, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		perror("mmap");
		return 2;
	}

	/* Per-page compressible but NOT same-filled (zram special-cases those). */
	for (size_t i = 0; i < n; i += 4096) {
		unsigned char base = (unsigned char)((i >> 12) & 0x3f) + 1;
		for (size_t j = 0; j < 4096; j++)
			p[i + j] = (char)(base + (j & 0x7));
	}

	/* Hold until parent reclaims / signals. */
	if (getenv("ZS_HOLD"))
		pause();

	struct timespec a, b;
	clock_gettime(CLOCK_MONOTONIC, &a);
	volatile char sink = 0;
	for (int k = 0; k < passes; k++)
		for (size_t i = 0; i < n; i += 4096)
			sink ^= p[i];
	clock_gettime(CLOCK_MONOTONIC, &b);
	(void)sink;

	double ms = (b.tv_sec - a.tv_sec) * 1e3 +
		    (b.tv_nsec - a.tv_nsec) / 1e6;
	fprintf(stderr, "touch_ms=%.3f\n", ms);
	munmap(p, n);
	return 0;
}
EOF
cc -O2 -o "$HELPER" "$WORKDIR/payload.c" || die "compile payload"

vm() { awk -v k="$1" '$1==k {print $2; exit}' /proc/vmstat; }
zswap_stored() { cat /sys/kernel/debug/zswap/stored_pages 2>/dev/null || echo 0; }
zswap_pool() { cat /sys/kernel/debug/zswap/pool_total_size 2>/dev/null || echo 0; }

setup_cg() {
	rmdir "$CG" 2>/dev/null || true
	mkdir -p "$CG"
	# Ensure memory is delegated if parent allows it.
	echo +memory > /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null || true
	echo max > "$CG/memory.max"
	echo max > "$CG/memory.zswap.max" 2>/dev/null || true
	echo 1 > "$CG/memory.zswap.writeback" 2>/dev/null || true
}

median() {
	printf '%s\n' "$@" | awk 'NF{a[++n]=$1} END{
		if (!n) { print "nan"; exit }
		asort(a)
		if (n%2) print a[(n+1)/2]
		else printf "%.3f", (a[n/2]+a[n/2+1])/2
	}'
}

try_set_compressor() {
	local alg=$1
	if ! echo "$alg" > "$ZSWAP/compressor" 2>/dev/null; then
		return 1
	fi
	# Confirm it stuck (param_set may reject asynchronously on some kernels).
	local now
	now=$(cat "$ZSWAP/compressor")
	[ "$now" = "$alg" ]
}

# ---- dedicated swapfile so zswap stores do not fight system zram priority ---
SWAPFILE="$WORKDIR/swapfile"
note "creating $((MEM_MB * 3))MB dedicated swapfile (prio 10)"
dd if=/dev/zero of="$SWAPFILE" bs=1M count=$((MEM_MB * 3)) status=none
chmod 600 "$SWAPFILE"
mkswap "$SWAPFILE" >/dev/null
# Lower than zram0 (100) so we must temporarily drop zram0, otherwise reclaim
# prefers zram and may bypass the measurement swapfile.
ZRAM0_WAS=0
if swapon --show --noheadings | awk '{print $1}' | grep -qx /dev/zram0; then
	ZRAM0_WAS=1
	note "temporarily swapoff /dev/zram0 for isolated zswap measurement"
	swapoff /dev/zram0
fi
swapon -p 10 "$SWAPFILE"

echo Y > "$ZSWAP/enabled"
echo 50 > "$ZSWAP/max_pool_percent"
# Shrinker off during store measurement so pages stay in the pool.
echo 0 > "$ZSWAP/shrinker_enabled" 2>/dev/null || true

# ============================================================================
# 1) zswap multi-compressor matrix
# ============================================================================
echo
echo "================ ZSWAP store / load =================="
printf '%-10s %-24s %10s %10s %10s %12s %10s\n' \
	"algo" "expected_path" "store_ms" "store_MBs" "load_ms" "stored_pages" "pool_B"
printf '%s\n' "---------------------------------------------------------------------------------------------"

declare -A ZSWAP_STORE_MS ZSWAP_LOAD_MS ZSWAP_STORED ZSWAP_POOL

for alg in $ALGS; do
	path=$(path_for_alg "$alg")
	if ! try_set_compressor "$alg"; then
		printf '%-10s %-24s %10s %10s %10s %12s %10s\n' \
			"$alg" "$path" "SKIP" "-" "-" "-" "-"
		continue
	fi

	store_times=()
	load_times=()
	stored_last=0
	pool_last=0

	for ((r = 1; r <= RUNS; r++)); do
		setup_cg
		# Drop any previous pool pressure gently.
		echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true

		before_zswpout=$(vm zswpout)
		before_stored=$(zswap_stored)

		ZS_HOLD=1 "$HELPER" "$MEM_MB" 1 &
		PAYLOAD_PID=$!
		# Move into cgroup after start.
		echo "$PAYLOAD_PID" > "$CG/cgroup.procs" 2>/dev/null || true
		# Wait until pages are dirtied.
		sleep 0.3

		t0=$(date +%s%N)
		# Push the whole working set toward swap/zswap.
		reclaim=$((MEM_MB * 1024 * 1024))
		# memory.reclaim can EAGAIN; chunk and tolerate failures.
		left=$reclaim
		while [ "$left" -gt 0 ]; do
			chunk=$((left > 32*1024*1024 ? 32*1024*1024 : left))
			echo "$chunk" > "$CG/memory.reclaim" 2>/dev/null || true
			left=$((left - chunk))
		done
		# Extra nudge if still little stored.
		for _ in 1 2 3 4 5; do
			cur=$(zswap_stored)
			[ "$cur" -gt $((MEM_MB * 1024 / 8)) ] && break
			echo $((16 * 1024 * 1024)) > "$CG/memory.reclaim" 2>/dev/null || true
			sleep 0.1
		done
		t1=$(date +%s%N)
		store_ms=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.3f", (b-a)/1e6}')

		stored_last=$(zswap_stored)
		pool_last=$(zswap_pool)
		after_zswpout=$(vm zswpout)

		# Release hold: child exits pause via SIGTERM then we re-run load
		# measurement with a fresh populate+touch cycle for clean load_ms.
		kill -CONT "$PAYLOAD_PID" 2>/dev/null || true
		kill "$PAYLOAD_PID" 2>/dev/null || true
		wait "$PAYLOAD_PID" 2>/dev/null || true
		PAYLOAD_PID=""
		rmdir "$CG" 2>/dev/null || true

		# If almost nothing landed in zswap, mark store invalid for this run.
		delta_zswpout=$((after_zswpout - before_zswpout))
		if [ "$stored_last" -lt 100 ] && [ "$delta_zswpout" -lt 100 ]; then
			store_times+=("nan")
			load_times+=("nan")
			continue
		fi
		store_times+=("$store_ms")

		# Load path: populate again, hold in zswap, then touch (decompress).
		setup_cg
		ZS_HOLD=1 "$HELPER" "$MEM_MB" 1 &
		PAYLOAD_PID=$!
		echo "$PAYLOAD_PID" > "$CG/cgroup.procs" 2>/dev/null || true
		sleep 0.3
		left=$reclaim
		while [ "$left" -gt 0 ]; do
			chunk=$((left > 32*1024*1024 ? 32*1024*1024 : left))
			echo "$chunk" > "$CG/memory.reclaim" 2>/dev/null || true
			left=$((left - chunk))
		done
		for _ in 1 2 3 4 5; do
			cur=$(zswap_stored)
			[ "$cur" -gt $((MEM_MB * 1024 / 8)) ] && break
			echo $((16 * 1024 * 1024)) > "$CG/memory.reclaim" 2>/dev/null || true
			sleep 0.1
		done

		# Signal child to leave pause and touch once... but payload only
		# touches after pause returns. Send SIGCONT after replacing the
		# hold with a one-shot touch by killing and using a non-hold run
		# that fault-ins from zswap: simpler approach below.
		kill "$PAYLOAD_PID" 2>/dev/null || true
		wait "$PAYLOAD_PID" 2>/dev/null || true
		PAYLOAD_PID=""

		# Fresh populate + fault-in inside the cgroup (decompress path).
		setup_cg
		load_ms=$(
			MEM_MB="$MEM_MB" CG="$CG" python3 - <<'PY'
import os, time, subprocess, mmap

mb = int(os.environ["MEM_MB"])
cg = os.environ["CG"]
n = mb << 20
PAGE = 4096

def w(path, val):
    subprocess.run(["sh", "-c", f"echo {val} > {path}"], check=False)

def reclaim(total):
    left = total
    while left > 0:
        chunk = min(left, 32 << 20)
        w(f"{cg}/memory.reclaim", chunk)
        left -= chunk

# Join this process to the cgroup.
w(f"{cg}/cgroup.procs", os.getpid())

buf = mmap.mmap(-1, n)
for i in range(0, n, PAGE):
    base = ((i >> 12) & 0x3f) + 1
    buf[i:i+PAGE] = bytes((base + (j & 0x7)) & 0xff for j in range(PAGE))

reclaim(n)
for _ in range(8):
    try:
        stored = int(open("/sys/kernel/debug/zswap/stored_pages").read().split()[0])
    except Exception:
        stored = 0
    if stored > (mb * 1024 // 8):
        break
    reclaim(16 << 20)
    time.sleep(0.05)

t0 = time.perf_counter()
sink = 0
for i in range(0, n, PAGE):
    sink ^= buf[i]
t1 = time.perf_counter()
ms = (t1 - t0) * 1e3
print(f"{ms:.3f}")
buf.close()
PY
		)
		rmdir "$CG" 2>/dev/null || true
		load_times+=("$load_ms")
	done

	store_med=$(median "${store_times[@]}")
	load_med=$(median "${load_times[@]}")
	if [ "$store_med" = "nan" ] || [ -z "$store_med" ]; then
		store_mbs="-"
		store_med="FAIL"
	else
		store_mbs=$(awk -v mb="$MEM_MB" -v ms="$store_med" 'BEGIN{ if(ms>0) printf "%.1f", mb*1000/ms; else print "inf"}')
	fi
	if [ "$load_med" = "nan" ] || [ -z "$load_med" ]; then
		load_mbs_disp="FAIL"
	else
		load_mbs_disp=$(awk -v mb="$MEM_MB" -v ms="$load_med" 'BEGIN{ if(ms>0) printf "%.1f", mb*1000/ms; else print "inf"}')
	fi

	ZSWAP_STORE_MS[$alg]=$store_med
	ZSWAP_LOAD_MS[$alg]=$load_med
	ZSWAP_STORED[$alg]=$stored_last
	ZSWAP_POOL[$alg]=$pool_last

	printf '%-10s %-24s %10s %10s %10s %12s %10s\n' \
		"$alg" "$path" "$store_med" "$store_mbs" "$load_med" "$stored_last" "$pool_last"
	# Also print load MB/s on the same conceptual row via note for clarity.
	note "  -> load_MBs≈${load_mbs_disp}  (runs store_ms=[${store_times[*]}] load_ms=[${load_times[*]}])"
done

# Restore system zram swap if we took it down.
if [ "$ZRAM0_WAS" = 1 ]; then
	note "re-enabling /dev/zram0 swap"
	swapon -p 100 /dev/zram0 2>/dev/null || true
fi
swapoff "$SWAPFILE" 2>/dev/null || true
SWAPFILE=""

# ============================================================================
# 2) zram direct compress write (shared zcomp backends)
# ============================================================================
echo
echo "================ ZRAM pageout (shared zcomp) =========="
if [ ! -w /sys/class/zram-control/hot_add ]; then
	note "zram hot_add unavailable; skipping zram matrix"
else
	# Clean up any leaked disposable devices from prior aborted runs.
	for d in /sys/block/zram[1-9]*; do
		[ -d "$d" ] || continue
		echo 1 > "$d/reset" 2>/dev/null || true
		echo "${d#/sys/block/zram}" > /sys/class/zram-control/hot_remove 2>/dev/null || true
	done

	ZRAM_ID=$(cat /sys/class/zram-control/hot_add)
	ZRAM_DEV=/dev/zram$ZRAM_ID
	ZRAM_SYS=/sys/block/zram$ZRAM_ID
	note "using disposable $ZRAM_DEV"

	printf '%-10s %-14s %12s %12s %14s %14s\n' \
		"algo" "backend" "write_ms" "write_MBs" "orig_B" "compr_B"
	printf '%s\n' "--------------------------------------------------------------------------------"

	for alg in $ALGS; do
		echo 1 > "$ZRAM_SYS/reset" 2>/dev/null || true
		if ! echo "$alg" > "$ZRAM_SYS/comp_algorithm" 2>/dev/null; then
			printf '%-10s %-14s %12s %12s %14s %14s\n' "$alg" "zcomp" "SKIP" "-" "-" "-"
			continue
		fi
		echo $((MEM_MB * 2 * 1024 * 1024)) > "$ZRAM_SYS/disksize"

		# O_DIRECT page-sized writes so the block layer compresses synchronously.
		result=$(
			MEM_MB="$MEM_MB" ZRAM_DEV="$ZRAM_DEV" python3 - <<'PY'
import os, time, ctypes

mb = int(os.environ["MEM_MB"])
dev = os.environ["ZRAM_DEV"]
n = mb << 20
PAGE = 4096

# page-aligned buffer required for O_DIRECT
buf = ctypes.create_string_buffer(n + PAGE)
addr = ctypes.addressof(buf)
aligned = addr + (PAGE - addr % PAGE) % PAGE
mem = (ctypes.c_char * n).from_address(aligned)
for i in range(0, n, PAGE):
    base = ((i >> 12) & 0x3f) + 1
    mem[i:i+PAGE] = bytes((base + (j & 0x7)) & 0xff for j in range(PAGE))

fd = os.open(dev, os.O_RDWR | os.O_DIRECT)
t0 = time.perf_counter()
wrote = 0
mv = memoryview(mem)
while wrote < n:
    wrote += os.write(fd, mv[wrote:wrote + PAGE])
os.fsync(fd)
t1 = time.perf_counter()
os.close(fd)
ms = (t1 - t0) * 1e3
print(f"{ms:.3f}")
PY
		)
		ms=$result
		mbs=$(awk -v mb="$MEM_MB" -v ms="$ms" 'BEGIN{ if(ms+0>0) printf "%.1f", mb*1000/ms; else print "-"}')
		# mm_stat: orig_data_size compr_data_size mem_used_total ...
		read -r orig compr _rest < <(cat "$ZRAM_SYS/mm_stat")
		printf '%-10s %-14s %12s %12s %14s %14s\n' \
			"$alg" "zcomp/$alg" "$ms" "$mbs" "${orig:-?}" "${compr:-?}"
	done
fi

# ============================================================================
# 3) zswap decompress vs zram-only decompress (cache advantage)
# ============================================================================
echo
echo "================ zswap cache vs zram-only ============="
# Use a fresh swapfile + optional zram0. Measure fault-in only when reclaim
# actually moved pages (zswpout or pswpout delta).
measure_faultin() {
	local mode=$1
	MEM_MB="$MEM_MB" CG="$CG" MODE="$mode" python3 - <<'PY'
import os, time, subprocess, mmap

mb = int(os.environ["MEM_MB"])
cg = os.environ["CG"]
mode = os.environ["MODE"]
n = mb << 20
PAGE = 4096

def w(path, val):
    subprocess.run(["sh", "-c", f"echo {val} > {path}"],
                   check=False, stderr=subprocess.DEVNULL)

def read_vm(key):
    with open("/proc/vmstat") as f:
        for line in f:
            k, v = line.split()
            if k == key:
                return int(v)
    return 0

def stored():
    try:
        return int(open("/sys/kernel/debug/zswap/stored_pages").read().split()[0])
    except Exception:
        return 0

w(f"{cg}/cgroup.procs", os.getpid())
# Cap cgroup so reclaim has a hard target (half the payload).
w(f"{cg}/memory.max", str((mb // 2) << 20))
w(f"{cg}/memory.high", str((mb // 2) << 20))

buf = mmap.mmap(-1, n)
for i in range(0, n, PAGE):
    base = ((i >> 12) & 0x3f) + 1
    buf[i:i+PAGE] = bytes((base + (j & 0x7)) & 0xff for j in range(PAGE))

z0 = read_vm("zswpout")
p0 = read_vm("pswpout")
# Force reclaim of the excess over memory.max
for _ in range(20):
    w(f"{cg}/memory.reclaim", str(32 << 20))
    time.sleep(0.05)
    if stored() > (mb * 1024 // 8) or (read_vm("pswpout") - p0) > (mb * 1024 // 8):
        break

z1 = read_vm("zswpout")
p1 = read_vm("pswpout")
st = stored()

t0 = time.perf_counter()
sink = 0
for i in range(0, n, PAGE):
    sink ^= buf[i]
t1 = time.perf_counter()
ms = (t1 - t0) * 1e3
print(f"{ms:.3f} {st} {z1-z0} {p1-p0}")
buf.close()
PY
}

# --- 3a: zswap ON, isolated swapfile (no zram) ---
if swapon --show --noheadings | awk '{print $1}' | grep -qx /dev/zram0; then
	swapoff /dev/zram0
	ZRAM0_WAS=1
fi
SWAPFILE="$WORKDIR/swapfile2"
dd if=/dev/zero of="$SWAPFILE" bs=1M count=$((MEM_MB * 3)) status=none
chmod 600 "$SWAPFILE"
mkswap "$SWAPFILE" >/dev/null
swapon -p 10 "$SWAPFILE"

echo Y > "$ZSWAP/enabled"
try_set_compressor lzo || true
echo 0 > "$ZSWAP/shrinker_enabled" 2>/dev/null || true
setup_cg
read -r ms_on stored_on dzsw dpsw < <(measure_faultin zswap-on)
note "zswap ON  + swapfile: fault-in ${ms_on} ms | stored_before_fault≈${stored_on} zswpoutΔ=${dzsw} pswpoutΔ=${dpsw}"
rmdir "$CG" 2>/dev/null || true
swapoff "$SWAPFILE"
rm -f "$SWAPFILE"
SWAPFILE=""

# --- 3b: zswap OFF, system zram only ---
echo 0 > "$ZSWAP/enabled"
if [ "${ZRAM0_WAS:-0}" = 1 ] || [ -b /dev/zram0 ]; then
	swapon -p 100 /dev/zram0 2>/dev/null || true
fi
setup_cg
read -r ms_off stored_off dzsw2 dpsw2 < <(measure_faultin zram-only)
note "zswap OFF + zram0:   fault-in ${ms_off} ms | stored=${stored_off} zswpoutΔ=${dzsw2} pswpoutΔ=${dpsw2}"
rmdir "$CG" 2>/dev/null || true

echo Y > "$ZSWAP/enabled"
awk -v on="$ms_on" -v off="$ms_off" -v dz="$dzsw" -v dp="$dpsw2" 'BEGIN{
	if (on+0>0 && off+0>0 && dz+0>100 && dp+0>100)
		printf "[*] speedup (zram-only / zswap-cache): %.2fx\n", off/on
	else
		printf "[*] skip speedup: need both paths to actually swap (zswpoutΔ=%s pswpoutΔ=%s)\n", dz, dp
}'

# leave zram0 as we found it
if [ "${ZRAM0_WAS:-0}" = 1 ]; then
	swapon -p 100 /dev/zram0 2>/dev/null || true
fi

echo
echo "================ Interpretation ======================="
cat <<EOF
* SW/zcomp path algs (lzo, lzo-rle, lz4hc): on a patched zswap_use_zcomp
  kernel these bypass crypto acomp and share lib/zcomp with zram. Compare
  their store_MBs / load_MBs against this machine's pre-series baseline
  (everything was crypto). Expect lower overhead / higher MB/s on store.

* crypto path algs (lz4, zstd, ...): stay on crypto for offload; control group,
  should be ~unchanged by the series.

* ZRAM write column: after the series these backends live in lib/zcomp
  (CONFIG_ZCOMP_BACKEND_*). MB/s should stay in the same ballpark as
  pre-series zram; the win is one shared implementation with zswap SW path.

* zswap ON vs zram-only: hot-page fault-in from the zswap pool vs decompress
  through the zram block path. Shows why a fast SW compressor on the zswap
  path matters in front of zram.

Log saved to: $LOG
EOF

log "done"

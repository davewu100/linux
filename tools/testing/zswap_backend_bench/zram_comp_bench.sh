#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# zram_comp_bench.sh - portable zram compression benchmark (pure shell).
#
# Measures, for each supported zram compression algorithm, the achieved
# compression ratio and write/read throughput over the SAME batch of
# representative mixed-compressibility data (not all-zero pages).
#
# Portable across environments (different kernels/algorithm sets): the
# algorithm list is read from the kernel, and only dd/awk/sync are needed.
# Run the identical script on a crypto-software kernel and on a zcomp kernel
# to compare.
#
# Usage:  sudo ./zram_comp_bench.sh [SIZE_MB] [ALG ...]
#   SIZE_MB : amount of data pushed through zram per algorithm (default 128)
#   ALG ... : restrict to these algorithms (default: all the kernel supports)
#
# Safe on production-ish boxes: uses a private zram device, no swap, no memory
# pressure; resets and (if it loaded the module) unloads zram at the end.

set -u

SIZE_MB="${1:-128}"
[ $# -ge 1 ] && shift
USER_ALGS="$*"

DEV=/sys/block/zram0
NODE=/dev/zram0
DATA=/tmp/zram_bench_data.bin
LOADED_BY_US=0

log()  { echo "$@"; }
die()  { echo "ERROR: $*" >&2; cleanup; exit 1; }

need_root() {
	[ "$(id -u)" -eq 0 ] || die "must run as root (sudo)"
}

ensure_zram() {
	if [ ! -d "$DEV" ]; then
		modprobe zram num_devices=1 2>/dev/null || die "cannot load zram module"
		LOADED_BY_US=1
		# give udev a moment
		sleep 1
	fi
	[ -d "$DEV" ] || die "zram sysfs not present"
}

cleanup() {
	echo 1 > "$DEV/reset" 2>/dev/null || true
	[ "$LOADED_BY_US" -eq 1 ] && modprobe -r zram 2>/dev/null || true
	rm -f "$DATA" 2>/dev/null || true
}

# Build a representative payload that resembles real anonymous memory well
# enough to separate the algorithms: mostly text-like/structured content
# (where lz4 vs zstd/deflate actually differ) with a minority of near-random
# data.  Avoids the degenerate all-zero / all-same case that would make every
# compressor look identical.  The text-like majority is deterministic; only
# the near-random tail comes from /dev/urandom and thus differs run to run,
# but its proportion is fixed, so the reported ratios are stable in practice.
make_data() {
	sz_bytes=$(( SIZE_MB * 1024 * 1024 ))
	[ -f "$DATA" ] && [ "$(wc -c < "$DATA")" -eq "$sz_bytes" ] && return 0

	rm -f "$DATA"

	# Seed 1 MB of English-like text with varied vocabulary (realistic entropy,
	# leaves headroom that better algorithms exploit -> visible ratio spread).
	seed=/tmp/zram_bench_seed.txt
	rm -f "$seed"
	i=0
	while [ "$i" -lt 4096 ]; do
		printf '%s' "the quick brown fox jumps over the lazy dog while zswap compresses anonymous memory pages into the pool and later decompresses them on swapin number $i differs slightly each line to add entropy " >> "$seed"
		i=$(( i + 1 ))
	done

	# 70% text-like (repeat seed), 30% near-random.
	txt_bytes=$(( sz_bytes * 70 / 100 ))
	rnd_bytes=$(( sz_bytes - txt_bytes ))

	# fill text portion by repeating the seed
	written=0
	seedsz=$(wc -c < "$seed")
	while [ "$written" -lt "$txt_bytes" ]; do
		need=$(( txt_bytes - written ))
		take=$seedsz
		[ "$take" -gt "$need" ] && take="$need"
		head -c "$take" "$seed" >> "$DATA"
		written=$(( written + take ))
	done

	# near-random tail
	head -c "$rnd_bytes" /dev/urandom >> "$DATA"

	# exact-size pad if needed
	cur=$(wc -c < "$DATA")
	[ "$cur" -lt "$sz_bytes" ] && head -c $(( sz_bytes - cur )) /dev/urandom >> "$DATA"
	rm -f "$seed"
}

# Nanosecond timestamp.  GNU date supports %N; some minimal date
# implementations (e.g. busybox) do not and leave a literal "N", which would
# break the arithmetic below.  Detect that once and fall back to second
# resolution (seconds * 1e9) so the throughput figures stay usable (coarser,
# but never a divide-by-zero) instead of failing outright.
if date +%N 2>/dev/null | grep -q '^[0-9]\{1,\}$'; then
	HAVE_NS=1
else
	HAVE_NS=0
	echo "note: date has no nanosecond (%N) support; timing falls back to" \
	     "second resolution (throughput is approximate)." >&2
fi

now_ns() {
	if [ "$HAVE_NS" -eq 1 ]; then
		date +%s%N
	else
		echo "$(( $(date +%s) * 1000000000 ))"
	fi
}

run_alg() {
	alg="$1"

	echo 1 > "$DEV/reset" 2>/dev/null || true
	# select algorithm; skip if unsupported
	if ! echo "$alg" > "$DEV/comp_algorithm" 2>/dev/null; then
		printf "%-8s  (not supported, skipped)\n" "$alg"
		return
	fi
	# size the device a bit larger than the payload
	echo "$(( (SIZE_MB + 32) * 1024 * 1024 ))" > "$DEV/disksize" 2>/dev/null \
		|| { printf "%-8s  (disksize failed)\n" "$alg"; return; }

	# ---- write (compress) ----
	sync
	t0=$(now_ns)
	dd if="$DATA" of="$NODE" bs=1M oflag=direct 2>/dev/null \
		|| dd if="$DATA" of="$NODE" bs=1M 2>/dev/null
	sync
	t1=$(now_ns)

	# ---- read (decompress) ----
	t2=$(now_ns)
	dd if="$NODE" of=/dev/null bs=1M iflag=direct 2>/dev/null \
		|| dd if="$NODE" of=/dev/null bs=1M 2>/dev/null
	t3=$(now_ns)

	# ---- stats ----
	# mm_stat is a space-separated line; the first two fields are, in order,
	# orig_data_size and compr_data_size (see Documentation/admin-guide/
	# blockdev/zram.rst).  Field order has been stable across releases.
	set -- $(cat "$DEV/mm_stat" 2>/dev/null)
	orig="${1:-0}"; compr="${2:-0}"

	awk -v alg="$alg" -v orig="$orig" -v compr="$compr" \
	    -v wns="$(( t1 - t0 ))" -v rns="$(( t3 - t2 ))" -v mb="$SIZE_MB" '
	BEGIN {
		ratio = (compr > 0) ? orig / compr : 0;
		wmbps = (wns > 0) ? mb * 1000000000.0 / wns : 0;
		rmbps = (rns > 0) ? mb * 1000000000.0 / rns : 0;
		printf "%-8s  ratio %6.3f:1   write %7.1f MB/s   read %7.1f MB/s   (orig=%d compr=%d)\n",
		       alg, ratio, wmbps, rmbps, orig, compr;
	}'

	echo 1 > "$DEV/reset" 2>/dev/null || true
}

main() {
	need_root
	ensure_zram

	# figure out algorithm list
	raw=$(cat "$DEV/comp_algorithm" 2>/dev/null | tr -d '[]')
	if [ -n "$USER_ALGS" ]; then
		ALGS="$USER_ALGS"
	else
		ALGS="$raw"
	fi

	log "=== zram compression benchmark ==="
	log "kernel : $(uname -r)"
	log "data   : ${SIZE_MB} MB mixed (70% text-like + 30% near-random)"
	log "algos  : $ALGS"
	log "-----------------------------------------------------------------------"

	make_data

	for a in $ALGS; do
		run_alg "$a"
	done

	log "-----------------------------------------------------------------------"
	log "ratio = orig/compressed (higher = better compression)"
	log "note  : throughput is end-to-end block I/O incl. dd overhead; use it"
	log "        for relative comparison across algorithms/kernels, not as an"
	log "        absolute compressor speed."

	cleanup
}

main

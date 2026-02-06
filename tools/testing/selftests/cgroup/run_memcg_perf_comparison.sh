#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# run_memcg_perf_comparison.sh - Run memory cgroup perf test and compare rstat vs atomic
#
# Creates a temporary cgroup, runs test_memcg_perf on it, then parses output to
# compare performance of:
#   - memory.stat       (rstat backend)  <- test reads this file for rstat timing
#   - memory.stat.atomic (atomic backend) <- test reads this file for atomic timing
#
# So: rstat timing = reading memory.stat; atomic timing = reading memory.stat.atomic.
#
# Usage:
#   sudo ./run_memcg_perf_comparison.sh [--iterations N] [--no-build] [--keep]
#
# Options:
#   --iterations N   Number of read iterations (default: 100000)
#   --no-build       Skip building test_memcg_perf
#   --keep           Do not remove the temporary cgroup (for debugging)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CGROUP_ROOT="/sys/fs/cgroup"
ITERATIONS=100000
DO_BUILD=1
KEEP_CG=0

# Parse options
while [ $# -gt 0 ]; do
	case "$1" in
		--iterations)
			ITERATIONS="$2"
			shift 2
			;;
		--no-build)
			DO_BUILD=0
			shift
			;;
		--keep)
			KEEP_CG=1
			shift
			;;
		--help|-h)
			head -25 "$0" | tail -20
			exit 0
			;;
		*)
			echo "Unknown option: $1" >&2
			exit 1
			;;
	esac
done

# Must run as root to create cgroup and read memory.stat*
if [ "$(id -u)" -ne 0 ]; then
	echo "Must run as root (e.g. sudo $0)" >&2
	exit 1
fi

# Resolve cgroup root (allow override for namespaced tests)
if [ -n "$CGROUP_ROOT" ] && [ -d "$CGROUP_ROOT" ]; then
	:
else
	# Try common cgroup2 mount
	for d in /sys/fs/cgroup /sys/fs/cgroup/unified; do
		if [ -d "$d" ] && [ -f "$d/cgroup.controllers" ]; then
			CGROUP_ROOT="$d"
			break
		fi
	done
fi
if [ ! -d "$CGROUP_ROOT" ] || [ ! -f "$CGROUP_ROOT/cgroup.controllers" ]; then
	echo "Cannot find cgroup2 root (tried $CGROUP_ROOT)" >&2
	exit 1
fi

# Ensure memory controller is available
if ! grep -q memory "$CGROUP_ROOT/cgroup.controllers" 2>/dev/null; then
	echo "Memory controller not enabled in $CGROUP_ROOT" >&2
	echo "Try: echo +memory > $CGROUP_ROOT/cgroup.subtree_control" >&2
	exit 1
fi

# Build test binary
TEST_BIN="$SCRIPT_DIR/test_memcg_perf"
if [ "$DO_BUILD" -eq 1 ]; then
	echo "Building test_memcg_perf..."
	make -C "$SCRIPT_DIR" test_memcg_perf >/dev/null 2>&1 || {
		echo "Build failed. Run: make -C $SCRIPT_DIR" >&2
		exit 1
	}
fi
if [ ! -x "$TEST_BIN" ]; then
	# Try OUTPUT dir used by kselftest
	TEST_BIN="$SCRIPT_DIR/../../../output/cgroup/test_memcg_perf"
	[ -x "$TEST_BIN" ] || TEST_BIN=""
fi
if [ -z "$TEST_BIN" ] || [ ! -x "$TEST_BIN" ]; then
	echo "test_memcg_perf not found. Build with: make -C $SCRIPT_DIR" >&2
	exit 1
fi

# Create temporary cgroup
CG_NAME="memcg_perf_$$"
CGPATH="${CGROUP_ROOT}/${CG_NAME}"
cleanup() {
	if [ "$KEEP_CG" -eq 0 ] && [ -d "$CGPATH" ]; then
		rmdir "$CGPATH" 2>/dev/null || true
	fi
}
trap cleanup EXIT

if ! mkdir "$CGPATH" 2>/dev/null; then
	echo "Failed to create cgroup $CGPATH" >&2
	exit 1
fi

echo "=== Memory Cgroup Perf Comparison ==="
echo "Cgroup: $CGPATH"
echo "Iterations: $ITERATIONS"
echo ""

# Run test and capture output
OUTPUT=$(mktemp)
"$TEST_BIN" --cgpath "$CGPATH" --iterations "$ITERATIONS" 2>&1 | tee "$OUTPUT" || true

# Parse: test_memcg_perf prints exactly 4 "Average: X.XXX us per read" lines in order:
#   1) memory.stat (rstat) sequential  2) memory.stat (rstat) FD reuse
#   3) memory.stat.atomic sequential   4) memory.stat.atomic FD reuse
get_avg_us() {
	local v
	v=$(grep "Average:" "$OUTPUT" | sed -n "${1}p" | awk '{gsub(/ us per read/,""); print $2}')
	echo "${v:-N/A}"
}
rstat_seq=$(get_avg_us 1)
rstat_fd=$(get_avg_us 2)
atomic_seq=$(get_avg_us 3)
atomic_fd=$(get_avg_us 4)

echo ""
echo "=== Comparison (us per read) ==="
printf "%-30s %12s %12s\n" "Test" "rstat" "atomic"
printf "%-30s %12s %12s\n" "-----------------------------" "------------" "------------"
printf "%-30s %12s %12s\n" "Sequential (open/read/close each)" "$rstat_seq" "$atomic_seq"
printf "%-30s %12s %12s\n" "FD reuse (open once, read many)" "$rstat_fd" "$atomic_fd"

# Optional: ratio if both numbers present
if [ "$rstat_seq" != "N/A" ] && [ "$atomic_seq" != "N/A" ]; then
	ratio=$(echo "scale=2; $atomic_seq / $rstat_seq" | bc 2>/dev/null || echo "N/A")
	echo ""
	echo "Atomic vs rstat ratio (sequential): ${ratio}x"
fi

rm -f "$OUTPUT"

if [ "$KEEP_CG" -eq 1 ]; then
	echo ""
	echo "Kept cgroup: $CGPATH (remove with: rmdir $CGPATH)"
fi

echo ""
echo "Done."

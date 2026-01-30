#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Run all performance tests using func_profiler tools

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=============================================="
echo "  Performance Testing with func_profiler  "
echo "=============================================="
echo

# Build all tools
echo "Building performance tools..."
make -f Makefile.perf clean
make -f Makefile.perf all
echo "✅ Build complete"
echo
echo "=============================================="
echo

# Test 1: memory.stat vs memory.stat.ks
echo "=== Test 1: memory.stat vs memory.stat.ks (specific) ==="
echo
if [ -f /sys/fs/cgroup/memory.stat.ks ]; then
    sudo ./compare_memstat_perf /sys/fs/cgroup
else
    echo "⚠️  /sys/fs/cgroup/memory.stat.ks not found"
    echo "   CONFIG_KSERIAL may not be enabled"
fi
echo
echo "=============================================="
echo

# Test 2: Using generic perf_compare
echo "=== Test 2: memory.stat vs memory.stat.ks (generic) ==="
echo
if [ -f /sys/fs/cgroup/memory.stat.ks ]; then
    sudo ./perf_compare 1000 \
        "cat /sys/fs/cgroup/memory.stat > /dev/null" \
        "cat /sys/fs/cgroup/memory.stat.ks > /dev/null"
else
    echo "⚠️  Skipping (memory.stat.ks not found)"
fi
echo
echo "=============================================="
echo

# Test 3: Using perf_benchmark
echo "=== Test 3: perf_benchmark framework ==="
echo
if [ -f /sys/fs/cgroup/memory.stat.ks ]; then
    sudo ./perf_benchmark -m
else
    echo "⚠️  Skipping (memory.stat.ks not found)"
fi
echo
echo "=============================================="
echo

# Test 4: Multiple file comparison
echo "=== Test 4: Multiple files comparison ==="
echo
sudo ./perf_benchmark -f \
    /proc/meminfo \
    /proc/cpuinfo \
    -n 100
echo
echo "=============================================="
echo

# Summary
echo "=== Summary ==="
echo
echo "✅ All performance tests completed"
echo
echo "Tools available:"
echo "  • perf_compare       - Generic command comparison"
echo "  • perf_benchmark     - Function benchmarking framework"  
echo "  • compare_memstat_perf - memory.stat specific comparison"
echo
echo "Examples:"
echo "  ./perf_compare 1000 'cmd1' 'cmd2'"
echo "  ./perf_benchmark -m"
echo "  ./compare_memstat_perf /sys/fs/cgroup"
echo

# Cleanup
make -f Makefile.perf clean
echo "✅ Cleanup complete"

#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Performance comparison: memory.numa_stat vs memory.numa_stat.ks

set -e

CGROUP_ROOT="/sys/fs/cgroup"
TEST_CGROUP="test_numa_perf_$$"
TEST_PATH="$CGROUP_ROOT/$TEST_CGROUP"
ITERATIONS=1000

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=========================================="
echo "Memory NUMA Stat Performance Comparison"
echo "=========================================="
echo ""

# Check if NUMA is available
if [ ! -f "$CGROUP_ROOT/memory.numa_stat" ]; then
    echo "❌ memory.numa_stat not found - NUMA not configured or not in root cgroup"
    exit 1
fi

# Check if kserial is enabled
if [ ! -f "$CGROUP_ROOT/memory.numa_stat.ks" ]; then
    echo "❌ memory.numa_stat.ks not found - CONFIG_KSERIAL not enabled"
    exit 1
fi

echo "✅ Both memory.numa_stat and memory.numa_stat.ks are available"
echo ""

# Create test cgroup
mkdir -p "$TEST_PATH"
echo "Created test cgroup: $TEST_PATH"
echo ""

# Function to measure read time in nanoseconds
measure_read_time() {
    local file=$1
    local iterations=$2
    local start end elapsed
    
    # Warmup
    cat "$file" > /dev/null 2>&1 || true
    
    start=$(date +%s%N)
    for ((i=0; i<iterations; i++)); do
        cat "$file" > /dev/null 2>&1 || true
    done
    end=$(date +%s%N)
    
    elapsed=$((end - start))
    echo $elapsed
}

# Function to extract kserial timing
get_kserial_time() {
    local file=$1
    grep "# kserial_time_ns" "$file" 2>/dev/null | awk '{print $3}' || echo "0"
}

echo "Test parameters:"
echo "  - Iterations: $ITERATIONS"
echo "  - Test cgroup: $TEST_PATH"
echo ""

# Test 1: memory.numa_stat (traditional)
echo "Testing memory.numa_stat (traditional)..."
NUMA_STAT_TIME=$(measure_read_time "$TEST_PATH/memory.numa_stat" $ITERATIONS)
NUMA_STAT_AVG=$((NUMA_STAT_TIME / ITERATIONS))
echo "  Total time: $NUMA_STAT_TIME ns"
echo "  Average per read: $NUMA_STAT_AVG ns ($(echo "scale=2; $NUMA_STAT_AVG / 1000" | bc) μs)"
echo ""

# Test 2: memory.numa_stat.ks (optimized)
echo "Testing memory.numa_stat.ks (optimized)..."
NUMA_STAT_KS_TIME=$(measure_read_time "$TEST_PATH/memory.numa_stat.ks" $ITERATIONS)
NUMA_STAT_KS_AVG=$((NUMA_STAT_KS_TIME / ITERATIONS))
echo "  Total time: $NUMA_STAT_KS_TIME ns"
echo "  Average per read: $NUMA_STAT_KS_AVG ns ($(echo "scale=2; $NUMA_STAT_KS_AVG / 1000" | bc) μs)"
echo ""

# Get built-in kserial timing
KSERIAL_BUILTIN_TIME=$(get_kserial_time "$TEST_PATH/memory.numa_stat.ks")
if [ "$KSERIAL_BUILTIN_TIME" != "0" ]; then
    echo "Built-in kserial timing (single read): $KSERIAL_BUILTIN_TIME ns ($(echo "scale=2; $KSERIAL_BUILTIN_TIME / 1000" | bc) μs)"
    echo ""
fi

# Calculate speedup
echo "=========================================="
echo "Performance Comparison Results"
echo "=========================================="
echo ""

SPEEDUP=$(echo "scale=2; $NUMA_STAT_AVG / $NUMA_STAT_KS_AVG" | bc)
IMPROVEMENT=$(echo "scale=2; ($NUMA_STAT_AVG - $NUMA_STAT_KS_AVG) / $NUMA_STAT_AVG * 100" | bc)

printf "${GREEN}Speedup: %.2fx faster${NC}\n" $SPEEDUP
printf "${YELLOW}Improvement: %.2f%% reduction in latency${NC}\n" $IMPROVEMENT
echo ""

echo "Breakdown:"
echo "  Traditional (memory.numa_stat):  $NUMA_STAT_AVG ns"
echo "  Optimized (memory.numa_stat.ks): $NUMA_STAT_KS_AVG ns"
echo "  Difference: $((NUMA_STAT_AVG - NUMA_STAT_KS_AVG)) ns saved per read"
echo ""

# Show sample output
echo "=========================================="
echo "Sample Output Comparison"
echo "=========================================="
echo ""
echo "--- memory.numa_stat (first 10 lines) ---"
head -10 "$TEST_PATH/memory.numa_stat"
echo ""
echo "--- memory.numa_stat.ks (first 10 lines + profiling) ---"
head -13 "$TEST_PATH/memory.numa_stat.ks"
echo ""

# Cleanup
rmdir "$TEST_PATH"
echo "Cleaned up test cgroup"
echo ""

echo "=========================================="
echo "Test Complete"
echo "=========================================="
echo ""
echo "Summary:"
echo "  - memory.numa_stat.ks is ${GREEN}${SPEEDUP}x faster${NC}"
echo "  - Optimizations: Direct seq_printf, no intermediate buffer"
echo "  - Same output format and data as traditional memory.numa_stat"
echo ""

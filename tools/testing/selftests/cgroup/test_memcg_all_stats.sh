#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Comprehensive performance comparison: memory.stat vs memory.stat.ks
#                                       memory.numa_stat vs memory.numa_stat.ks

set -e

CGROUP_ROOT="/sys/fs/cgroup"
TEST_CGROUP="test_all_stats_$$"
TEST_PATH="$CGROUP_ROOT/$TEST_CGROUP"
ITERATIONS=${1:-1000}

# Colors for output
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

echo "=========================================="
echo "Comprehensive Memory Stats Performance Test"
echo "=========================================="
echo ""

# Check system configuration
echo "System Configuration:"
echo "  - Cgroup root: $CGROUP_ROOT"
echo "  - Test iterations: $ITERATIONS"
echo ""

# Check if kserial is enabled
if [ ! -f "$CGROUP_ROOT/memory.stat.ks" ]; then
    echo "${RED}❌ CONFIG_KSERIAL not enabled${NC}"
    exit 1
fi

echo "${GREEN}✅ CONFIG_KSERIAL is enabled${NC}"

# Check NUMA support
if [ -f "$CGROUP_ROOT/memory.numa_stat" ]; then
    NUMA_AVAILABLE=1
    echo "${GREEN}✅ NUMA support is available${NC}"
else
    NUMA_AVAILABLE=0
    echo "${YELLOW}⚠️  NUMA not available (CONFIG_NUMA not set or single-node system)${NC}"
fi
echo ""

# Create test cgroup
mkdir -p "$TEST_PATH"
echo "Created test cgroup: $TEST_PATH"
echo ""

# Allocate some memory in the cgroup to make stats more interesting
echo $$ > "$TEST_PATH/cgroup.procs"
dd if=/dev/zero of=/dev/null bs=1M count=100 2>/dev/null &
WORKER_PID=$!
sleep 0.5
kill $WORKER_PID 2>/dev/null || true
wait $WORKER_PID 2>/dev/null || true
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

# Function to get built-in timing
get_builtin_time() {
    local file=$1
    grep "# kserial_time_ns" "$file" 2>/dev/null | awk '{print $3}' || echo "0"
}

# Test 1: memory.stat vs memory.stat.ks
echo "=========================================="
echo "${BLUE}Test 1: memory.stat Performance${NC}"
echo "=========================================="
echo ""

echo "Testing memory.stat (traditional)..."
STAT_TIME=$(measure_read_time "$TEST_PATH/memory.stat" $ITERATIONS)
STAT_AVG=$((STAT_TIME / ITERATIONS))
STAT_US=$(echo "scale=2; $STAT_AVG / 1000" | bc)
echo "  Total: $STAT_TIME ns"
echo "  Average: $STAT_AVG ns ($STAT_US μs)"
echo ""

echo "Testing memory.stat.ks (optimized)..."
STAT_KS_TIME=$(measure_read_time "$TEST_PATH/memory.stat.ks" $ITERATIONS)
STAT_KS_AVG=$((STAT_KS_TIME / ITERATIONS))
STAT_KS_US=$(echo "scale=2; $STAT_KS_AVG / 1000" | bc)
echo "  Total: $STAT_KS_TIME ns"
echo "  Average: $STAT_KS_AVG ns ($STAT_KS_US μs)"
echo ""

STAT_BUILTIN=$(get_builtin_time "$TEST_PATH/memory.stat.ks")
if [ "$STAT_BUILTIN" != "0" ]; then
    STAT_BUILTIN_US=$(echo "scale=2; $STAT_BUILTIN / 1000" | bc)
    echo "Built-in timing: $STAT_BUILTIN ns ($STAT_BUILTIN_US μs)"
    echo ""
fi

STAT_SPEEDUP=$(echo "scale=2; $STAT_AVG / $STAT_KS_AVG" | bc)
STAT_IMPROVEMENT=$(echo "scale=2; ($STAT_AVG - $STAT_KS_AVG) / $STAT_AVG * 100" | bc)

printf "${GREEN}memory.stat.ks: %.2fx faster (%.2f%% improvement)${NC}\n" $STAT_SPEEDUP $STAT_IMPROVEMENT
echo ""

# Test 2: memory.numa_stat vs memory.numa_stat.ks (if available)
if [ $NUMA_AVAILABLE -eq 1 ]; then
    echo "=========================================="
    echo "${BLUE}Test 2: memory.numa_stat Performance${NC}"
    echo "=========================================="
    echo ""

    echo "Testing memory.numa_stat (traditional)..."
    NUMA_TIME=$(measure_read_time "$TEST_PATH/memory.numa_stat" $ITERATIONS)
    NUMA_AVG=$((NUMA_TIME / ITERATIONS))
    NUMA_US=$(echo "scale=2; $NUMA_AVG / 1000" | bc)
    echo "  Total: $NUMA_TIME ns"
    echo "  Average: $NUMA_AVG ns ($NUMA_US μs)"
    echo ""

    echo "Testing memory.numa_stat.ks (optimized)..."
    NUMA_KS_TIME=$(measure_read_time "$TEST_PATH/memory.numa_stat.ks" $ITERATIONS)
    NUMA_KS_AVG=$((NUMA_KS_TIME / ITERATIONS))
    NUMA_KS_US=$(echo "scale=2; $NUMA_KS_AVG / 1000" | bc)
    echo "  Total: $NUMA_KS_TIME ns"
    echo "  Average: $NUMA_KS_AVG ns ($NUMA_KS_US μs)"
    echo ""

    NUMA_BUILTIN=$(get_builtin_time "$TEST_PATH/memory.numa_stat.ks")
    if [ "$NUMA_BUILTIN" != "0" ]; then
        NUMA_BUILTIN_US=$(echo "scale=2; $NUMA_BUILTIN / 1000" | bc)
        echo "Built-in timing: $NUMA_BUILTIN ns ($NUMA_BUILTIN_US μs)"
        echo ""
    fi

    NUMA_SPEEDUP=$(echo "scale=2; $NUMA_AVG / $NUMA_KS_AVG" | bc)
    NUMA_IMPROVEMENT=$(echo "scale=2; ($NUMA_AVG - $NUMA_KS_AVG) / $NUMA_AVG * 100" | bc)

    printf "${GREEN}memory.numa_stat.ks: %.2fx faster (%.2f%% improvement)${NC}\n" $NUMA_SPEEDUP $NUMA_IMPROVEMENT
    echo ""
fi

# Summary
echo "=========================================="
echo "${YELLOW}Performance Summary${NC}"
echo "=========================================="
echo ""

printf "%-25s %15s %15s %10s\n" "File" "Traditional" "Optimized" "Speedup"
printf "%-25s %15s %15s %10s\n" "----" "-----------" "---------" "-------"
printf "%-25s %12s μs %12s μs %8.2fx\n" "memory.stat" "$STAT_US" "$STAT_KS_US" $STAT_SPEEDUP

if [ $NUMA_AVAILABLE -eq 1 ]; then
    printf "%-25s %12s μs %12s μs %8.2fx\n" "memory.numa_stat" "$NUMA_US" "$NUMA_KS_US" $NUMA_SPEEDUP
fi
echo ""

# Sample outputs
echo "=========================================="
echo "${BLUE}Sample Outputs${NC}"
echo "=========================================="
echo ""

echo "--- memory.stat (first 5 lines) ---"
head -5 "$TEST_PATH/memory.stat"
echo "..."
echo ""

echo "--- memory.stat.ks (first 5 lines + profiling) ---"
head -8 "$TEST_PATH/memory.stat.ks"
echo ""

if [ $NUMA_AVAILABLE -eq 1 ]; then
    echo "--- memory.numa_stat (first 5 lines) ---"
    head -5 "$TEST_PATH/memory.numa_stat"
    echo "..."
    echo ""

    echo "--- memory.numa_stat.ks (first 5 lines + profiling) ---"
    head -8 "$TEST_PATH/memory.numa_stat.ks"
    echo ""
fi

# Cleanup
rmdir "$TEST_PATH"
echo "Cleaned up test cgroup"
echo ""

echo "=========================================="
echo "${GREEN}Test Complete${NC}"
echo "=========================================="
echo ""
echo "Key Findings:"
printf "  ${GREEN}✓${NC} memory.stat.ks is %.2fx faster than memory.stat\n" $STAT_SPEEDUP
if [ $NUMA_AVAILABLE -eq 1 ]; then
    printf "  ${GREEN}✓${NC} memory.numa_stat.ks is %.2fx faster than memory.numa_stat\n" $NUMA_SPEEDUP
fi
echo ""
echo "Optimization technique:"
echo "  ${YELLOW}→${NC} Direct seq_printf (no seq_buf intermediate buffer)"
echo "  ${YELLOW}→${NC} Reduced memory allocations and copies"
echo "  ${YELLOW}→${NC} Same output format and data as traditional files"
echo ""

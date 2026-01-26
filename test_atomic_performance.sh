#!/bin/bash
# Performance test script for atomic counter optimizations
# This script compares performance before and after optimizations

set -e

RESULTS_DIR="./atomic_perf_results"
mkdir -p "$RESULTS_DIR"

echo "=========================================="
echo "Atomic Counter Performance Test Suite"
echo "=========================================="
echo ""

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Check if kernel is compiled with atomic counter support
check_config() {
    echo -e "${YELLOW}[1/5] Checking kernel configuration...${NC}"
    
    if ! grep -q "CONFIG_MEMCG_ATOMIC_COUNTER=y" /boot/config-$(uname -r) 2>/dev/null; then
        if [ ! -f .config ]; then
            echo -e "${RED}ERROR: Cannot find kernel config${NC}"
            exit 1
        fi
        
        if ! grep -q "CONFIG_MEMCG_ATOMIC_COUNTER=y" .config; then
            echo -e "${RED}ERROR: CONFIG_MEMCG_ATOMIC_COUNTER is not enabled${NC}"
            exit 1
        fi
    fi
    
    echo -e "${GREEN}✓ Atomic counter is enabled${NC}"
    echo ""
}

# Test 1: Cache line bouncing test
test_cache_bouncing() {
    echo -e "${YELLOW}[2/5] Testing cache line bouncing reduction...${NC}"
    
    # Create test cgroup
    CGROUP_PATH="/sys/fs/cgroup/test_atomic_$$"
    mkdir -p "$CGROUP_PATH"
    
    # Run parallel memory operations
    perf stat -e cache-misses,cache-references,L1-dcache-load-misses \
        -o "$RESULTS_DIR/cache_bouncing.txt" -- bash -c "
        for i in {1..8}; do
            (
                echo \$\$ > $CGROUP_PATH/cgroup.procs
                stress-ng --vm 1 --vm-bytes 64M --timeout 5s --quiet
            ) &
        done
        wait
    " 2>&1
    
    # Cleanup
    rmdir "$CGROUP_PATH" 2>/dev/null || true
    
    # Analyze results
    CACHE_MISS_RATE=$(grep "cache-misses" "$RESULTS_DIR/cache_bouncing.txt" | \
        awk '{print $4}' | tr -d '%')
    
    echo -e "Cache miss rate: ${CACHE_MISS_RATE}%"
    if (( $(echo "$CACHE_MISS_RATE < 5.0" | bc -l) )); then
        echo -e "${GREEN}✓ PASS: Cache miss rate is low (<5%)${NC}"
    else
        echo -e "${RED}✗ WARNING: Cache miss rate is high (${CACHE_MISS_RATE}%)${NC}"
    fi
    echo ""
}

# Test 2: Atomic operation frequency
test_atomic_ops() {
    echo -e "${YELLOW}[3/5] Testing atomic operation frequency...${NC}"
    
    # Use perf to count atomic operations
    perf stat -e cycles,instructions,cpu/event=0xb1,umask=0x01/ \
        -o "$RESULTS_DIR/atomic_ops.txt" -- bash -c "
        CGROUP_PATH=/sys/fs/cgroup/test_atomic_$$
        mkdir -p \$CGROUP_PATH
        echo \$\$ > \$CGROUP_PATH/cgroup.procs
        
        # High-frequency memory operations
        dd if=/dev/zero of=/dev/null bs=4K count=1000000 2>&1
        
        rmdir \$CGROUP_PATH
    " 2>&1
    
    echo -e "${GREEN}✓ Atomic operation test completed${NC}"
    echo "Results saved to: $RESULTS_DIR/atomic_ops.txt"
    echo ""
}

# Test 3: Batching effectiveness
test_batching() {
    echo -e "${YELLOW}[4/5] Testing per-CPU batching effectiveness...${NC}"
    
    # Monitor atomic_stock usage
    cat > /tmp/test_batch.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>

#define ITERATIONS 1000000
#define ALLOC_SIZE 4096

int main() {
    int i;
    void *ptr;
    
    for (i = 0; i < ITERATIONS; i++) {
        ptr = malloc(ALLOC_SIZE);
        if (ptr) {
            memset(ptr, 0, ALLOC_SIZE);
            free(ptr);
        }
        
        if (i % 100000 == 0) {
            printf("Progress: %d%%\n", (i * 100) / ITERATIONS);
        }
    }
    
    return 0;
}
EOF
    
    gcc -O2 /tmp/test_batch.c -o /tmp/test_batch
    
    # Run with perf
    CGROUP_PATH="/sys/fs/cgroup/test_batch_$$"
    mkdir -p "$CGROUP_PATH"
    echo $$ > "$CGROUP_PATH/cgroup.procs"
    
    perf stat -e cycles,instructions,cache-misses \
        -o "$RESULTS_DIR/batching.txt" /tmp/test_batch 2>&1
    
    rmdir "$CGROUP_PATH" 2>/dev/null || true
    rm /tmp/test_batch.c /tmp/test_batch
    
    echo -e "${GREEN}✓ Batching test completed${NC}"
    echo ""
}

# Test 4: Compare with baseline
test_performance_comparison() {
    echo -e "${YELLOW}[5/5] Performance comparison test...${NC}"
    
    # Test memory operations in cgroup
    CGROUP_PATH="/sys/fs/cgroup/perf_test_$$"
    mkdir -p "$CGROUP_PATH"
    
    echo "Running memory stress test..."
    time_result=$(time -p bash -c "
        echo \$\$ > $CGROUP_PATH/cgroup.procs
        stress-ng --vm 4 --vm-bytes 256M --timeout 30s --metrics-brief 2>&1
    " 2>&1)
    
    echo "$time_result" > "$RESULTS_DIR/performance.txt"
    
    # Extract key metrics
    real_time=$(echo "$time_result" | grep "^real" | awk '{print $2}')
    sys_time=$(echo "$time_result" | grep "^sys" | awk '{print $2}')
    
    echo "Real time: ${real_time}s"
    echo "System time: ${sys_time}s"
    
    rmdir "$CGROUP_PATH" 2>/dev/null || true
    
    echo -e "${GREEN}✓ Performance test completed${NC}"
    echo ""
}

# Generate report
generate_report() {
    echo "=========================================="
    echo "Test Results Summary"
    echo "=========================================="
    echo ""
    
    cat > "$RESULTS_DIR/REPORT.md" << EOF
# Atomic Counter Performance Test Report

Generated: $(date)
Kernel: $(uname -r)

## Test Results

### 1. Cache Line Bouncing Test
$(cat "$RESULTS_DIR/cache_bouncing.txt" 2>/dev/null || echo "No data")

### 2. Atomic Operations Test
$(cat "$RESULTS_DIR/atomic_ops.txt" 2>/dev/null || echo "No data")

### 3. Batching Effectiveness
$(cat "$RESULTS_DIR/batching.txt" 2>/dev/null || echo "No data")

### 4. Performance Comparison
$(cat "$RESULTS_DIR/performance.txt" 2>/dev/null || echo "No data")

## Analysis

### Expected Improvements
- Cache miss rate: < 5% (optimized vs 8-10% baseline)
- Atomic operations: ~60% reduction
- System time: 20-40% improvement

### Recommendations
1. If cache miss rate > 5%, check cache line alignment
2. If sys time improvement < 20%, verify batching is working
3. Monitor /proc/vmstat for memcg statistics updates

## Conclusion

Compare these results with baseline (before optimization) to verify:
- ✓ Reduced cache misses
- ✓ Fewer atomic operations
- ✓ Lower system time overhead
EOF
    
    echo -e "${GREEN}✓ Report generated: $RESULTS_DIR/REPORT.md${NC}"
    echo ""
    
    # Display summary
    echo "Key Metrics:"
    echo "============"
    if [ -f "$RESULTS_DIR/cache_bouncing.txt" ]; then
        echo -n "Cache miss rate: "
        grep "cache-misses" "$RESULTS_DIR/cache_bouncing.txt" | awk '{print $4}'
    fi
    if [ -f "$RESULTS_DIR/performance.txt" ]; then
        echo -n "System time: "
        grep "^sys" "$RESULTS_DIR/performance.txt" | awk '{print $2}'
    fi
    echo ""
}

# Main execution
main() {
    # Check if running as root
    if [ "$EUID" -ne 0 ]; then
        echo -e "${RED}ERROR: This script must be run as root${NC}"
        exit 1
    fi
    
    # Run tests
    check_config
    test_cache_bouncing
    test_atomic_ops
    test_batching
    test_performance_comparison
    generate_report
    
    echo "=========================================="
    echo -e "${GREEN}All tests completed successfully!${NC}"
    echo "=========================================="
    echo ""
    echo "Results directory: $RESULTS_DIR"
    echo "Full report: $RESULTS_DIR/REPORT.md"
    echo ""
    echo "To compare with baseline, run this script before and after"
    echo "applying optimizations and compare the REPORT.md files."
}

# Run main
main "$@"

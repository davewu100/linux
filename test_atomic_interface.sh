#!/bin/bash
# Test script for memory.stat.atomic interface

CGROUP_ROOT="/sys/fs/cgroup"
TEST_CGROUP="$CGROUP_ROOT/test_atomic"

echo "=========================================="
echo "Testing memory.stat.atomic Interface"
echo "=========================================="
echo ""

# Create test cgroup if needed
if [ ! -d "$TEST_CGROUP" ]; then
    echo "Creating test cgroup: $TEST_CGROUP"
    mkdir -p "$TEST_CGROUP"
fi

echo "1. Checking available interfaces:"
echo "   - memory.stat: $(ls -la "$CGROUP_ROOT/memory.stat" 2>&1 | grep -q "No such file" && echo "NOT FOUND" || echo "FOUND")"
echo "   - memory.stat.atomic: $(ls -la "$CGROUP_ROOT/memory.stat.atomic" 2>&1 | grep -q "No such file" && echo "NOT FOUND" || echo "FOUND")"
echo "   - memory.numa_stat.atomic: $(ls -la "$CGROUP_ROOT/memory.numa_stat.atomic" 2>&1 | grep -q "No such file" && echo "NOT FOUND" || echo "FOUND")"
echo ""

echo "2. Reading memory.stat (rstat backend) - first 6 lines:"
head -6 "$CGROUP_ROOT/memory.stat" 2>/dev/null || echo "  [Not available]"
echo ""

echo "3. Reading memory.stat.atomic (atomic backend) - first 6 lines:"
head -6 "$CGROUP_ROOT/memory.stat.atomic" 2>/dev/null || echo "  [Not available - CONFIG_MEMCG_ATOMIC_COUNTER not enabled]"
echo ""

echo "4. Performance comparison:"
echo "   Measuring read latency..."

# Test rstat
if [ -f "$CGROUP_ROOT/memory.stat" ]; then
    START=$(date +%s%N)
    for i in {1..100}; do
        cat "$CGROUP_ROOT/memory.stat" > /dev/null 2>&1
    done
    END=$(date +%s%N)
    RSTAT_TIME=$((($END - $START) / 100 / 1000))  # microseconds
    echo "   rstat (memory.stat): ${RSTAT_TIME} μs per read (avg of 100)"
fi

# Test atomic
if [ -f "$CGROUP_ROOT/memory.stat.atomic" ]; then
    START=$(date +%s%N)
    for i in {1..100}; do
        cat "$CGROUP_ROOT/memory.stat.atomic" > /dev/null 2>&1
    done
    END=$(date +%s%N)
    ATOMIC_TIME=$((($END - $START) / 100 / 1000))  # microseconds
    echo "   atomic (memory.stat.atomic): ${ATOMIC_TIME} μs per read (avg of 100)"
    
    if [ -n "$RSTAT_TIME" ] && [ "$RSTAT_TIME" -gt 0 ]; then
        SPEEDUP=$((RSTAT_TIME * 100 / ATOMIC_TIME))
        echo "   Speedup: ${SPEEDUP}% of rstat time"
    fi
fi

echo ""

echo "5. NUMA statistics (if available):"
if [ -f "$CGROUP_ROOT/memory.numa_stat" ]; then
    echo "   memory.numa_stat (rstat) - first 6 lines:"
    head -6 "$CGROUP_ROOT/memory.numa_stat" 2>/dev/null || echo "     [Error reading]"
    echo ""
fi
if [ -f "$CGROUP_ROOT/memory.numa_stat.atomic" ]; then
    echo "   memory.numa_stat.atomic - first 6 lines:"
    head -6 "$CGROUP_ROOT/memory.numa_stat.atomic" 2>/dev/null || echo "     [Error reading]"
    echo ""
fi

echo "=========================================="
echo "Test completed!"
echo "=========================================="
echo ""
echo "Usage examples:"
echo "  # Compare outputs"
echo "  diff <(cat /sys/fs/cgroup/memory.stat) \\"
echo "       <(cat /sys/fs/cgroup/memory.stat.atomic)"
echo ""
echo "  # Benchmark specific cgroup"
echo "  time cat /sys/fs/cgroup/my_workload/memory.stat"
echo "  time cat /sys/fs/cgroup/my_workload/memory.stat.atomic"

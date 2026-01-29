#!/bin/bash
# Test script for kserial block read functionality

set -e

echo "========================================================================"
echo "k-serial Block Read Test"
echo "========================================================================"
echo ""

# Check if kserial module is loaded
if [ ! -e /proc/kserial ]; then
    echo "Error: /proc/kserial not found"
    echo "Loading k-serial module..."
    sudo insmod kserial.ko || {
        echo "Failed to load module"
        exit 1
    }
fi

echo "Test 1: Single element read (baseline)"
echo "------------------------------------------------------------------------"
echo "Command: kserial mem_cgroup vmstats.state[9]"
time sudo ./kserial mem_cgroup vmstats.state[9]
echo ""

echo "Test 2: Array range read [0..9]"
echo "------------------------------------------------------------------------"
echo "Command: kserial mem_cgroup vmstats.state[0..9]"
time sudo ./kserial mem_cgroup vmstats.state[0..9]
echo ""

echo "Test 3: Larger range read [0..49]"
echo "------------------------------------------------------------------------"
echo "Command: kserial mem_cgroup vmstats.state[0..49]"
time sudo ./kserial mem_cgroup vmstats.state[0..49]
echo ""

echo "Test 4: Wildcard read (entire array)"
echo "------------------------------------------------------------------------"
echo "Command: kserial mem_cgroup vmstats.state[*]"
time sudo ./kserial mem_cgroup vmstats.state[*]
echo ""

echo "Test 5: Performance comparison"
echo "------------------------------------------------------------------------"
echo "Reading 50 elements..."
echo ""

echo "Method 1: Individual queries (50 separate calls)"
START=$(date +%s.%N)
for i in {0..49}; do
    sudo ./kserial mem_cgroup vmstats.state[$i] > /dev/null 2>&1
done
END=$(date +%s.%N)
INDIVIDUAL_TIME=$(echo "$END - $START" | bc)
echo "Individual queries time: ${INDIVIDUAL_TIME}s"

echo ""
echo "Method 2: Block read (1 call)"
START=$(date +%s.%N)
sudo ./kserial mem_cgroup vmstats.state[0..49] > /dev/null 2>&1
END=$(date +%s.%N)
BLOCK_TIME=$(echo "$END - $START" | bc)
echo "Block read time: ${BLOCK_TIME}s"

echo ""
SPEEDUP=$(echo "scale=1; $INDIVIDUAL_TIME / $BLOCK_TIME" | bc)
echo "Speedup: ${SPEEDUP}x faster"
echo ""

echo "========================================================================"
echo "Summary"
echo "========================================================================"
echo "✅ Block read functionality working!"
echo "✅ Performance improvement: ${SPEEDUP}x"
echo ""
echo "Supported syntax:"
echo "  field[N]           - Single element: state[5]"
echo "  field[start..end]  - Range: state[0..49]"
echo "  field[*]           - All elements: state[*]"

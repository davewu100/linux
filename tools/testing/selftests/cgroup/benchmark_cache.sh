#!/bin/bash
# Benchmark kserial cache performance
# 
# Shows the dramatic speedup from caching BTF lookups

set -e

ITERATIONS=${1:-1000}

echo "======================================"
echo "kserial Cache Performance Benchmark"
echo "======================================"
echo "Iterations: $ITERATIONS"
echo ""

if [ ! -e /proc/kserial ]; then
    echo "Error: /proc/kserial not found"
    echo "Please load kserial module first"
    exit 1
fi

if ! command -v kserial &> /dev/null; then
    echo "Error: kserial tool not found"
    exit 1
fi

# ============================================
# Test 1: Cold cache (first query)
# ============================================
echo "Test 1: Cold Cache (First Query)"
echo "-----------------------------------"

# Clear cache
echo "clear" > /proc/kserial_stats 2>/dev/null || true

START=$(date +%s.%N)
kserial -s cgroup level nr_descendants max_depth >/dev/null 2>&1
END=$(date +%s.%N)

COLD_TIME=$(echo "$END - $START" | bc)
echo "Time: ${COLD_TIME}s (includes BTF lookup)"
echo ""

# ============================================
# Test 2: Warm cache (repeated queries)
# ============================================
echo "Test 2: Warm Cache (Repeated Queries)"
echo "--------------------------------------"

START=$(date +%s.%N)
for i in $(seq 1 $ITERATIONS); do
    kserial -s cgroup level nr_descendants max_depth >/dev/null 2>&1
done
END=$(date +%s.%N)

TOTAL_TIME=$(echo "$END - $START" | bc)
AVG_TIME=$(echo "scale=6; $TOTAL_TIME / $ITERATIONS" | bc)

echo "Total time:   ${TOTAL_TIME}s"
echo "Average time: ${AVG_TIME}s per query"
echo "Throughput:   $(echo "scale=0; $ITERATIONS / $TOTAL_TIME" | bc) queries/sec"
echo ""

# ============================================
# Test 3: Compare with cold cache simulation
# ============================================
echo "Test 3: Speedup Analysis"
echo "------------------------"

SPEEDUP=$(echo "scale=1; $COLD_TIME / $AVG_TIME" | bc)

echo "Cold cache (first):  ${COLD_TIME}s"
echo "Warm cache (avg):    ${AVG_TIME}s"
echo "Speedup:             ${SPEEDUP}x"
echo ""

# ============================================
# Test 4: Cache statistics
# ============================================
echo "Test 4: Cache Statistics"
echo "------------------------"

if [ -f /proc/kserial_stats ]; then
    cat /proc/kserial_stats
else
    echo "Statistics not available (need /proc/kserial_stats)"
fi
echo ""

# ============================================
# Test 5: Different field combinations
# ============================================
echo "Test 5: Different Field Patterns"
echo "---------------------------------"

# Single field
START=$(date +%s.%N)
for i in $(seq 1 100); do
    kserial -s cgroup level >/dev/null 2>&1
done
END=$(date +%s.%N)
SINGLE_TIME=$(echo "scale=6; ($END - $START) / 100" | bc)
echo "Single field (level):            ${SINGLE_TIME}s"

# Multiple simple fields
START=$(date +%s.%N)
for i in $(seq 1 100); do
    kserial -s cgroup level nr_descendants max_depth >/dev/null 2>&1
done
END=$(date +%s.%N)
MULTI_TIME=$(echo "scale=6; ($END - $START) / 100" | bc)
echo "Multiple fields (3):             ${MULTI_TIME}s"

# Nested field
START=$(date +%s.%N)
for i in $(seq 1 100); do
    kserial -s cgroup self.id >/dev/null 2>&1
done
END=$(date +%s.%N)
NESTED_TIME=$(echo "scale=6; ($END - $START) / 100" | bc)
echo "Nested field (self.id):          ${NESTED_TIME}s"

# Array access
START=$(date +%s.%N)
for i in $(seq 1 100); do
    kserial -s cgroup nr_dying_subsys[0] >/dev/null 2>&1
done
END=$(date +%s.%N)
ARRAY_TIME=$(echo "scale=6; ($END - $START) / 100" | bc)
echo "Array access (nr_dying_subsys[0]): ${ARRAY_TIME}s"

echo ""

# ============================================
# Summary
# ============================================
echo "======================================"
echo "Summary"
echo "======================================"
echo ""
echo "Key Findings:"
echo "  • Cache provides ${SPEEDUP}x speedup"
echo "  • Warm queries take only ${AVG_TIME}s"
echo "  • Throughput: $(echo "scale=0; $ITERATIONS / $TOTAL_TIME" | bc) queries/sec"
echo ""
echo "Recommendation:"
if [ $(echo "$SPEEDUP > 50" | bc) -eq 1 ]; then
    echo "  ✓ Excellent! Cache is working very well"
elif [ $(echo "$SPEEDUP > 10" | bc) -eq 1 ]; then
    echo "  ✓ Good! Cache provides significant speedup"
else
    echo "  ⚠ Cache speedup lower than expected"
    echo "    Check if cache is enabled and has sufficient size"
fi
echo ""
echo "For production use:"
echo "  • Use consistent field names to maximize cache hits"
echo "  • Consider pre-warming cache at startup"
echo "  • Monitor cache hit rate via /proc/kserial_stats"

#!/bin/bash
# Benchmark: cat vs kserial for reading 3 memory.stat fields

ITERATIONS=1000

echo "======================================================================"
echo "Performance Benchmark: cat vs kserial"
echo "======================================================================"
echo "Task: Read 3 fields (anon, file, kernel) from memory.stat"
echo "Iterations: $ITERATIONS"
echo ""

# ============================================
# Test 1: cat + head
# ============================================
echo "Test 1: cat /sys/fs/cgroup/memory.stat | head -3"
echo "----------------------------------------------------------------------"

START=$(date +%s.%N)
for i in $(seq 1 $ITERATIONS); do
    cat /sys/fs/cgroup/memory.stat | head -3 >/dev/null 2>&1
done
END=$(date +%s.%N)

CAT_TOTAL=$(echo "$END - $START" | bc)
CAT_AVG=$(echo "scale=3; $CAT_TOTAL * 1000 / $ITERATIONS" | bc)

echo "Total time:   ${CAT_TOTAL}s"
echo "Average:      ${CAT_AVG}ms per query"
echo "Syscalls:     ~$((ITERATIONS * 5)) (open, read, close, pipe ops)"
echo ""

# ============================================
# Test 2: grep (more selective)
# ============================================
echo "Test 2: grep -E '^(anon|file|kernel) ' /sys/fs/cgroup/memory.stat"
echo "----------------------------------------------------------------------"

START=$(date +%s.%N)
for i in $(seq 1 $ITERATIONS); do
    grep -E "^(anon|file|kernel) " /sys/fs/cgroup/memory.stat >/dev/null 2>&1
done
END=$(date +%s.%N)

GREP_TOTAL=$(echo "$END - $START" | bc)
GREP_AVG=$(echo "scale=3; $GREP_TOTAL * 1000 / $ITERATIONS" | bc)

echo "Total time:   ${GREP_TOTAL}s"
echo "Average:      ${GREP_AVG}ms per query"
echo "Syscalls:     ~$((ITERATIONS * 3)) (open, read, close)"
echo ""

# ============================================
# Test 3: kserial (if available)
# ============================================
echo "Test 3: kserial -m anon file kernel"
echo "----------------------------------------------------------------------"

if [ -e /proc/kserial ] && command -v kserial &> /dev/null; then
    START=$(date +%s.%N)
    for i in $(seq 1 $ITERATIONS); do
        kserial -m anon file kernel >/dev/null 2>&1
    done
    END=$(date +%s.%N)
    
    KSERIAL_TOTAL=$(echo "$END - $START" | bc)
    KSERIAL_AVG=$(echo "scale=3; $KSERIAL_TOTAL * 1000 / $ITERATIONS" | bc)
    
    echo "Total time:   ${KSERIAL_TOTAL}s"
    echo "Average:      ${KSERIAL_AVG}ms per query"
    echo "Syscalls:     ~$((ITERATIONS * 4)) (open, write, read, close)"
    KSERIAL_AVAILABLE=1
else
    echo "⚠️  kserial not available"
    KSERIAL_AVAILABLE=0
fi
echo ""

# ============================================
# Summary
# ============================================
echo "======================================================================"
echo "Summary"
echo "======================================================================"
printf "%-20s %15s %15s %s\n" "Method" "Avg Time" "Total Time" "Winner"
echo "----------------------------------------------------------------------"
printf "%-20s %15s %15s" "cat | head -3" "${CAT_AVG}ms" "${CAT_TOTAL}s"
if [ $KSERIAL_AVAILABLE -eq 0 ]; then
    echo ""
else
    if [ $(echo "$CAT_AVG < $KSERIAL_AVG" | bc) -eq 1 ]; then
        echo " ✅"
    else
        echo ""
    fi
fi

printf "%-20s %15s %15s" "grep" "${GREP_AVG}ms" "${GREP_TOTAL}s"
if [ $KSERIAL_AVAILABLE -eq 0 ]; then
    echo ""
else
    if [ $(echo "$GREP_AVG < $CAT_AVG && $GREP_AVG < $KSERIAL_AVG" | bc) -eq 1 ]; then
        echo " ✅"
    else
        echo ""
    fi
fi

if [ $KSERIAL_AVAILABLE -eq 1 ]; then
    printf "%-20s %15s %15s" "kserial -m" "${KSERIAL_AVG}ms" "${KSERIAL_TOTAL}s"
    if [ $(echo "$KSERIAL_AVG < $CAT_AVG && $KSERIAL_AVG < $GREP_AVG" | bc) -eq 1 ]; then
        echo " ✅"
    else
        echo ""
    fi
fi

echo ""
echo "Analysis:"
echo "----------------------------------------------------------------------"

if [ $KSERIAL_AVAILABLE -eq 1 ]; then
    SPEEDUP_VS_CAT=$(echo "scale=1; $CAT_AVG / $KSERIAL_AVG" | bc)
    SPEEDUP_VS_GREP=$(echo "scale=1; $GREP_AVG / $KSERIAL_AVG" | bc)
    
    if [ $(echo "$KSERIAL_AVG < $CAT_AVG" | bc) -eq 1 ]; then
        echo "✓ kserial is ${SPEEDUP_VS_CAT}x faster than cat | head"
    else
        echo "✓ cat | head is faster than kserial"
    fi
    
    if [ $(echo "$KSERIAL_AVG < $GREP_AVG" | bc) -eq 1 ]; then
        echo "✓ kserial is ${SPEEDUP_VS_GREP}x faster than grep"
    else
        echo "✓ grep is faster than kserial"
    fi
fi

echo ""
echo "Syscall breakdown:"
echo "  cat | head: ~5 syscalls (open, read, close + pipe ops)"
echo "  grep:       ~3 syscalls (open, read, close)"
echo "  kserial:    ~4 syscalls (open, write, read, close)"
echo ""
echo "Why differences:"
echo "  - cat reads entire file, kserial reads specific fields"
echo "  - grep uses regex matching, kserial uses cached offsets"
echo "  - Pipe overhead in 'cat | head'"
echo "  - Text parsing vs binary data"

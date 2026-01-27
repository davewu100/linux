#!/bin/bash
# Script to verify mem_cgroup statistics

echo "=== Compare k-serial and memory.stat values ==="
echo ""

# Get current process cgroup (cgroup v2)
CGROUP_PATH=$(cat /proc/self/cgroup | head -1 | cut -d: -f3)
if [ -z "$CGROUP_PATH" ]; then
    CGROUP_PATH="/"
fi

echo "Current process cgroup path: $CGROUP_PATH"
echo ""

# Check and compile test_kserial_real if needed
if [ ! -x "./test_kserial_real" ]; then
    echo "--- Compiling test_kserial_real ---"
    if [ -f Makefile.kserial ]; then
        make -f Makefile.kserial test_kserial_real || {
            echo "Compilation failed, trying direct gcc..."
            gcc -Wall -Wextra -O2 -o test_kserial_real test_kserial_real.c || {
                echo "Error: Failed to compile test_kserial_real"
                exit 1
            }
        }
    else
        gcc -Wall -Wextra -O2 -o test_kserial_real test_kserial_real.c || {
            echo "Error: Failed to compile test_kserial_real"
            exit 1
        }
    fi
    echo "✓ test_kserial_real compiled"
    echo ""
fi

# Read current process cgroup's memory.stat (this triggers stat flush)
echo "--- Current process cgroup memory.stat values (bytes) ---"
MEMORY_STAT_FILE="/sys/fs/cgroup${CGROUP_PATH}/memory.stat"
MEMORY_CURRENT_FILE="/sys/fs/cgroup${CGROUP_PATH}/memory.current"
if [ -f "$MEMORY_STAT_FILE" ]; then
    # Trigger stat flush by reading memory.stat multiple times
    # This ensures stats are flushed from per-CPU/per-NMI buffers to vmstats.state[]
    cat "$MEMORY_STAT_FILE" > /dev/null
    # Also read memory.current to trigger another flush path
    if [ -f "$MEMORY_CURRENT_FILE" ]; then
        cat "$MEMORY_CURRENT_FILE" > /dev/null
    fi
    # Read again to ensure flush completed
    cat "$MEMORY_STAT_FILE" > /dev/null
    
    ANON_BYTES=$(grep "^anon " "$MEMORY_STAT_FILE" | awk '{print $2}')
    FILE_BYTES=$(grep "^file " "$MEMORY_STAT_FILE" | awk '{print $2}')
    KERNEL_BYTES=$(grep "^kernel " "$MEMORY_STAT_FILE" | awk '{print $2}')
    KERNEL_STACK_BYTES=$(grep "^kernel_stack " "$MEMORY_STAT_FILE" | awk '{print $2}')
    
    echo "anon         = $ANON_BYTES bytes"
    echo "file         = $FILE_BYTES bytes"
    echo "kernel       = $KERNEL_BYTES bytes"
    echo "kernel_stack = $KERNEL_STACK_BYTES bytes"
else
    echo "memory.stat not found at $MEMORY_STAT_FILE"
    exit 1
fi

echo ""
echo "--- k-serial query values (raw) ---"
echo "(Note: Stats flushed before query - reading vmstats.state[] directly)"
KSERIAL_OUTPUT=$(sudo ./test_kserial_real --struct mem_cgroup \
    vmstats.state[14] vmstats.state[16] vmstats.state[34] vmstats.state[23] 2>&1)

echo "$KSERIAL_OUTPUT"

# Extract k-serial values
ANON_PAGES=$(echo "$KSERIAL_OUTPUT" | grep "vmstats.state\[14\]" | awk '{print $3}')
FILE_PAGES=$(echo "$KSERIAL_OUTPUT" | grep "vmstats.state\[16\]" | awk '{print $3}')
KERNEL_RAW=$(echo "$KSERIAL_OUTPUT" | grep "vmstats.state\[34\]" | awk '{print $3}')
KERNEL_STACK_KB=$(echo "$KSERIAL_OUTPUT" | grep "vmstats.state\[23\]" | awk '{print $3}')

echo ""
echo "--- Unit Conversion and Comparison ---"
echo ""

# Convert k-serial values to bytes and compare
if [ -n "$ANON_PAGES" ] && [ "$ANON_PAGES" != "=" ] && [ -n "$ANON_BYTES" ]; then
    ANON_BYTES_KSERIAL=$((ANON_PAGES * 4096))
    echo "anon:"
    echo "  k-serial:   $ANON_PAGES pages = $ANON_BYTES_KSERIAL bytes"
    echo "  memory.stat: $ANON_BYTES bytes"
    if [ "$ANON_BYTES_KSERIAL" -eq "$ANON_BYTES" ]; then
        echo "  ✓ Match!"
    else
        DIFF=$((ANON_BYTES_KSERIAL - ANON_BYTES))
        if [ $DIFF -lt 0 ]; then
            DIFF=$((-$DIFF))
        fi
        echo "  ✗ Mismatch! Difference: $DIFF bytes"
    fi
    echo ""
fi

if [ -n "$FILE_PAGES" ] && [ "$FILE_PAGES" != "=" ] && [ -n "$FILE_BYTES" ]; then
    FILE_BYTES_KSERIAL=$((FILE_PAGES * 4096))
    echo "file:"
    echo "  k-serial:   $FILE_PAGES pages = $FILE_BYTES_KSERIAL bytes"
    echo "  memory.stat: $FILE_BYTES bytes"
    if [ "$FILE_BYTES_KSERIAL" -eq "$FILE_BYTES" ]; then
        echo "  ✓ Match!"
    else
        DIFF=$((FILE_BYTES_KSERIAL - FILE_BYTES))
        if [ $DIFF -lt 0 ]; then
            DIFF=$((-$DIFF))
        fi
        echo "  ✗ Mismatch! Difference: $DIFF bytes"
    fi
    echo ""
fi

if [ -n "$KERNEL_RAW" ] && [ "$KERNEL_RAW" != "=" ] && [ -n "$KERNEL_BYTES" ]; then
    # vmstats.state[34] stores MEMCG_KMEM in pages, convert to bytes
    KERNEL_BYTES_KSERIAL=$((KERNEL_RAW * 4096))
    echo "kernel:"
    echo "  k-serial:   $KERNEL_RAW pages = $KERNEL_BYTES_KSERIAL bytes"
    echo "  memory.stat: $KERNEL_BYTES bytes"
    if [ "$KERNEL_BYTES_KSERIAL" -eq "$KERNEL_BYTES" ]; then
        echo "  ✓ Match!"
    else
        DIFF=$((KERNEL_BYTES_KSERIAL - KERNEL_BYTES))
        if [ $DIFF -lt 0 ]; then
            DIFF=$((-$DIFF))
        fi
        echo "  ✗ Mismatch! Difference: $DIFF bytes"
        if [ "$KERNEL_RAW" -eq "0" ]; then
            echo "  Note: k-serial returned 0 - stats may not be flushed yet."
            echo "        memory.stat automatically flushes stats on read."
            echo "        Try triggering a stat flush before querying with k-serial."
        fi
    fi
    echo ""
fi

if [ -n "$KERNEL_STACK_KB" ] && [ "$KERNEL_STACK_KB" != "=" ] && [ -n "$KERNEL_STACK_BYTES" ]; then
    KERNEL_STACK_BYTES_KSERIAL=$((KERNEL_STACK_KB * 1024))
    echo "kernel_stack:"
    echo "  k-serial:   $KERNEL_STACK_KB KB = $KERNEL_STACK_BYTES_KSERIAL bytes"
    echo "  memory.stat: $KERNEL_STACK_BYTES bytes"
    if [ "$KERNEL_STACK_BYTES_KSERIAL" -eq "$KERNEL_STACK_BYTES" ]; then
        echo "  ✓ Match!"
    else
        DIFF=$((KERNEL_STACK_BYTES_KSERIAL - KERNEL_STACK_BYTES))
        if [ $DIFF -lt 0 ]; then
            DIFF=$((-$DIFF))
        fi
        echo "  ✗ Mismatch! Difference: $DIFF bytes"
    fi
    echo ""
fi

echo "Note:"
echo "  - memory.stat values are in bytes"
echo "  - k-serial values need conversion:"
echo "    * anon, file, kernel: pages * 4096 = bytes"
echo "    * kernel_stack: KB * 1024 = bytes"
echo ""
echo "Important: k-serial reads the current process's mem_cgroup,"
echo "           memory.stat reads the value for the corresponding cgroup path"
echo "           If they don't match, check if the process is in the expected cgroup"
echo ""
echo "Possible reasons for mismatch:"
echo "  1. Statistics not flushed (reading memory.stat triggers automatic flush)"
echo "  2. Different cgroup contexts"
echo "  3. Timing differences in stat collection"
echo "  4. Per-CPU stats not fully aggregated to vmstats.state[]"

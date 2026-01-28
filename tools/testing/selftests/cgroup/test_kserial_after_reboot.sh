#!/bin/bash
# kserial test script after reboot
# Please run this script manually (requires sudo privileges)

set -e

KERNEL_DIR="/home/jianyuew/develop/kernel/cgroup/linux"
cd "$KERNEL_DIR/tools/testing/selftests/cgroup"

echo "=== kserial Post-Reboot Test ==="
echo ""

# 1. Check kernel version
echo "1. Checking kernel version..."
echo "   Current kernel: $(uname -r)"
echo ""

# 2. Check if symbols exist
echo "2. Checking if required symbols exist..."
SYMBOLS=(
    "btf_find_by_name_kind"
    "bpf_get_btf_vmlinux"
    "btf_type_skip_modifiers"
    "btf_name_by_offset"
)

MISSING=0
for sym in "${SYMBOLS[@]}"; do
    if sudo cat /proc/kallsyms 2>/dev/null | grep -q "^[0-9a-f]* [tT] $sym$"; then
        echo "   ✓ $sym"
    else
        echo "   ✗ $sym - not found!"
        MISSING=1
    fi
done

if [ $MISSING -eq 1 ]; then
    echo ""
    echo "⚠️  Warning: Some symbols not found, kernel may need to be recompiled"
    echo ""
fi

# 3. Check module files
echo ""
echo "3. Checking module files..."
if [ -f "$KERNEL_DIR/kernel/kserial.ko" ]; then
    echo "   ✓ kserial.ko exists"
    ls -lh "$KERNEL_DIR/kernel/kserial.ko"
else
    echo "   ✗ kserial.ko not found"
    echo "   Need to recompile modules"
fi

# 4. Try to load modules
echo ""
echo "4. Loading modules..."
if lsmod | grep -q kserial; then
    echo "   ✓ Modules already loaded"
    lsmod | grep kserial
else
    echo "   Loading modules..."
    
    # Load in dependency order: kserial_block -> kserial -> kserial_procfs
    if [ -f "$KERNEL_DIR/kernel/kserial_block.ko" ]; then
        if lsmod | grep -q kserial_block; then
            echo "   ✓ kserial_block already loaded"
        else
            echo "   Loading kserial_block.ko..."
            sudo insmod "$KERNEL_DIR/kernel/kserial_block.ko" 2>&1 || {
                echo "   ⚠ kserial_block.ko load failed"
                sudo dmesg | tail -5
            }
        fi
    fi
    
    if [ -f "$KERNEL_DIR/kernel/kserial.ko" ]; then
        if lsmod | grep -q "^kserial "; then
            echo "   ✓ kserial already loaded"
        else
            echo "   Loading kserial.ko..."
            sudo insmod "$KERNEL_DIR/kernel/kserial.ko" 2>&1 || {
                echo "   ⚠ kserial.ko load failed"
                echo "   Error messages:"
                sudo dmesg | tail -10 | grep -i kserial
                exit 1
            }
        fi
    fi
    
    if [ -f "$KERNEL_DIR/kernel/kserial_procfs.ko" ]; then
        if lsmod | grep -q kserial_procfs; then
            echo "   ✓ kserial_procfs already loaded"
        else
            echo "   Loading kserial_procfs.ko..."
            sudo insmod "$KERNEL_DIR/kernel/kserial_procfs.ko" 2>&1 || {
                echo "   ⚠ kserial_procfs.ko load failed"
                echo "   Error messages:"
                sudo dmesg | tail -10 | grep -i kserial
                exit 1
            }
        fi
    fi
    
    echo "   ✓ Modules loaded successfully"
fi

# 5. Verify /proc/kserial
echo ""
echo "5. Verifying /proc/kserial..."
if [ -e /proc/kserial ]; then
    echo "   ✓ /proc/kserial exists"
    ls -l /proc/kserial
else
    echo "   ✗ /proc/kserial does not exist"
    echo "   Check dmesg:"
    dmesg | tail -10 | grep -i kserial || echo "   No related logs"
    exit 1
fi

# 6. Compile test program
echo ""
echo "6. Compiling test program..."
if [ ! -x "./test_kserial_real" ]; then
    gcc -Wall -Wextra -o test_kserial_real test_kserial_real.c || {
        echo "   ✗ Compilation failed"
        exit 1
    }
fi
echo "   ✓ Test program ready"

# 7. Run tests
echo ""
echo "7. Running tests..."
echo ""
echo "--- Test 1: Query cgroup fields ---"
sudo ./test_kserial_real level nr_descendants max_depth
echo ""

echo "--- Test 2: Query mem_cgroup memory statistics ---"
sudo ./test_kserial_real --struct mem_cgroup vmstats.state[14] vmstats.state[16] vmstats.state[34]
echo ""

echo "--- Test 3: Query specified process (PID 1) ---"
sudo ./test_kserial_real --struct mem_cgroup --pid 1 vmstats.state[14] vmstats.state[16]
echo ""

echo "=== All tests completed ==="

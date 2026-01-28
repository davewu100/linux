#!/bin/bash
# Script to load kserial modules

set -e

KERNEL_DIR="/home/jianyuew/develop/kernel/cgroup/linux"
cd "$KERNEL_DIR"

echo "=== Loading kserial modules ==="
echo ""

# Check if module files exist
KSERIAL_KO="kernel/kserial.ko"
KSERIAL_BLOCK_KO="kernel/kserial_block.ko"
KSERIAL_PROCFS_KO="kernel/kserial_procfs.ko"

if [ ! -f "$KSERIAL_KO" ] || [ ! -f "$KSERIAL_PROCFS_KO" ]; then
    echo "⚠ Module files not found, please compile first:"
    echo "   make -j\$(nproc) kernel/kserial.ko kernel/kserial_procfs.ko kernel/kserial_block.ko"
    exit 1
fi

# Unload old modules (if exist)
echo "1. Unloading old modules (if exist)..."
sudo rmmod kserial_procfs 2>/dev/null || true
sudo rmmod kserial_block 2>/dev/null || true
sudo rmmod kserial 2>/dev/null || true
echo "   ✓ Done"

# Load modules (in dependency order)
echo ""
echo "2. Loading modules (in dependency order)..."

# First load kserial_block.ko (doesn't depend on other kserial modules)
if [ -f "$KSERIAL_BLOCK_KO" ]; then
    if lsmod | grep -q kserial_block; then
        echo "   ✓ kserial_block already loaded"
    else
        echo "   Loading kserial_block.ko..."
        sudo insmod "$KSERIAL_BLOCK_KO" 2>&1 || {
            echo "   ⚠ Load failed, check errors:"
            sudo dmesg | tail -10
            exit 1
        }
        echo "   ✓ kserial_block loaded successfully"
    fi
fi

# Then load kserial.ko (depends on kserial_block)
if [ -f "$KSERIAL_KO" ]; then
    if lsmod | grep -q "^kserial "; then
        echo "   ✓ kserial already loaded"
    else
        echo "   Loading kserial.ko..."
        sudo insmod "$KSERIAL_KO" 2>&1 || {
            echo "   ⚠ Load failed, check errors:"
            sudo dmesg | tail -10 | grep -i kserial
            exit 1
        }
        echo "   ✓ kserial loaded successfully"
    fi
fi

# Finally load kserial_procfs.ko (depends on kserial)
if [ -f "$KSERIAL_PROCFS_KO" ]; then
    if lsmod | grep -q kserial_procfs; then
        echo "   ✓ kserial_procfs already loaded"
    else
        echo "   Loading kserial_procfs.ko..."
        sudo insmod "$KSERIAL_PROCFS_KO" 2>&1 || {
            echo "   ⚠ Load failed, check errors:"
            sudo dmesg | tail -10 | grep -i kserial
            exit 1
        }
        echo "   ✓ kserial_procfs loaded successfully"
    fi
fi

echo "   ✓ Modules loaded successfully"

# Verify
echo ""
echo "3. Verifying..."
if [ -e /proc/kserial ]; then
    echo "   ✓ /proc/kserial exists"
    echo ""
    echo "Loaded modules:"
    lsmod | grep kserial || echo "   (not shown, but loaded)"
else
    echo "   ⚠ /proc/kserial does not exist"
    echo "   Recent dmesg output:"
    dmesg | tail -10 | grep -i kserial || dmesg | tail -5
fi

echo ""
echo "=== Done ==="
echo ""
echo "You can now test:"
echo "  cd tools/testing/selftests/cgroup"
echo "  sudo ./test_kserial_real --struct mem_cgroup vmstats.state[14] vmstats.state[16] vmstats.state[34]"

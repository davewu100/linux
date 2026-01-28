#!/bin/bash
# Quick load kserial modules (correct order)

set -e

KERNEL_DIR="/home/jianyuew/develop/kernel/cgroup/linux"
cd "$KERNEL_DIR"

echo "=== Loading kserial modules ==="
echo ""

# Check module files
KSERIAL_BLOCK_KO="/lib/modules/$(uname -r)/kernel/kernel/kserial_block.ko"
KSERIAL_KO="/lib/modules/$(uname -r)/kernel/kernel/kserial.ko"
KSERIAL_PROCFS_KO="/lib/modules/$(uname -r)/kernel/kernel/kserial_procfs.ko"

# Load in dependency order
echo "1. Loading kserial_block.ko..."
if lsmod | grep -q kserial_block; then
    echo "   ✓ Already loaded"
else
    sudo modprobe kserial_block || sudo insmod "$KSERIAL_BLOCK_KO"
    echo "   ✓ Loaded successfully"
fi

echo ""
echo "2. Loading kserial.ko..."
if lsmod | grep -q "^kserial "; then
    echo "   ✓ Already loaded"
else
    sudo modprobe kserial || sudo insmod "$KSERIAL_KO"
    echo "   ✓ Loaded successfully"
fi

echo ""
echo "3. Loading kserial_procfs.ko..."
if lsmod | grep -q kserial_procfs; then
    echo "   ✓ Already loaded"
else
    sudo modprobe kserial_procfs || sudo insmod "$KSERIAL_PROCFS_KO"
    echo "   ✓ Loaded successfully"
fi

echo ""
echo "4. Verifying..."
if [ -e /proc/kserial ]; then
    echo "   ✓ /proc/kserial exists"
    lsmod | grep kserial
else
    echo "   ✗ /proc/kserial does not exist"
    echo "   Error messages:"
    sudo dmesg | tail -10 | grep -i kserial
    exit 1
fi

echo ""
echo "=== Done ==="

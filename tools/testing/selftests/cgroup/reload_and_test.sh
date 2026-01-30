#!/bin/bash
# Reload kserial modules and run verification test

set -e

KERNEL_DIR="/home/jianyuew/develop/kernel/cgroup/linux"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

cd "$KERNEL_DIR"

echo "=== Reloading kserial modules ==="
echo ""

# Unload old modules
echo "1. Unloading old modules..."
sudo rmmod kserial_procfs 2>/dev/null || true
sudo rmmod kserial_block 2>/dev/null || true
sudo rmmod kserial 2>/dev/null || true
echo "   ✓ Done"
echo ""

# Load modules in correct order
echo "2. Loading new modules..."
echo "   Loading kserial_block.ko..."
sudo insmod kernel/kserial_block.ko 2>&1 | grep -v "File exists" || true

echo "   Loading kserial.ko..."
sudo insmod kernel/kserial.ko 2>&1 | grep -v "File exists" || true

echo "   Loading kserial_procfs.ko..."
sudo insmod kernel/kserial_procfs.ko 2>&1 || {
    echo "   ✗ Failed to load kserial_procfs.ko"
    echo "   Error messages:"
    dmesg | tail -10 | grep -i kserial || dmesg | tail -5
    exit 1
}
echo "   ✓ All modules loaded"
echo ""

# Verify
echo "3. Verifying modules..."
if [ -e /dev/kserial ]; then
    echo "   ✓ /dev/kserial exists"
    lsmod | grep kserial
else
    echo "   ✗ /dev/kserial does not exist"
    echo "   Recent dmesg output:"
    dmesg | tail -10 | grep -i kserial || dmesg | tail -5
    exit 1
fi
echo ""

# Run verification test
echo "=== Running verification test ==="
echo ""
cd "$SCRIPT_DIR"
sudo ./verify_memcg_stats.sh

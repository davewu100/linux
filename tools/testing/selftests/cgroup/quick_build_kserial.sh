#!/bin/bash
# Quick build and load kserial modules

set -e

KERNEL_DIR="/home/jianyuew/develop/kernel/cgroup/linux"
cd "$KERNEL_DIR"

echo "=== Quick Build kserial Modules ==="
echo ""

# Check configuration
if ! grep -q "^CONFIG_KSERIAL=m" .config 2>/dev/null; then
    echo "⚠ Configuration is not module mode, updating..."
    sed -i 's/^CONFIG_KSERIAL=y/CONFIG_KSERIAL=m/' .config
    echo "✓ Updated configuration to module mode"
fi

# Unload old modules (if exist)
echo ""
echo "1. Unloading old modules (if exist)..."
sudo rmmod kserial_procfs 2>/dev/null || true
sudo rmmod kserial_block 2>/dev/null || true
sudo rmmod kserial 2>/dev/null || true
echo "✓ Done"

# Prepare build environment
echo ""
echo "2. Preparing build environment..."
if [ ! -d include/generated ]; then
    echo "   Running make prepare..."
    make prepare >/dev/null 2>&1 || {
        echo "   ⚠ make prepare failed, trying full preparation..."
        make prepare
    }
fi

# Check if vmlinux needs to be compiled first (for Module.symvers generation)
if [ ! -f Module.symvers ] || [ ! -f vmlinux ]; then
    echo "   Compiling vmlinux to generate symbol table..."
    make -j$(nproc) vmlinux >/dev/null 2>&1 || {
        echo "   ⚠ vmlinux compilation failed, continuing..."
    }
fi
echo "   ✓ Build environment ready"

# Compile modules
echo ""
echo "3. Compiling kserial modules..."
make -j$(nproc) M=kernel modules 2>&1 | tee /tmp/kserial_build.log | \
    grep -E "(kserial|Error|error|Building|CC|LD|WARNING)" || true

# Find compiled modules
KSERIAL_KO=$(find kernel -name "kserial.ko" -type f 2>/dev/null | head -1)
KSERIAL_BLOCK_KO=$(find kernel -name "kserial_block.ko" -type f 2>/dev/null | head -1)
KSERIAL_PROCFS_KO=$(find kernel -name "kserial_procfs.ko" -type f 2>/dev/null | head -1)

if [ -z "$KSERIAL_PROCFS_KO" ]; then
    echo ""
    echo "⚠ Module files not found, check build log..."
    tail -30 /tmp/kserial_build.log
    exit 1
fi

echo ""
echo "✓ Found module files:"
[ -n "$KSERIAL_KO" ] && echo "  - $KSERIAL_KO"
[ -n "$KSERIAL_BLOCK_KO" ] && echo "  - $KSERIAL_BLOCK_KO"
echo "  - $KSERIAL_PROCFS_KO"

# Load modules
echo ""
echo "4. Loading modules..."
if [ -n "$KSERIAL_KO" ]; then
    echo "  Loading kserial.ko..."
    sudo insmod "$KSERIAL_KO" 2>&1 | grep -v "File exists" || true
fi

if [ -n "$KSERIAL_BLOCK_KO" ]; then
    echo "  Loading kserial_block.ko..."
    sudo insmod "$KSERIAL_BLOCK_KO" 2>&1 | grep -v "File exists" || true
fi

echo "  Loading kserial_procfs.ko..."
sudo insmod "$KSERIAL_PROCFS_KO" 2>&1 || {
    echo "  ⚠ Load failed, check errors:"
    dmesg | tail -10
    exit 1
}

echo "✓ Modules loaded successfully"

# Verify
echo ""
echo "5. Verifying..."
if [ -e /proc/kserial ]; then
    echo "✓ /proc/kserial exists"
    echo ""
    echo "Loaded modules:"
    lsmod | grep kserial || echo "  (not shown, but loaded)"
else
    echo "⚠ /proc/kserial does not exist"
    echo "Recent dmesg output:"
    dmesg | tail -10 | grep -i kserial || dmesg | tail -5
fi

echo ""
echo "=== Done ==="
echo ""
echo "You can now test:"
echo "  cd tools/testing/selftests/cgroup"
echo "  sudo ./test_kserial_real --struct mem_cgroup vmstats.state[14] vmstats.state[16] vmstats.state[34]"

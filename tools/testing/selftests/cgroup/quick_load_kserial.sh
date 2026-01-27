#!/bin/bash
# 快速加载 kserial 模块（正确的顺序）

set -e

KERNEL_DIR="/home/jianyuew/develop/kernel/cgroup/linux"
cd "$KERNEL_DIR"

echo "=== 加载 kserial 模块 ==="
echo ""

# 检查模块文件
KSERIAL_BLOCK_KO="/lib/modules/$(uname -r)/kernel/kernel/kserial_block.ko"
KSERIAL_KO="/lib/modules/$(uname -r)/kernel/kernel/kserial.ko"
KSERIAL_PROCFS_KO="/lib/modules/$(uname -r)/kernel/kernel/kserial_procfs.ko"

# 按依赖顺序加载
echo "1. 加载 kserial_block.ko..."
if lsmod | grep -q kserial_block; then
    echo "   ✓ 已加载"
else
    sudo modprobe kserial_block || sudo insmod "$KSERIAL_BLOCK_KO"
    echo "   ✓ 加载成功"
fi

echo ""
echo "2. 加载 kserial.ko..."
if lsmod | grep -q "^kserial "; then
    echo "   ✓ 已加载"
else
    sudo modprobe kserial || sudo insmod "$KSERIAL_KO"
    echo "   ✓ 加载成功"
fi

echo ""
echo "3. 加载 kserial_procfs.ko..."
if lsmod | grep -q kserial_procfs; then
    echo "   ✓ 已加载"
else
    sudo modprobe kserial_procfs || sudo insmod "$KSERIAL_PROCFS_KO"
    echo "   ✓ 加载成功"
fi

echo ""
echo "4. 验证..."
if [ -e /proc/kserial ]; then
    echo "   ✓ /proc/kserial 存在"
    lsmod | grep kserial
else
    echo "   ✗ /proc/kserial 不存在"
    echo "   错误信息:"
    sudo dmesg | tail -10 | grep -i kserial
    exit 1
fi

echo ""
echo "=== 完成 ==="

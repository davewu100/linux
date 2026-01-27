#!/bin/bash
# 加载 kserial 模块的脚本

set -e

KERNEL_DIR="/home/jianyuew/develop/kernel/cgroup/linux"
cd "$KERNEL_DIR"

echo "=== 加载 kserial 模块 ==="
echo ""

# 检查模块文件是否存在
KSERIAL_KO="kernel/kserial.ko"
KSERIAL_BLOCK_KO="kernel/kserial_block.ko"
KSERIAL_PROCFS_KO="kernel/kserial_procfs.ko"

if [ ! -f "$KSERIAL_KO" ] || [ ! -f "$KSERIAL_PROCFS_KO" ]; then
    echo "⚠ 模块文件不存在，请先编译："
    echo "   make -j\$(nproc) kernel/kserial.ko kernel/kserial_procfs.ko kernel/kserial_block.ko"
    exit 1
fi

# 卸载旧模块（如果存在）
echo "1. 卸载旧模块（如果存在）..."
sudo rmmod kserial_procfs 2>/dev/null || true
sudo rmmod kserial_block 2>/dev/null || true
sudo rmmod kserial 2>/dev/null || true
echo "   ✓ 完成"

# 加载模块（按依赖顺序）
echo ""
echo "2. 加载模块（按依赖顺序）..."

# 首先加载 kserial_block.ko（不依赖其他 kserial 模块）
if [ -f "$KSERIAL_BLOCK_KO" ]; then
    if lsmod | grep -q kserial_block; then
        echo "   ✓ kserial_block 已加载"
    else
        echo "   加载 kserial_block.ko..."
        sudo insmod "$KSERIAL_BLOCK_KO" 2>&1 || {
            echo "   ⚠ 加载失败，查看错误:"
            sudo dmesg | tail -10
            exit 1
        }
        echo "   ✓ kserial_block 加载成功"
    fi
fi

# 然后加载 kserial.ko（依赖 kserial_block）
if [ -f "$KSERIAL_KO" ]; then
    if lsmod | grep -q "^kserial "; then
        echo "   ✓ kserial 已加载"
    else
        echo "   加载 kserial.ko..."
        sudo insmod "$KSERIAL_KO" 2>&1 || {
            echo "   ⚠ 加载失败，查看错误:"
            sudo dmesg | tail -10 | grep -i kserial
            exit 1
        }
        echo "   ✓ kserial 加载成功"
    fi
fi

# 最后加载 kserial_procfs.ko（依赖 kserial）
if [ -f "$KSERIAL_PROCFS_KO" ]; then
    if lsmod | grep -q kserial_procfs; then
        echo "   ✓ kserial_procfs 已加载"
    else
        echo "   加载 kserial_procfs.ko..."
        sudo insmod "$KSERIAL_PROCFS_KO" 2>&1 || {
            echo "   ⚠ 加载失败，查看错误:"
            sudo dmesg | tail -10 | grep -i kserial
            exit 1
        }
        echo "   ✓ kserial_procfs 加载成功"
    fi
fi

echo "   ✓ 模块加载成功"

# 验证
echo ""
echo "3. 验证..."
if [ -e /proc/kserial ]; then
    echo "   ✓ /proc/kserial 存在"
    echo ""
    echo "已加载的模块:"
    lsmod | grep kserial || echo "   (未显示，但已加载)"
else
    echo "   ⚠ /proc/kserial 不存在"
    echo "   最近的 dmesg 输出:"
    dmesg | tail -10 | grep -i kserial || dmesg | tail -5
fi

echo ""
echo "=== 完成 ==="
echo ""
echo "现在可以测试:"
echo "  cd tools/testing/selftests/cgroup"
echo "  sudo ./test_kserial_real --struct mem_cgroup vmstats.state[14] vmstats.state[16] vmstats.state[34]"

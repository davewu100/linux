#!/bin/bash
# 快速编译和加载 kserial 模块

set -e

KERNEL_DIR="/home/jianyuew/develop/kernel/cgroup/linux"
cd "$KERNEL_DIR"

echo "=== 快速编译 kserial 模块 ==="
echo ""

# 检查配置
if ! grep -q "^CONFIG_KSERIAL=m" .config 2>/dev/null; then
    echo "⚠ 配置不是模块模式，正在更新..."
    sed -i 's/^CONFIG_KSERIAL=y/CONFIG_KSERIAL=m/' .config
    echo "✓ 已更新配置为模块模式"
fi

# 卸载旧模块（如果存在）
echo ""
echo "1. 卸载旧模块（如果存在）..."
sudo rmmod kserial_procfs 2>/dev/null || true
sudo rmmod kserial_block 2>/dev/null || true
sudo rmmod kserial 2>/dev/null || true
echo "✓ 完成"

# 准备构建环境
echo ""
echo "2. 准备构建环境..."
if [ ! -d include/generated ]; then
    echo "   运行 make prepare..."
    make prepare >/dev/null 2>&1 || {
        echo "   ⚠ make prepare 失败，尝试完整准备..."
        make prepare
    }
fi

# 检查是否需要先编译 vmlinux（用于生成 Module.symvers）
if [ ! -f Module.symvers ] || [ ! -f vmlinux ]; then
    echo "   编译 vmlinux 以生成符号表..."
    make -j$(nproc) vmlinux >/dev/null 2>&1 || {
        echo "   ⚠ vmlinux 编译失败，尝试继续..."
    }
fi
echo "   ✓ 构建环境就绪"

# 编译模块
echo ""
echo "3. 编译 kserial 模块..."
make -j$(nproc) M=kernel modules 2>&1 | tee /tmp/kserial_build.log | \
    grep -E "(kserial|Error|error|Building|CC|LD|WARNING)" || true

# 查找编译好的模块
KSERIAL_KO=$(find kernel -name "kserial.ko" -type f 2>/dev/null | head -1)
KSERIAL_BLOCK_KO=$(find kernel -name "kserial_block.ko" -type f 2>/dev/null | head -1)
KSERIAL_PROCFS_KO=$(find kernel -name "kserial_procfs.ko" -type f 2>/dev/null | head -1)

if [ -z "$KSERIAL_PROCFS_KO" ]; then
    echo ""
    echo "⚠ 未找到模块文件，检查编译日志..."
    tail -30 /tmp/kserial_build.log
    exit 1
fi

echo ""
echo "✓ 找到模块文件:"
[ -n "$KSERIAL_KO" ] && echo "  - $KSERIAL_KO"
[ -n "$KSERIAL_BLOCK_KO" ] && echo "  - $KSERIAL_BLOCK_KO"
echo "  - $KSERIAL_PROCFS_KO"

# 加载模块
echo ""
echo "4. 加载模块..."
if [ -n "$KSERIAL_KO" ]; then
    echo "  加载 kserial.ko..."
    sudo insmod "$KSERIAL_KO" 2>&1 | grep -v "File exists" || true
fi

if [ -n "$KSERIAL_BLOCK_KO" ]; then
    echo "  加载 kserial_block.ko..."
    sudo insmod "$KSERIAL_BLOCK_KO" 2>&1 | grep -v "File exists" || true
fi

echo "  加载 kserial_procfs.ko..."
sudo insmod "$KSERIAL_PROCFS_KO" 2>&1 || {
    echo "  ⚠ 加载失败，查看错误:"
    dmesg | tail -10
    exit 1
}

echo "✓ 模块加载成功"

# 验证
echo ""
echo "5. 验证..."
if [ -e /proc/kserial ]; then
    echo "✓ /proc/kserial 存在"
    echo ""
    echo "已加载的模块:"
    lsmod | grep kserial || echo "  (未显示，但已加载)"
else
    echo "⚠ /proc/kserial 不存在"
    echo "最近的 dmesg 输出:"
    dmesg | tail -10 | grep -i kserial || dmesg | tail -5
fi

echo ""
echo "=== 完成 ==="
echo ""
echo "现在可以测试:"
echo "  cd tools/testing/selftests/cgroup"
echo "  sudo ./test_kserial_real --struct mem_cgroup vmstats.state[14] vmstats.state[16] vmstats.state[34]"

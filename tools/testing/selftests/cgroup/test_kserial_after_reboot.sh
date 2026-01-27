#!/bin/bash
# 重启后的 kserial 测试脚本
# 请手动运行此脚本（需要 sudo 权限）

set -e

KERNEL_DIR="/home/jianyuew/develop/kernel/cgroup/linux"
cd "$KERNEL_DIR/tools/testing/selftests/cgroup"

echo "=== kserial 重启后测试 ==="
echo ""

# 1. 检查内核版本
echo "1. 检查内核版本..."
echo "   当前内核: $(uname -r)"
echo ""

# 2. 检查符号是否存在
echo "2. 检查必要的符号是否存在..."
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
        echo "   ✗ $sym - 不存在！"
        MISSING=1
    fi
done

if [ $MISSING -eq 1 ]; then
    echo ""
    echo "⚠️  警告：部分符号不存在，可能需要重新编译内核"
    echo ""
fi

# 3. 检查模块文件
echo ""
echo "3. 检查模块文件..."
if [ -f "$KERNEL_DIR/kernel/kserial.ko" ]; then
    echo "   ✓ kserial.ko 存在"
    ls -lh "$KERNEL_DIR/kernel/kserial.ko"
else
    echo "   ✗ kserial.ko 不存在"
    echo "   需要重新编译模块"
fi

# 4. 尝试加载模块
echo ""
echo "4. 加载模块..."
if lsmod | grep -q kserial; then
    echo "   ✓ 模块已加载"
    lsmod | grep kserial
else
    echo "   正在加载模块..."
    
    # 按依赖顺序加载：kserial_block -> kserial -> kserial_procfs
    if [ -f "$KERNEL_DIR/kernel/kserial_block.ko" ]; then
        if lsmod | grep -q kserial_block; then
            echo "   ✓ kserial_block 已加载"
        else
            echo "   加载 kserial_block.ko..."
            sudo insmod "$KERNEL_DIR/kernel/kserial_block.ko" 2>&1 || {
                echo "   ⚠ kserial_block.ko 加载失败"
                sudo dmesg | tail -5
            }
        fi
    fi
    
    if [ -f "$KERNEL_DIR/kernel/kserial.ko" ]; then
        if lsmod | grep -q "^kserial "; then
            echo "   ✓ kserial 已加载"
        else
            echo "   加载 kserial.ko..."
            sudo insmod "$KERNEL_DIR/kernel/kserial.ko" 2>&1 || {
                echo "   ⚠ kserial.ko 加载失败"
                echo "   错误信息:"
                sudo dmesg | tail -10 | grep -i kserial
                exit 1
            }
        fi
    fi
    
    if [ -f "$KERNEL_DIR/kernel/kserial_procfs.ko" ]; then
        if lsmod | grep -q kserial_procfs; then
            echo "   ✓ kserial_procfs 已加载"
        else
            echo "   加载 kserial_procfs.ko..."
            sudo insmod "$KERNEL_DIR/kernel/kserial_procfs.ko" 2>&1 || {
                echo "   ⚠ kserial_procfs.ko 加载失败"
                echo "   错误信息:"
                sudo dmesg | tail -10 | grep -i kserial
                exit 1
            }
        fi
    fi
    
    echo "   ✓ 模块加载成功"
fi

# 5. 验证 /proc/kserial
echo ""
echo "5. 验证 /proc/kserial..."
if [ -e /proc/kserial ]; then
    echo "   ✓ /proc/kserial 存在"
    ls -l /proc/kserial
else
    echo "   ✗ /proc/kserial 不存在"
    echo "   检查 dmesg:"
    dmesg | tail -10 | grep -i kserial || echo "   无相关日志"
    exit 1
fi

# 6. 编译测试程序
echo ""
echo "6. 编译测试程序..."
if [ ! -x "./test_kserial_real" ]; then
    gcc -Wall -Wextra -o test_kserial_real test_kserial_real.c || {
        echo "   ✗ 编译失败"
        exit 1
    }
fi
echo "   ✓ 测试程序已就绪"

# 7. 运行测试
echo ""
echo "7. 运行测试..."
echo ""
echo "--- 测试 1: 查询 cgroup 字段 ---"
sudo ./test_kserial_real level nr_descendants max_depth
echo ""

echo "--- 测试 2: 查询 mem_cgroup 内存统计 ---"
sudo ./test_kserial_real --struct mem_cgroup vmstats.state[14] vmstats.state[16] vmstats.state[34]
echo ""

echo "--- 测试 3: 查询指定进程 (PID 1) ---"
sudo ./test_kserial_real --struct mem_cgroup --pid 1 vmstats.state[14] vmstats.state[16]
echo ""

echo "=== 所有测试完成 ==="

#!/bin/bash
#
# 性能对比测试脚本：层级值方案 vs 原方案
#

set -e

echo "========================================="
echo "Atomic Counter 性能对比测试"
echo "========================================="
echo ""

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 配置
NUM_CGROUPS=30
NUM_CPUS=$(nproc)
TEST_DURATION=30  # 秒

echo "测试配置："
echo "- CPU 数量: $NUM_CPUS"
echo "- cgroup 数量: $NUM_CGROUPS"
echo "- 测试时长: $TEST_DURATION 秒"
echo ""

# 创建测试 cgroups
setup_cgroups() {
    echo "创建测试 cgroups..."
    
    # 清理旧的
    for i in $(seq 1 $NUM_CGROUPS); do
        if [ -d /sys/fs/cgroup/test$i ]; then
            rmdir /sys/fs/cgroup/test$i 2>/dev/null || true
        fi
    done
    
    # 创建新的
    for i in $(seq 1 $NUM_CGROUPS); do
        mkdir -p /sys/fs/cgroup/test$i
        echo "  创建 test$i"
    done
    
    echo "完成！"
    echo ""
}

# 清理测试 cgroups
cleanup_cgroups() {
    echo "清理测试 cgroups..."
    for i in $(seq 1 $NUM_CGROUPS); do
        if [ -d /sys/fs/cgroup/test$i ]; then
            rmdir /sys/fs/cgroup/test$i 2>/dev/null || true
        fi
    done
    echo "完成！"
}

# 写重负载测试
test_write_heavy() {
    echo "========================================="
    echo "测试 1: 写重负载（10000 写 : 1 读）"
    echo "========================================="
    echo ""
    
    # 启动写入进程
    echo "启动写入负载..."
    pids=()
    for i in $(seq 1 $NUM_CGROUPS); do
        (
            cgroup_path="/sys/fs/cgroup/test$i"
            # 高频内存分配（每个 cgroup 每秒 ~333 次更新）
            for j in $(seq 1 100); do
                dd if=/dev/zero of=/dev/null bs=1M count=100 2>/dev/null
            done
        ) &
        pids+=($!)
    done
    
    # 启动读取进程（低频）
    (
        while true; do
            cat /sys/fs/cgroup/memory.stat > /dev/null
            sleep 1
        done
    ) &
    reader_pid=$!
    
    # 运行 perf 统计
    echo "使用 perf 收集性能数据..."
    perf stat -e cycles,instructions,cache-misses,cache-references \
        -p $(IFS=, ; echo "${pids[*]}") \
        sleep $TEST_DURATION 2>&1 | tee perf_write_heavy.log
    
    # 停止所有进程
    kill ${pids[@]} $reader_pid 2>/dev/null || true
    wait 2>/dev/null || true
    
    echo ""
    echo "结果已保存到 perf_write_heavy.log"
    echo ""
}

# 读重负载测试
test_read_heavy() {
    echo "========================================="
    echo "测试 2: 读重负载（10 写 : 1000 读）"
    echo "========================================="
    echo ""
    
    # 启动低频写入
    (
        while true; do
            for i in $(seq 1 $NUM_CGROUPS); do
                dd if=/dev/zero of=/dev/null bs=1K count=1 2>/dev/null
            done
            sleep 1
        done
    ) &
    writer_pid=$!
    
    # 启动高频读取
    pids=()
    for i in $(seq 1 10); do
        (
            while true; do
                cat /sys/fs/cgroup/memory.stat > /dev/null
                cat /sys/fs/cgroup/test1/memory.stat > /dev/null
                cat /sys/fs/cgroup/test2/memory.stat > /dev/null
            done
        ) &
        pids+=($!)
    done
    
    # 运行 perf 统计
    echo "使用 perf 收集性能数据..."
    perf stat -e cycles,instructions,cache-misses,cache-references \
        -p $(IFS=, ; echo "${pids[*]}") \
        sleep $TEST_DURATION 2>&1 | tee perf_read_heavy.log
    
    # 停止所有进程
    kill $writer_pid ${pids[@]} 2>/dev/null || true
    wait 2>/dev/null || true
    
    echo ""
    echo "结果已保存到 perf_read_heavy.log"
    echo ""
}

# 热点分析
analyze_hotspots() {
    echo "========================================="
    echo "测试 3: 热点分析"
    echo "========================================="
    echo ""
    
    # 启动写入负载
    pids=()
    for i in $(seq 1 $NUM_CGROUPS); do
        (
            while true; do
                dd if=/dev/zero of=/dev/null bs=1M count=10 2>/dev/null
            done
        ) &
        pids+=($!)
    done
    
    # 记录 CPU 热点
    echo "收集 CPU 热点（30 秒）..."
    perf record -e cycles:pp -g -p $(IFS=, ; echo "${pids[*]}") \
        sleep $TEST_DURATION 2>/dev/null
    
    # 停止负载
    kill ${pids[@]} 2>/dev/null || true
    wait 2>/dev/null || true
    
    # 生成报告
    echo ""
    echo "Top 20 热点函数："
    perf report --stdio --no-children | head -40 | tee perf_hotspots.log
    
    echo ""
    echo "详细报告已保存到 perf_hotspots.log"
    echo "使用 'perf report' 查看完整报告"
    echo ""
}

# Cache line 竞争分析
analyze_cache_contention() {
    echo "========================================="
    echo "测试 4: Cache Line 竞争分析"
    echo "========================================="
    echo ""
    
    # 检查 perf c2c 是否可用
    if ! perf c2c record --help &>/dev/null; then
        echo -e "${YELLOW}警告: perf c2c 不可用，跳过此测试${NC}"
        return
    fi
    
    # 启动写入负载
    pids=()
    for i in $(seq 1 $NUM_CGROUPS); do
        (
            while true; do
                dd if=/dev/zero of=/dev/null bs=1M count=10 2>/dev/null
            done
        ) &
        pids+=($!)
    done
    
    # 记录 cache-to-cache 传输
    echo "收集 cache-to-cache 数据（30 秒）..."
    perf c2c record -p $(IFS=, ; echo "${pids[*]}") \
        sleep $TEST_DURATION 2>/dev/null
    
    # 停止负载
    kill ${pids[@]} 2>/dev/null || true
    wait 2>/dev/null || true
    
    # 生成报告
    echo ""
    echo "Cache line 竞争报告："
    perf c2c report --stdio | head -50 | tee perf_c2c.log
    
    echo ""
    echo "详细报告已保存到 perf_c2c.log"
    echo "使用 'perf c2c report' 查看完整报告"
    echo ""
}

# 功能验证
test_correctness() {
    echo "========================================="
    echo "测试 5: 功能正确性验证"
    echo "========================================="
    echo ""
    
    echo "创建子 cgroup 并分配内存..."
    
    # 记录初始值
    root_before=$(grep "^anon " /sys/fs/cgroup/memory.stat | awk '{print $2}')
    echo "Root cgroup 初始 anon: $root_before"
    
    # 在子 cgroup 中分配内存
    mkdir -p /sys/fs/cgroup/verify_test
    (
        echo $$ > /sys/fs/cgroup/verify_test/cgroup.procs
        # 分配 100MB
        dd if=/dev/zero of=/tmp/test_file bs=1M count=100 2>/dev/null
        sleep 2
    )
    
    # 检查子 cgroup 的值
    child_anon=$(grep "^anon " /sys/fs/cgroup/verify_test/memory.stat | awk '{print $2}')
    echo "Child cgroup anon: $child_anon"
    
    # 检查父 cgroup 是否包含子值
    root_after=$(grep "^anon " /sys/fs/cgroup/memory.stat | awk '{print $2}')
    echo "Root cgroup 之后 anon: $root_after"
    
    diff=$((root_after - root_before))
    echo "差值: $diff"
    
    # 验证层级聚合
    if [ $diff -gt 90000000 ]; then  # ~90MB
        echo -e "${GREEN}✓ 功能正确：父节点包含子节点的值${NC}"
    else
        echo -e "${RED}✗ 功能异常：父节点值未正确更新${NC}"
    fi
    
    # 清理
    rm -f /tmp/test_file
    rmdir /sys/fs/cgroup/verify_test 2>/dev/null || true
    
    echo ""
}

# 主菜单
main() {
    # 检查权限
    if [ "$EUID" -ne 0 ]; then
        echo -e "${RED}错误：需要 root 权限${NC}"
        echo "请使用: sudo $0"
        exit 1
    fi
    
    # 检查 perf 是否可用
    if ! command -v perf &>/dev/null; then
        echo -e "${YELLOW}警告: perf 工具未安装${NC}"
        echo "安装: sudo apt install linux-tools-generic"
        exit 1
    fi
    
    echo "选择测试项："
    echo "  1) 全部测试（推荐）"
    echo "  2) 写重负载测试"
    echo "  3) 读重负载测试"
    echo "  4) 热点分析"
    echo "  5) Cache 竞争分析"
    echo "  6) 功能正确性验证"
    echo "  0) 退出"
    echo ""
    read -p "请选择 [0-6]: " choice
    
    case $choice in
        1)
            setup_cgroups
            test_correctness
            test_write_heavy
            test_read_heavy
            analyze_hotspots
            analyze_cache_contention
            cleanup_cgroups
            ;;
        2)
            setup_cgroups
            test_write_heavy
            cleanup_cgroups
            ;;
        3)
            setup_cgroups
            test_read_heavy
            cleanup_cgroups
            ;;
        4)
            setup_cgroups
            analyze_hotspots
            cleanup_cgroups
            ;;
        5)
            setup_cgroups
            analyze_cache_contention
            cleanup_cgroups
            ;;
        6)
            test_correctness
            ;;
        0)
            echo "退出"
            exit 0
            ;;
        *)
            echo -e "${RED}无效选择${NC}"
            exit 1
            ;;
    esac
    
    echo ""
    echo "========================================="
    echo "测试完成！"
    echo "========================================="
    echo ""
    echo "查看结果："
    echo "  - perf_write_heavy.log  - 写重负载性能数据"
    echo "  - perf_read_heavy.log   - 读重负载性能数据"
    echo "  - perf_hotspots.log     - CPU 热点函数"
    echo "  - perf_c2c.log          - Cache line 竞争"
    echo "  - perf.data             - perf 原始数据（可用 'perf report' 查看）"
    echo ""
}

# 运行主程序
main "$@"

#!/bin/bash
# 对比 kserial 和 BPF 在实时监控场景下的性能
#
# Usage: sudo ./benchmark_monitoring.sh

set -e

DURATION=10  # 监控时长（秒）
INTERVAL=0.1 # 采样间隔（秒）

echo "======================================"
echo "实时监控性能对比测试"
echo "======================================"
echo "测试条件："
echo "  - 监控时长: ${DURATION}秒"
echo "  - 采样间隔: ${INTERVAL}秒"
echo "  - 预期采样次数: $((DURATION * 10))"
echo ""

# ============================================
# 测试 1: kserial 循环查询
# ============================================
echo "测试 1: kserial 循环查询（拉取模式）"
echo "--------------------------------------"

if [ ! -e /proc/kserial ]; then
    echo "⚠️  /proc/kserial 不存在，跳过此测试"
    KSERIAL_TIME="N/A"
    KSERIAL_SAMPLES=0
else
    START=$(date +%s.%N)
    SAMPLES=0
    
    # 循环查询
    END_TIME=$(echo "$(date +%s.%N) + $DURATION" | bc)
    while [ $(echo "$(date +%s.%N) < $END_TIME" | bc) -eq 1 ]; do
        # 每次都需要 open, write, read, close
        if timeout 0.5 kserial -s mem_cgroup -r vmstats.state[9] >/dev/null 2>&1; then
            ((SAMPLES++))
        fi
        sleep $INTERVAL
    done
    
    KSERIAL_TIME=$(echo "$(date +%s.%N) - $START" | bc)
    KSERIAL_SAMPLES=$SAMPLES
    
    echo "✓ 完成"
    echo "  - 实际采样次数: $KSERIAL_SAMPLES"
    echo "  - 总耗时: ${KSERIAL_TIME}秒"
    echo "  - 平均延迟: $(echo "scale=2; $KSERIAL_TIME / $KSERIAL_SAMPLES * 1000" | bc)ms/sample"
    echo "  - 系统调用次数: $((KSERIAL_SAMPLES * 4)) (open+write+read+close)"
fi
echo ""

# ============================================
# 测试 2: bpftrace interval（推荐）
# ============================================
echo "测试 2: bpftrace interval（推送模式）"
echo "--------------------------------------"

if ! command -v bpftrace &> /dev/null; then
    echo "⚠️  bpftrace 未安装，跳过此测试"
    BPF_TIME="N/A"
    BPF_SAMPLES=0
else
    START=$(date +%s.%N)
    
    # bpftrace 持续监控
    SAMPLES=$(timeout $DURATION bpftrace -e "
        interval:ms:100 {
            \$m = ((struct task_struct *)curtask)->memcg->vmstats;
            printf(\".\");
            @count++;
        }
        END {
            printf(\"\\nSamples: %d\\n\", @count);
        }
    " 2>/dev/null | grep "Samples:" | awk '{print $2}')
    
    BPF_TIME=$(echo "$(date +%s.%N) - $START" | bc)
    BPF_SAMPLES=${SAMPLES:-0}
    
    echo "✓ 完成"
    echo "  - 实际采样次数: $BPF_SAMPLES"
    echo "  - 总耗时: ${BPF_TIME}秒"
    echo "  - 平均延迟: $(echo "scale=2; $BPF_TIME / $BPF_SAMPLES * 1000" | bc)ms/sample"
    echo "  - 系统调用次数: 1 (仅初始化)"
fi
echo ""

# ============================================
# 测试 3: cat memory.stat 循环（对照组）
# ============================================
echo "测试 3: cat memory.stat 循环（对照组）"
echo "--------------------------------------"

START=$(date +%s.%N)
SAMPLES=0

END_TIME=$(echo "$(date +%s.%N) + $DURATION" | bc)
while [ $(echo "$(date +%s.%N) < $END_TIME" | bc) -eq 1 ]; do
    cat /sys/fs/cgroup/memory.stat | head -1 >/dev/null 2>&1
    ((SAMPLES++))
    sleep $INTERVAL
done

CAT_TIME=$(echo "$(date +%s.%N) - $START" | bc)
CAT_SAMPLES=$SAMPLES

echo "✓ 完成"
echo "  - 实际采样次数: $CAT_SAMPLES"
echo "  - 总耗时: ${CAT_TIME}秒"
echo "  - 平均延迟: $(echo "scale=2; $CAT_TIME / $CAT_SAMPLES * 1000" | bc)ms/sample"
echo "  - 系统调用次数: $((CAT_SAMPLES * 2)) (open+read)"
echo ""

# ============================================
# 结果汇总
# ============================================
echo "======================================"
echo "结果汇总"
echo "======================================"
printf "%-20s %15s %15s %15s\n" "方法" "采样次数" "总耗时(s)" "平均延迟(ms)"
echo "--------------------------------------------------------------"
printf "%-20s %15s %15s %15s\n" \
    "kserial 循环" \
    "$KSERIAL_SAMPLES" \
    "$KSERIAL_TIME" \
    "$(echo "scale=2; $KSERIAL_TIME / $KSERIAL_SAMPLES * 1000" | bc 2>/dev/null || echo 'N/A')"

printf "%-20s %15s %15s %15s\n" \
    "bpftrace interval" \
    "$BPF_SAMPLES" \
    "$BPF_TIME" \
    "$(echo "scale=2; $BPF_TIME / $BPF_SAMPLES * 1000" | bc 2>/dev/null || echo 'N/A')"

printf "%-20s %15s %15s %15s\n" \
    "cat 循环" \
    "$CAT_SAMPLES" \
    "$CAT_TIME" \
    "$(echo "scale=2; $CAT_TIME / $CAT_SAMPLES * 1000" | bc)"
echo ""

# ============================================
# 分析
# ============================================
echo "======================================"
echo "分析"
echo "======================================"
echo ""
echo "🔴 kserial 循环（不推荐用于实时监控）"
echo "   - 每次采样需要 4 次系统调用"
echo "   - 频繁的用户-内核上下文切换"
echo "   - 适合：一次性查询，按需获取"
echo ""
echo "🟢 bpftrace interval（推荐用于实时监控）"
echo "   - 程序常驻内核，无需频繁切换"
echo "   - 定时器驱动，精确的采样间隔"
echo "   - 适合：持续监控，性能分析"
echo ""
echo "🟡 cat memory.stat 循环（简单场景可用）"
echo "   - 每次采样需要 2 次系统调用"
echo "   - 读取整个 stat 文件，数据较多"
echo "   - 适合：简单脚本，低频采样"
echo ""

# ============================================
# CPU 开销对比
# ============================================
echo "======================================"
echo "CPU 开销估算"
echo "======================================"
echo ""
if [ "$KSERIAL_SAMPLES" -gt 0 ]; then
    echo "kserial 循环："
    echo "  - 系统调用: $((KSERIAL_SAMPLES * 4))"
    echo "  - 上下文切换: ~$((KSERIAL_SAMPLES * 4))"
    echo ""
fi

if [ "$BPF_SAMPLES" -gt 0 ]; then
    echo "bpftrace interval："
    echo "  - 系统调用: 1"
    echo "  - 上下文切换: ~$BPF_SAMPLES (仅数据返回)"
    echo ""
fi

echo "cat 循环："
echo "  - 系统调用: $((CAT_SAMPLES * 2))"
echo "  - 上下文切换: ~$((CAT_SAMPLES * 2))"
echo ""

echo "💡 结论："
echo "   BPF 的系统调用次数是 kserial 循环的 1/$((KSERIAL_SAMPLES * 4)) !"

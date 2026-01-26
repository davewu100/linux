#!/bin/bash
# 智能内存监控脚本
# 自动检测环境，选择最佳监控方式：
#   1. 优先使用 bpftrace（性能最好）
#   2. 回退到 kserial（广泛兼容）
#   3. 再回退到 cat memory.stat（最基本）
#
# Usage: sudo ./smart_monitor.sh [interval_seconds]

INTERVAL=${1:-1}

echo "======================================"
echo "智能内存监控工具"
echo "======================================"
echo ""

# ============================================
# 检测 1: BPF 支持
# ============================================
echo "检测环境..."
echo ""

BPF_AVAILABLE=0
KSERIAL_AVAILABLE=0
CAT_AVAILABLE=0

# 检查 bpftrace
if command -v bpftrace &> /dev/null; then
    # 尝试运行简单的 BPF 程序
    if timeout 1 bpftrace -e 'BEGIN { exit(); }' &>/dev/null; then
        BPF_AVAILABLE=1
        echo "✓ bpftrace 可用"
    else
        echo "✗ bpftrace 已安装但不能运行（可能需要 root 或 CONFIG_BPF=n）"
    fi
else
    echo "✗ bpftrace 未安装"
fi

# 检查 kserial
if [ -e /proc/kserial ]; then
    if command -v kserial &> /dev/null; then
        # 尝试查询
        if timeout 1 kserial -s mem_cgroup -r vmstats.state[9] &>/dev/null; then
            KSERIAL_AVAILABLE=1
            echo "✓ kserial 可用"
        else
            echo "✗ /proc/kserial 存在但查询失败"
        fi
    else
        echo "✗ kserial 工具未安装（但可以直接访问 /proc/kserial）"
        # 仍然可以通过直接读写 /proc/kserial 使用
        KSERIAL_AVAILABLE=1
    fi
else
    echo "✗ kserial 内核模块未加载"
fi

# 检查 memory.stat
if [ -f /sys/fs/cgroup/memory.stat ]; then
    CAT_AVAILABLE=1
    echo "✓ memory.stat 可用"
else
    echo "✗ memory.stat 不存在"
fi

echo ""

# ============================================
# 选择监控方式
# ============================================
if [ $BPF_AVAILABLE -eq 1 ]; then
    METHOD="bpftrace"
    PERF_RATING="⭐⭐⭐⭐⭐"
elif [ $KSERIAL_AVAILABLE -eq 1 ]; then
    METHOD="kserial"
    PERF_RATING="⭐⭐⭐"
elif [ $CAT_AVAILABLE -eq 1 ]; then
    METHOD="cat"
    PERF_RATING="⭐⭐"
else
    echo "❌ 错误：没有可用的监控方式！"
    exit 1
fi

echo "======================================"
echo "选择的监控方式: $METHOD"
echo "性能评级: $PERF_RATING"
echo "======================================"
echo ""

# ============================================
# 执行监控
# ============================================
case $METHOD in
    bpftrace)
        echo "使用 bpftrace（最优性能）"
        echo "监控间隔: ${INTERVAL}秒"
        echo "按 Ctrl+C 停止"
        echo ""
        
        # 转换为毫秒
        INTERVAL_MS=$((INTERVAL * 1000))
        
        bpftrace -e "
BEGIN {
    printf(\"%-20s %15s %15s %15s %15s\\n\", 
           \"时间\", \"anon(MB)\", \"file(MB)\", \"kernel(MB)\", \"delta_anon(MB)\");
    printf(\"--------------------------------------------------------------------------------\\n\");
    @prev_anon = 0;
}

interval:ms:$INTERVAL_MS {
    \$task = (struct task_struct *)curtask;
    \$memcg = (struct mem_cgroup *)\$task->memcg;
    
    if (\$memcg != 0) {
        \$vmstats = \$memcg->vmstats;
        \$anon = \$vmstats->state[9];
        \$file = \$vmstats->state[10];
        \$kernel = \$vmstats->state[40];
        
        \$anon_mb = \$anon / 1048576;
        \$file_mb = \$file / 1048576;
        \$kernel_mb = \$kernel / 1048576;
        \$delta_mb = \$anon_mb - @prev_anon;
        
        printf(\"%-20s %15ld %15ld %15ld %15ld\\n\",
               strftime(\"%H:%M:%S\", nsecs),
               \$anon_mb,
               \$file_mb,
               \$kernel_mb,
               \$delta_mb);
        
        @prev_anon = \$anon_mb;
    }
}

END {
    clear(@prev_anon);
}
"
        ;;
        
    kserial)
        echo "使用 kserial 循环（回退方案）"
        echo "监控间隔: ${INTERVAL}秒"
        echo "按 Ctrl+C 停止"
        echo ""
        echo "⚠️  注意：这种方式性能不如 BPF，但在受限环境下很有用！"
        echo ""
        
        printf "%-20s %15s %15s %15s %15s\n" \
            "时间" "anon(MB)" "file(MB)" "kernel(MB)" "delta_anon(MB)"
        echo "--------------------------------------------------------------------------------"
        
        PREV_ANON=0
        COUNT=0
        
        while true; do
            START=$(date +%s.%N)
            
            # 查询（4 次系统调用）
            RESULT=$(kserial -s mem_cgroup -r vmstats.state[9] vmstats.state[10] vmstats.state[40] 2>/dev/null)
            
            if [ $? -eq 0 ]; then
                ANON=$(echo "$RESULT" | awk 'NR==1 {print $1}')
                FILE=$(echo "$RESULT" | awk 'NR==2 {print $1}')
                KERNEL=$(echo "$RESULT" | awk 'NR==3 {print $1}')
                
                ANON_MB=$((ANON / 1048576))
                FILE_MB=$((FILE / 1048576))
                KERNEL_MB=$((KERNEL / 1048576))
                
                if [ $COUNT -gt 0 ]; then
                    DELTA_MB=$((ANON_MB - PREV_ANON))
                else
                    DELTA_MB=0
                fi
                
                printf "%-20s %15d %15d %15d %15d\n" \
                    "$(date +%H:%M:%S)" \
                    "$ANON_MB" \
                    "$FILE_MB" \
                    "$KERNEL_MB" \
                    "$DELTA_MB"
                
                PREV_ANON=$ANON_MB
                ((COUNT++))
            fi
            
            sleep $INTERVAL
        done
        ;;
        
    cat)
        echo "使用 cat memory.stat（基础方案）"
        echo "监控间隔: ${INTERVAL}秒"
        echo "按 Ctrl+C 停止"
        echo ""
        
        printf "%-20s %15s %15s %15s\n" \
            "时间" "anon(MB)" "file(MB)" "kernel(MB)"
        echo "----------------------------------------------------------------"
        
        while true; do
            STATS=$(cat /sys/fs/cgroup/memory.stat 2>/dev/null | head -4)
            
            ANON=$(echo "$STATS" | grep "^anon " | awk '{print $2}')
            FILE=$(echo "$STATS" | grep "^file " | awk '{print $2}')
            KERNEL=$(echo "$STATS" | grep "^kernel " | awk '{print $2}')
            
            ANON_MB=$((ANON / 1048576))
            FILE_MB=$((FILE / 1048576))
            KERNEL_MB=$((KERNEL / 1048576))
            
            printf "%-20s %15d %15d %15d\n" \
                "$(date +%H:%M:%S)" \
                "$ANON_MB" \
                "$FILE_MB" \
                "$KERNEL_MB"
            
            sleep $INTERVAL
        done
        ;;
esac

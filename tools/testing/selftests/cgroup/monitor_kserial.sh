#!/bin/bash
# 使用 kserial 做实时监控（演示为什么不推荐）
#
# Usage: sudo ./monitor_kserial.sh [interval_seconds]

INTERVAL=${1:-1}  # 默认 1 秒采样一次

echo "使用 kserial 实时监控 memory.stat"
echo "采样间隔: ${INTERVAL}秒"
echo "按 Ctrl+C 停止"
echo ""
printf "%-20s %15s %15s %15s %15s\n" "时间" "anon(MB)" "file(MB)" "kernel(MB)" "delta_anon(MB)"
echo "--------------------------------------------------------------------------------"

PREV_ANON=0
COUNT=0

while true; do
    # 每次循环都需要：
    # 1. open /proc/kserial
    # 2. write schema
    # 3. read result  
    # 4. close
    
    START=$(date +%s.%N)
    
    # 查询 anon, file, kernel
    RESULT=$(kserial -s mem_cgroup -r vmstats.state[9] vmstats.state[10] vmstats.state[40] 2>/dev/null)
    
    if [ $? -eq 0 ]; then
        # 解析结果
        ANON=$(echo "$RESULT" | awk 'NR==1 {print $1}')
        FILE=$(echo "$RESULT" | awk 'NR==2 {print $1}')
        KERNEL=$(echo "$RESULT" | awk 'NR==3 {print $1}')
        
        # 转换为 MB
        ANON_MB=$((ANON / 1048576))
        FILE_MB=$((FILE / 1048576))
        KERNEL_MB=$((KERNEL / 1048576))
        
        # 计算变化量
        if [ $COUNT -gt 0 ]; then
            DELTA_MB=$((ANON_MB - PREV_ANON))
        else
            DELTA_MB=0
        fi
        
        END=$(date +%s.%N)
        QUERY_TIME=$(echo "$END - $START" | bc)
        
        # 输出
        printf "%-20s %15d %15d %15d %15d (%.3fs)\n" \
            "$(date +%H:%M:%S)" \
            "$ANON_MB" \
            "$FILE_MB" \
            "$KERNEL_MB" \
            "$DELTA_MB" \
            "$QUERY_TIME"
        
        PREV_ANON=$ANON_MB
        ((COUNT++))
    else
        echo "Error: kserial query failed"
    fi
    
    # 等待下一次采样
    # 注意：这里的睡眠时间不准确，因为查询本身需要时间
    sleep $INTERVAL
done

#!/bin/bash
# 使用 bpftrace 做实时监控（推荐方式）
#
# Usage: sudo ./monitor_bpftrace.sh [interval_ms]

INTERVAL_MS=${1:-1000}  # 默认 1000ms = 1秒

echo "使用 bpftrace 实时监控 memory.stat"
echo "采样间隔: ${INTERVAL_MS}ms"
echo "按 Ctrl+C 停止"
echo ""

# bpftrace 程序
sudo bpftrace -e "
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
        
        // 读取值（直接在内核态！无需系统调用！）
        \$anon = \$vmstats->state[9];
        \$file = \$vmstats->state[10];
        \$kernel = \$vmstats->state[40];
        
        // 转换为 MB
        \$anon_mb = \$anon / 1048576;
        \$file_mb = \$file / 1048576;
        \$kernel_mb = \$kernel / 1048576;
        
        // 计算变化
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

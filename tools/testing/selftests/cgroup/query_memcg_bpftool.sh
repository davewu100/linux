#!/bin/bash
# Query mem_cgroup stats using bpftool + bpftrace combo
#
# Usage: sudo ./query_memcg_bpftool.sh

echo "Method 1: Direct bpftrace one-liner"
echo "===================================="
sudo bpftrace -e 'BEGIN { 
    $m = ((struct task_struct *)curtask)->memcg->vmstats; 
    printf("anon           %15ld\n", $m->state[9]); 
    printf("file           %15ld\n", $m->state[10]); 
    printf("kernel         %15ld\n", $m->state[40]); 
    printf("kernel_stack   %15ld\n", $m->state[20]); 
    exit(); 
}' 2>/dev/null

echo ""
echo "Method 2: Using BTF to explore struct"
echo "======================================"
echo "Available mem_cgroup fields:"
bpftool btf dump file /sys/kernel/btf/vmlinux format c | grep -A 30 "struct mem_cgroup {"

echo ""
echo "Method 3: Compare with actual memory.stat"
echo "=========================================="
if [ -f /sys/fs/cgroup/memory.stat ]; then
    head -10 /sys/fs/cgroup/memory.stat
else
    echo "memory.stat not found"
fi

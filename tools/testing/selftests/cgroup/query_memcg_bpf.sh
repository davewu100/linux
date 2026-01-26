#!/bin/bash
# Query mem_cgroup stats using BPF
# 
# Usage: sudo ./query_memcg_bpf.sh [anon|file|kernel|kernel_stack|all]

STAT="${1:-all}"

# BPF program to read mem_cgroup stats
read -r -d '' BPF_PROG << 'EOF'
#include <linux/sched.h>

// Approximate enum indices (need to verify with actual kernel)
#define NR_ANON_MAPPED      9
#define NR_FILE_PAGES       10
#define NR_KERNEL_STACK_KB  20
#define MEMCG_KMEM          40

BEGIN {
    $task = (struct task_struct *)curtask;
    $memcg = (struct mem_cgroup *)$task->memcg;
    
    if ($memcg != 0) {
        $vmstats = $memcg->vmstats;
        
        printf("anon=%ld\n", $vmstats->state[NR_ANON_MAPPED]);
        printf("file=%ld\n", $vmstats->state[NR_FILE_PAGES]);
        printf("kernel=%ld\n", $vmstats->state[MEMCG_KMEM]);
        printf("kernel_stack=%ld\n", $vmstats->state[NR_KERNEL_STACK_KB]);
    }
    exit();
}
EOF

# Run bpftrace
if command -v bpftrace &> /dev/null; then
    echo "$BPF_PROG" | sudo bpftrace - 2>/dev/null
else
    echo "Error: bpftrace not installed"
    echo "Install with: sudo apt-get install bpftrace"
    exit 1
fi

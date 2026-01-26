#!/usr/bin/env python3
"""
Query memory cgroup statistics using BCC (BPF Compiler Collection)

Requirements:
    pip3 install bcc

Usage:
    sudo python3 query_memcg_bcc.py
"""

from bcc import BPF
import sys

# BPF program
bpf_text = """
#include <linux/sched.h>
#include <linux/memcontrol.h>

struct memcg_stat {
    u64 anon;
    u64 file;
    u64 kernel;
    u64 kernel_stack;
    u64 pagetables;
    u64 sock;
};

BPF_PERF_OUTPUT(stats);

int get_memcg_stats(struct pt_regs *ctx) {
    struct task_struct *task = (struct task_struct *)bpf_get_current_task();
    struct mem_cgroup *memcg = task->memcg;
    
    if (!memcg || !memcg->vmstats) {
        return 0;
    }
    
    struct memcg_stat stat = {};
    
    // Read vmstats.state array
    // Note: These indices are approximate
    bpf_probe_read_kernel(&stat.anon, sizeof(u64), &memcg->vmstats->state[9]);
    bpf_probe_read_kernel(&stat.file, sizeof(u64), &memcg->vmstats->state[10]);
    bpf_probe_read_kernel(&stat.kernel, sizeof(u64), &memcg->vmstats->state[40]);
    bpf_probe_read_kernel(&stat.kernel_stack, sizeof(u64), &memcg->vmstats->state[20]);
    bpf_probe_read_kernel(&stat.pagetables, sizeof(u64), &memcg->vmstats->state[25]);
    bpf_probe_read_kernel(&stat.sock, sizeof(u64), &memcg->vmstats->state[41]);
    
    stats.perf_submit(ctx, &stat, sizeof(stat));
    return 0;
}
"""

def print_stat(cpu, data, size):
    """Print memory cgroup statistics"""
    event = b["stats"].event(data)
    print("Memory cgroup statistics:")
    print("─────────────────────────")
    print(f"anon           {event.anon:>15,} bytes")
    print(f"file           {event.file:>15,} bytes")
    print(f"kernel         {event.kernel:>15,} bytes")
    print(f"kernel_stack   {event.kernel_stack:>15,} bytes")
    print(f"pagetables     {event.pagetables:>15,} bytes")
    print(f"sock           {event.sock:>15,} bytes")
    sys.exit(0)

if __name__ == "__main__":
    # Load BPF program
    try:
        b = BPF(text=bpf_text)
        
        # Attach to a tracepoint (we'll trigger it immediately)
        b.attach_tracepoint(tp="syscalls:sys_enter_getpid", fn_name="get_memcg_stats")
        
        # Open perf buffer
        b["stats"].open_perf_buffer(print_stat)
        
        # Trigger the tracepoint by calling getpid
        import os
        os.getpid()
        
        # Poll for events
        b.perf_buffer_poll(timeout=1000)
        
        print("No data received")
        
    except KeyboardInterrupt:
        pass
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        print("\nMake sure BCC is installed:", file=sys.stderr)
        print("  Ubuntu/Debian: sudo apt-get install bpfcc-tools python3-bpfcc", file=sys.stderr)
        print("  Fedora/RHEL:   sudo dnf install bcc-tools python3-bcc", file=sys.stderr)
        sys.exit(1)

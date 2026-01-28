# Kernel Memory Stat Flush Status

## Current Status (2026-01-27)

### Problem
The `kernel` memory statistic (MEMCG_KMEM) from k-serial consistently returns 0, while `memory.stat` shows a non-zero value (e.g., ~371MB).

### Investigation Summary

1. **Stat Storage**: `MEMCG_KMEM` is stored in `vmstats.state[34]` (index from `memcg_stats_index(MEMCG_KMEM)`).

2. **Flush Mechanism**: 
   - `memory.stat` automatically calls `mem_cgroup_flush_stats()` which triggers `css_rstat_flush()`.
   - The flush aggregates per-CPU stats via `mem_cgroup_css_rstat_flush()` callback.
   - This callback calls `mem_cgroup_stat_aggregate()` to merge per-CPU counters into `vmstats->state[]`.

3. **Current Implementation**:
   - Modified `kserial_procfs.c` to call `mem_cgroup_flush_stats()` before querying.
   - Additionally, directly invokes `css_rstat_flush` callback for all CPUs to bypass threshold checks.
   - Code location: `kernel/kserial_procfs.c:172-183`

4. **Test Results**:
   - `anon`, `file`, and `kernel_stack` statistics match correctly.
   - `kernel` (MEMCG_KMEM) consistently returns 0 from k-serial.
   - `memory.stat` shows correct non-zero values.

### Code Changes

**File**: `kernel/kserial_procfs.c`

```c
/* Force flush stats before reading to ensure we get up-to-date values */
/* First call mem_cgroup_flush_stats to trigger update tracking, then force flush */
{
    struct mem_cgroup *memcg = (struct mem_cgroup *)target_struct;
    struct cgroup_subsys_state *css = &memcg->css;
    int cpu;
    
    /* Call mem_cgroup_flush_stats first to ensure stats are marked for update */
    mem_cgroup_flush_stats(memcg);
    
    /* Then directly flush all CPUs to ensure complete aggregation */
    /* This ensures we get all per-CPU stats, including those not yet marked */
    if (css->ss && css->ss->css_rstat_flush) {
        for_each_possible_cpu(cpu) {
            css->ss->css_rstat_flush(css, cpu);
        }
    }
}
```

### Possible Causes

1. **NMI-Safe Accounting**: If `CONFIG_MEMCG_NMI_SAFETY_REQUIRES_ATOMIC` is enabled, `MEMCG_KMEM` may be stored in `memcg->kmem_stat` atomic variable and flushed via `flush_nmi_stats()`. However, this config is not enabled in the current kernel.

2. **Per-CPU Aggregation Timing**: The per-CPU stats may not be fully aggregated when k-serial reads them, even after explicit flush calls.

3. **Stat Update Tracking**: The rstat update tracking mechanism may not mark `MEMCG_KMEM` updates correctly, causing them to be skipped during flush.

4. **Different Stat Paths**: `MEMCG_KMEM` might be updated through a different code path that doesn't properly trigger rstat updates.

### Next Steps

1. Verify `CONFIG_MEMCG_NMI_SAFETY_REQUIRES_ATOMIC` configuration.
2. Add debug logging to trace `MEMCG_KMEM` updates and flush operations.
3. Check if `mod_memcg_state(memcg, MEMCG_KMEM, val)` properly triggers `memcg_rstat_updated()`.
4. Investigate if there's a delay or batching mechanism for `MEMCG_KMEM` updates.

### Related Files

- `mm/memcontrol.c`: Memory cgroup implementation
  - `mem_cgroup_css_rstat_flush()`: Flush callback (line 4090)
  - `mem_cgroup_stat_aggregate()`: Per-CPU aggregation (line 4007)
  - `mod_memcg_state()`: Update memcg stats (line 689)
  - `flush_nmi_stats()`: NMI-safe stat flush (line 4044)

- `kernel/kserial_procfs.c`: k-serial procfs interface
  - `ks_proc_write()`: Query handler with stat flush (line 172-183)

- `tools/testing/selftests/cgroup/verify_memcg_stats.sh`: Test script

### Notes

- `memory.stat` reading automatically triggers `mem_cgroup_flush_stats()` via `memcg_stat_format()`.
- The script `verify_memcg_stats.sh` reads `memory.stat` before querying k-serial to ensure stats are flushed.
- Despite explicit flush calls, `MEMCG_KMEM` remains 0 in k-serial output.

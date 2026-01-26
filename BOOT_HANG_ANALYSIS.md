# Atomic Counter Boot Hang - Root Cause Analysis

## Problem Summary

Kernel boot hangs at systemd initialization with atomic counter backend enabled (`CONFIG_MEMCG_ATOMIC_COUNTER=y`).

**Symptom**: System completely freezes during boot, unresponsive to Ctrl+Alt+F2, only Ctrl+Alt+Del works.

## Root Cause

The atomic counter implementation has a **critical initialization order bug** during early boot:

### Issue #1: Early Read Before Initialization Complete

```
Boot Sequence:
1. mem_cgroup_alloc() 
   └─> css_atomic_init()        # Allocates counter, cache marked invalid
2. Some code calls memcg_page_state()
   └─> css_atomic_page_state()  # Cache invalid, triggers flush
       └─> css_atomic_flush()   # Tries to traverse tree
           └─> mem_cgroup_iter() # May iterate incomplete tree
```

**Problem**: If any code reads memcg stats before the cgroup tree is fully initialized, it triggers a tree traversal on an incomplete tree structure, potentially causing:
- Infinite loops (circular references in partial tree)
- NULL pointer dereferences (uninitialized parent/child links)
- Deadlocks (locks not fully initialized)

### Issue #2: Lock Ordering Problem

```c
// mm/memcontrol-atomic.c:265
int css_atomic_online(struct mem_cgroup *memcg)
{
    struct mem_cgroup *parent = parent_mem_cgroup(memcg);
    
    spin_lock(&parent->atomic_children_lock);  // <-- Potential deadlock
    list_add_rcu(&memcg->atomic_sibling, &parent->atomic_children);
    spin_unlock(&parent->atomic_children_lock);
}
```

If `css_atomic_page_state()` is called during the online phase (from interrupt context or another CPU), it may try to acquire the same lock while traversing, causing deadlock.

### Issue #3: Root Cgroup Edge Case

The root cgroup initialization is special:
- Root memcg is created very early in boot
- Many subsystems immediately start reading stats from root
- But atomic_cache is initialized with `valid = false`
- First read forces flush, but tree traversal may not work correctly for root

## Evidence from Code

### Problematic Call Path

```c
// kernel/cgroup/atomic.c:612
for (iter = memcg; iter; iter = mem_cgroup_iter(memcg, iter, NULL)) {
    ret = visit(iter, arg);  // Visit each cgroup in tree
    if (unlikely(ret))
        return ret;
}
```

`mem_cgroup_iter()` is a complex iterator that:
- Tracks position in tree traversal
- Handles concurrent modifications
- **Assumes tree is properly initialized**

### Cache Invalidation Issue

```c
// kernel/cgroup/atomic.c:124-126
if (!memcg->atomic_cache || !memcg->atomic_cache->valid)
    force = true;
css_atomic_flush(memcg, force);  // <-- Forces expensive tree walk
```

Every first read after boot will force a tree traversal, but at boot time this is dangerous.

## Why It Hangs at "snap.seeded.service"

The hang appears to occur at snap.seeded.service because:
1. This is when systemd starts doing more aggressive cgroup management
2. Snap services likely trigger memcg stat reads (memory accounting)
3. These reads hit the uninitialized cache → force flush → bad tree traversal
4. System hangs in `mem_cgroup_iter()` infinite loop or deadlock

## Immediate Fix

**Option 1: Disable Atomic Counter (Safe)**
```bash
cd /home/jianyuew/repo/tmp/linux
bash fix_atomic_counter.sh
```

**Option 2: Boot Old Kernel**
- Select previous kernel in GRUB Advanced Options
- System will boot normally with rstat backend

## Long-term Fix (Code Changes Required)

### Fix #1: Guard Against Early Reads

```c
// kernel/cgroup/atomic.c:99
u64 css_atomic_page_state(struct mem_cgroup *memcg, int idx, bool force)
{
    // NEW: Check if memcg is fully online before allowing tree traversal
    if (!mem_cgroup_online(memcg)) {
        // During initialization, return local counter only (no tree walk)
        struct memcg_atomic_counter *counter = READ_ONCE(memcg->atomic_counter);
        if (counter)
            return atomic64_read(&counter->state[memcg_stats_index(idx)]);
        return 0;
    }
    
    // ... rest of existing code
}
```

### Fix #2: Lazy Cache Initialization

```c
// mm/memcontrol-atomic.c:186
int css_atomic_init(struct mem_cgroup *memcg)
{
    // ... existing allocation code ...
    
    // NEW: For root memcg, pre-populate cache to avoid first-read flush
    if (mem_cgroup_is_root(memcg)) {
        memcg->atomic_cache->valid = true;  // Skip first flush
        // Initialize all stats to 0
        memset(memcg->atomic_cache->stats, 0, sizeof(memcg->atomic_cache->stats));
        memset(memcg->atomic_cache->events, 0, sizeof(memcg->atomic_cache->events));
    }
    
    return 0;
}
```

### Fix #3: Add Online State Check to Flush

```c
// kernel/cgroup/atomic.c:297
void css_atomic_flush(struct mem_cgroup *memcg, bool force)
{
    // NEW: Don't flush until memcg is fully online
    if (!mem_cgroup_online(memcg))
        return;
        
    // ... rest of existing code
}
```

### Fix #4: Add Iterator Safety Check

```c
// kernel/cgroup/atomic.c:612
for (iter = memcg; iter; iter = mem_cgroup_iter(memcg, iter, NULL)) {
    // NEW: Skip memcgs that aren't fully initialized
    if (!mem_cgroup_online(iter))
        continue;
        
    ret = visit(iter, arg);
    if (unlikely(ret))
        return ret;
}
```

## Testing Plan (After Fix)

1. **Boot Test**: Verify kernel boots successfully to login prompt
2. **Stress Test**: Create/destroy many cgroups rapidly
3. **Memory Pressure**: Trigger OOM scenarios with atomic counter enabled
4. **Performance Test**: Run memcg_perf tests to verify functionality

## Related Files

- `/home/jianyuew/repo/tmp/linux/kernel/cgroup/atomic.c` - Main atomic counter logic
- `/home/jianyuew/repo/tmp/linux/mm/memcontrol-atomic.c` - Memcg-specific code
- `/home/jianyuew/repo/tmp/linux/mm/memcontrol.c` - Integration points
- `.config` - Currently has `CONFIG_MEMCG_ATOMIC_COUNTER=y`

## Lessons Learned

1. **Never assume tree is initialized**: Always check online state before traversal
2. **First read is critical**: Cache initialization must be safe for early boot
3. **Root cgroup is special**: Needs different handling than normal cgroups
4. **Test in VM first**: Boot issues are hard to debug on bare metal

---

**Status**: Boot hang confirmed, fix script created, awaiting reboot to old kernel
**Next Step**: Boot old kernel → Run fix_atomic_counter.sh → Test rebuild

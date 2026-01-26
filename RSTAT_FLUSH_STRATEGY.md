# Rstat-Like Flush 策略

## ✅ 完全模仿 rstat 的实现

### 核心设计

```
写入：
  1. 更新本地 counter（atomic）
  2. 向上传播 stats_updates++（所有祖先）
  
读取：
  - 内核内部：直接 READ_ONCE(cache)，从不 flush！
  - 用户态：调用 css_atomic_flush()，检查阈值再决定
```

---

## 📊 三种 Flush 函数

### 1. `css_atomic_page_state()` - 内核内部读取

```c
u64 css_atomic_page_state(struct mem_cgroup *memcg, int idx, bool force)
{
    cache = memcg->atomic_cache;
    
    if (force)
        css_atomic_refresh_cache(memcg);  // 强制刷新
    
    return READ_ONCE(cache->stats[idx]);  // 直接读，不检查！
}
```

**调用场景：**
- `memcg_page_state(memcg, idx)` - 内核各处读取统计
- 默认 `force=false`：直接读 cache，不触发 flush
- 只有显式传 `force=true` 才会刷新

**对应 rstat：**
```c
// rstat 的内核读取也是直接读，不 flush
unsigned long memcg_page_state(struct mem_cgroup *memcg, int idx)
{
    x = READ_ONCE(memcg->vmstats->state[i]);  // 直接读
    return x;
}
```

---

### 2. `css_atomic_flush()` - 阈值 + 速率限制

```c
void css_atomic_flush(struct mem_cgroup *memcg, bool force)
{
    if (force) {
        css_atomic_refresh_cache(memcg);  // 立即刷新
        return;
    }
    
    // 检查阈值
    threshold = ATOMIC_FLUSH_THRESHOLD * num_online_cpus();
    updates = atomic_read(&cache->stats_updates);
    
    if (updates < threshold)
        return;  // 未超阈值，不刷新
    
    // 检查速率限制（2秒）
    if (time_before(now, cache->flush_time + ATOMIC_FLUSH_TIME))
        return;  // 刚刷新过，不刷新
    
    // 通过检查，刷新
    css_atomic_refresh_cache(memcg);
}
```

**调用场景：**

| 调用位置 | force | 行为 |
|---------|-------|------|
| `memcg_stat_format` (memory.stat) | false | 检查阈值 + 2s 限速 |
| `memory_numa_stat_show` | false | 检查阈值 + 2s 限速 |
| OOM 路径 | true | 立即刷新 |
| 对比模式 | true | 立即刷新 |

**对应 rstat：**
```c
void __mem_cgroup_flush_stats(struct mem_cgroup *memcg, bool force)
{
    bool needs_flush = memcg_vmstats_needs_flush(memcg->vmstats);
    
    if (!force && !needs_flush)
        return;  // 检查阈值
    
    css_rstat_flush(&memcg->css);  // 实际 flush
}
```

---

### 3. `css_atomic_flush_ratelimited()` - 更严格的速率限制

```c
void css_atomic_flush_ratelimited(struct mem_cgroup *memcg)
{
    threshold = ATOMIC_FLUSH_THRESHOLD * num_online_cpus();
    updates = atomic_read(&cache->stats_updates);
    
    if (updates < threshold)
        return;  // 未超阈值
    
    // 更严格的速率限制（4秒）
    if (time_before(now, cache->flush_time + 2 * ATOMIC_FLUSH_TIME))
        return;  // 需要等 4 秒
    
    css_atomic_refresh_cache(memcg);
}
```

**调用场景：**
- 热路径（如页回收）- Line 3733

**对应 rstat：**
```c
void mem_cgroup_flush_stats_ratelimited(struct mem_cgroup *memcg)
{
    /* Only flush if > 4s (2 * FLUSH_TIME) */
    if (time_after64(jiffies_64, flush_last_time + 2*FLUSH_TIME))
        mem_cgroup_flush_stats(memcg);
}
```

---

## 🎯 读取 memory.stat 和 memory.numa_stat

### memory.stat 读取流程

```
用户：cat /sys/fs/cgroup/memory.stat
  ↓
memory_stat_show()
  ↓
memory_stat_format()
  ↓
css_atomic_flush(memcg, false)  ← 检查阈值和 2s 限速
  ↓
  if (updates < threshold)  return;  // 未超阈值，不刷新
  if (age < 2s)             return;  // 刚刷新过，不刷新
  css_atomic_refresh_cache();        // 满足条件，刷新
  ↓
memcg_page_state() 读取数据
  ↓
返回给用户
```

### 关键点

**不是"总是刷新"，而是"检查后决定"！**

```
场景 1：updates < 阈值
  → 不刷新，返回 cache（可能有点旧）
  
场景 2：updates >= 阈值 且 刚刷新过（< 2s）
  → 不刷新，返回 cache
  
场景 3：updates >= 阈值 且 上次刷新 >= 2s
  → 刷新，返回最新数据
```

---

## 📊 完整对比

| 操作 | rstat | 我们的实现 | 一致性 |
|------|-------|-----------|--------|
| **内核读取** | `READ_ONCE(vmstats)` | `READ_ONCE(cache)` | ✅ 完全一致 |
| **memory.stat** | 检查阈值 | 检查阈值 + 2s | ✅ 一致（多了 2s 限速） |
| **阈值** | `BATCH * cpus` | `64 * cpus` | ✅ 类似 |
| **速率限制** | 无（只在 ratelimited） | 2s（在 flush） | ⚠️ 差异 |
| **ratelimited** | 4s | 4s | ✅ 完全一致 |

### 差异说明

rstat 的 `mem_cgroup_flush_stats()` 没有速率限制，只检查阈值：
```c
void mem_cgroup_flush_stats(struct mem_cgroup *memcg)
{
    __mem_cgroup_flush_stats(memcg, false);
    // 只检查阈值，不检查时间
}
```

我们的 `css_atomic_flush(memcg, false)` 额外加了 2s 速率限制。

**是否需要移除 2s 限速？**

---

## 💡 建议

### 选项 A：完全模仿 rstat（移除 flush 中的 2s 限速）

```c
void css_atomic_flush(struct mem_cgroup *memcg, bool force)
{
    if (force) {
        css_atomic_refresh_cache(memcg);
        return;
    }
    
    // 只检查阈值，不检查时间（像 rstat）
    threshold = ATOMIC_FLUSH_THRESHOLD * num_online_cpus();
    updates = atomic_read(&cache->stats_updates);
    
    if (updates < threshold)
        return;
    
    // 直接刷新，不检查时间
    css_atomic_refresh_cache(memcg);
}
```

### 选项 B：保留 2s 限速（更保守）

```c
// 当前实现
// 优点：避免频繁刷新
// 缺点：与 rstat 有差异
```

---

## ✅ 回答你的问题

> 现在 memory.stat 和 memory.numa_stat 读取，会强制刷新数据了吧?

**不是"强制刷新"，而是"检查后决定"：**

```
memory.stat 读取：
  css_atomic_flush(memcg, false)
    ↓
  检查：updates >= threshold？
    NO  → 不刷新，返回 cache
    YES → 继续
    ↓
  检查：age >= 2s？
    NO  → 不刷新，返回 cache
    YES → 刷新！

结论：
- 如果更新次数少，不刷新
- 如果刚刷新过（< 2s），不刷新
- 只有更新多 且 时间久了，才刷新
```

**与 rstat 对比：**

rstat 的 memory.stat 读取：
```c
mem_cgroup_flush_stats(memcg)
  ↓
检查：updates >= threshold？
  NO  → 不刷新
  YES → 刷新（不检查时间）
```

rstat 不检查时间，只要超过阈值就刷新。

---

## 🔧 推荐改进

**完全模仿 rstat，移除 flush 中的 2s 检查：**

让 `css_atomic_flush(memcg, false)` 的行为完全等同于 rstat 的 `mem_cgroup_flush_stats(memcg)`。

2s 速率限制只保留在 `css_atomic_flush_ratelimited()` 中（4s 限制）。

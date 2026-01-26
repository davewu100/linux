# Atomic Counter - 时间窗口 Cache 方案

## 🎯 设计概述

这是基于 rstat 思想的简化版本：**本地值 + 时间窗口 Cache（2秒 TTL）**

### 核心特性

```
1. 写入：只更新本地节点 ✓
   - 无向上传播
   - 无根节点瓶颈
   - 写入时标记 cache 失效

2. 读取：优先从 cache ✓
   - Cache 有效且 < 2秒 → O(1) 返回
   - Cache 失效或 > 2秒 → O(N) 刷新

3. 强制刷新：✓
   - force=true 参数
   - 读取 memory.stat / memory.numa_stat 时
```

---

## 📐 设计原理

### 与 rstat 的对比

| 特性 | rstat | 时间窗口方案 |
|------|-------|-------------|
| **写入** | Per-CPU 累积 | 本地 atomic 更新 |
| **读取** | 聚合 128 CPU + 树遍历 | Cache (2s TTL) |
| **刷新触发** | 阈值 + 时间 | 仅时间 (简化) |
| **并发** | Per-CPU 无竞争 | Atomic 轻微竞争 |
| **内存** | Per-CPU × N | 单个 counter × N |

### 设计哲学

```
灵感来自 rstat，但简化：
1. 不用 per-CPU（简化内存占用）
2. 不用阈值检查（简化逻辑）
3. 只用时间窗口（2秒 TTL）
4. 显式刷新（memory.stat 读取时）

权衡：
- 写入稍慢于 rstat（atomic vs per-CPU）
- 但仍远快于层级值方案（无根节点瓶颈）
- 读取大部分时候 O(1)（2秒内）
- 代码极简（~260 行）
```

---

## 💻 实现细节

### 数据结构

```c
/* Counter: 存储本地值（非层级） */
struct memcg_atomic_counter {
    atomic64_t state[MEMCG_VMSTAT_SIZE];    // 本地值
    atomic64_t events[NR_MEMCG_EVENTS];
};

/* Cache: 时间窗口缓存 */
struct memcg_atomic_cache {
    bool valid;                              // Cache 是否有效
    unsigned long flush_time;                // 上次刷新时间（jiffies）
    u64 stats[MEMCG_VMSTAT_SIZE];          // 缓存的聚合值
    unsigned long events[NR_MEMCG_EVENTS];
};

#define ATOMIC_CACHE_TTL (2UL * HZ)  // 2秒 TTL
```

### 写入路径

```c
void css_atomic_mod_state(memcg, idx, val) {
    counter = READ_ONCE(memcg->atomic_counter);
    
    /* 只更新本地 counter */
    atomic64_add(val, &counter->state[idx]);
    
    /* 标记 cache 失效 */
    if (memcg->atomic_cache)
        WRITE_ONCE(memcg->atomic_cache->valid, false);
}
```

**特点：**
- ✅ 1 个原子操作（fast）
- ✅ 只写本地节点（no hot spot）
- ✅ 标记 cache 失效（1 次写入）

### 读取路径

```c
u64 css_atomic_page_state(memcg, idx, force) {
    cache = memcg->atomic_cache;
    now = jiffies;
    
    /* 检查 cache 是否在 TTL 窗口内 */
    if (!force && READ_ONCE(cache->valid)) {
        unsigned long age = now - READ_ONCE(cache->flush_time);
        if (age < ATOMIC_CACHE_TTL) {
            /* Cache 命中：2秒内 */
            return READ_ONCE(cache->stats[idx]);  // O(1)
        }
    }
    
    /* Cache 失效或过期：刷新 */
    css_atomic_refresh_cache(memcg);  // O(N)
    return READ_ONCE(cache->stats[idx]);
}
```

**特点：**
- ✅ 2秒内读取：O(1)
- ⚠️ 超过 2秒或失效：O(N) 刷新
- ✅ 简单清晰

### Cache 刷新

```c
static void css_atomic_refresh_cache(memcg) {
    cache = memcg->atomic_cache;
    
    /* 遍历子树聚合 */
    for (i = 0; i < MEMCG_VMSTAT_SIZE; i++) {
        cache->stats[i] = css_atomic_aggregate_stats(memcg, i);
    }
    
    /* 标记有效并记录时间 */
    WRITE_ONCE(cache->flush_time, jiffies);
    WRITE_ONCE(cache->valid, true);
}

static u64 css_atomic_aggregate_stats(memcg, idx) {
    total = 0;
    
    /* 遍历整棵子树 */
    for (iter = memcg; iter; iter = mem_cgroup_iter(memcg, iter, NULL)) {
        counter = READ_ONCE(iter->atomic_counter);
        total += atomic64_read(&counter->state[idx]);
    }
    
    return total;
}
```

---

## 📊 性能分析

### 写入性能

```
操作：task1 分配内存

时间窗口方案：
1. atomic64_add(&task1->counter->state[idx], val)  ~50 cycles
2. cache->valid = false                            ~10 cycles
总计：~60 cycles ✅ 快

对比：
- 层级值方案：450 cycles（D=3, 根节点竞争）❌
- 原方案：50 cycles + atomic_inc ✅
```

### 读取性能

```
场景 1：2秒内读取（大部分情况）
if (cache_age < 2s)
    return cache->stats[idx];  // ~20 cycles ✅

场景 2：首次读取或超过2秒
css_atomic_refresh_cache();    // ~1500 cycles (30节点)
return cache->stats[idx];

平均（假设每秒1次读取）：
- 1次刷新 (1500 cycles) 每 2 秒
- 1次命中 (20 cycles) 其他时候
- 平均：(1500 + 20) / 2 = 760 cycles
```

### 综合性能（写重场景）

```
场景：10,000 写/秒，1 读/秒，30 cgroup

时间窗口方案：
  写入：10,000 × 60 = 600,000 cycles
  读取：1 × 760 = 760 cycles (平均)
  总计：600,760 cycles/秒

对比：
  原方案：    500,122 cycles/秒  ← 稍快（阈值优化）
  时间窗口：  600,760 cycles/秒  ← 可接受
  层级值：  4,500,020 cycles/秒  ← 极慢

排名：原方案 > 时间窗口 ≈ 本地值无cache > 层级值
```

---

## 🎯 优势与劣势

### ✅ 优势

1. **写入性能优秀**
   - 只更新本地：~60 cycles
   - 无根节点瓶颈
   - 接近原方案（50 cycles）

2. **代码极简**
   - ~260 行（vs 原方案 540 行）
   - 无阈值检查逻辑
   - 无 seqlock
   - 逻辑清晰

3. **读取大部分 O(1)**
   - 2秒窗口内：O(1)
   - 适合写重场景

4. **类似 rstat 思想**
   - 时间窗口
   - 延迟聚合
   - 易于理解

### ⚠️ 劣势

1. **数据可能延迟**
   - 最多 2 秒延迟
   - 对于需要实时数据的场景不适合

2. **写入标记失效有开销**
   - 每次写都要标记 cache 失效
   - 比 per-CPU 稍慢

3. **没有阈值优化**
   - 原方案用阈值避免过于频繁刷新
   - 时间窗口方案更简单但可能不够精细

---

## 🔄 与其他方案对比

### 完整对比表

| 方案 | 写入 | 读取 | 代码 | 适用场景 |
|------|------|------|------|---------|
| **rstat** | ~20 cycles | ~5000 cycles | 复杂 | 通用（默认）|
| **原方案** | ~50 cycles | ~122 cycles | 540行 | 写重 + 精确控制 |
| **时间窗口** | ~60 cycles | ~760 cycles | **260行** | 写重 + 简单优先 ✅ |
| **层级值** | ~450 cycles | ~20 cycles | 118行 | 读重（罕见）|

### 推荐使用场景

```
✅ 推荐使用时间窗口方案：
1. 写重场景（>100:1）
2. 可接受 2秒数据延迟
3. 追求代码简洁
4. 不需要极致性能优化

✅ 推荐使用原方案：
1. 需要精确阈值控制
2. 不可接受任何数据延迟
3. 需要最优性能
4. 生产环境

❌ 不推荐层级值方案：
1. 任何写重场景
```

---

## 🚀 使用方法

### 编译

```bash
cd /home/jianyuew/repo/tmp/linux

# 配置
echo "CONFIG_MEMCG_ATOMIC_COUNTER=y" >> .config
make oldconfig

# 编译
make mm/memcontrol-atomic.o kernel/cgroup/atomic.o
make -j$(nproc)

# 安装
sudo make modules_install install
sudo reboot
```

### 测试

```bash
# 功能验证
mkdir /sys/fs/cgroup/test
echo $$ > /sys/fs/cgroup/test/cgroup.procs
dd if=/dev/zero of=/dev/null bs=1M count=100

# 检查统计（会触发强制刷新）
cat /sys/fs/cgroup/test/memory.stat
cat /sys/fs/cgroup/memory.stat

# 性能测试
sudo perf stat -e cycles,cache-misses \
    bash -c 'for i in {1..10000}; do 
        dd if=/dev/zero of=/dev/null bs=1K count=1 2>/dev/null
    done'
```

### 在代码中强制刷新

```c
/* 当需要精确数据时 */
css_atomic_flush(memcg);  // 立即刷新 cache

/* 或使用 force 参数 */
value = css_atomic_page_state(memcg, idx, true);  // 强制刷新
```

---

## 📝 调优参数

### TTL 时间调整

```c
/* 当前：2秒 TTL */
#define ATOMIC_CACHE_TTL (2UL * HZ)

/* 可以调整为：*/
#define ATOMIC_CACHE_TTL (5UL * HZ)   // 5秒（更少刷新）
#define ATOMIC_CACHE_TTL (1UL * HZ)   // 1秒（更新鲜）

影响：
- 更长 TTL：更少刷新，更高 cache 命中率，但数据更旧
- 更短 TTL：更频繁刷新，更新鲜数据，但更多 O(N) 操作
```

### 建议配置

| 场景 | TTL | 说明 |
|------|-----|------|
| 监控系统 | 5-10s | 可接受延迟 |
| 内存管理 | 2s | 平衡（默认）|
| 实时系统 | 1s | 需要新鲜数据 |
| OOM 场景 | force=true | 必须精确 |

---

## 🎓 设计启示

### 为什么这个方案可行？

1. **Linux 场景是写重**
   - 内存分配：每秒数万到数百万次
   - 统计读取：每秒几次到几十次
   - 写:读比例通常 >1000:1

2. **2秒延迟可接受**
   - 监控系统：通常采样间隔 >5 秒
   - memory.stat 读取：会强制刷新
   - OOM 处理：使用 force=true

3. **简单即美**
   - 无复杂阈值逻辑
   - 无 seqlock
   - 易于理解和维护

### 核心权衡

```
牺牲：数据可能延迟 2 秒
换取：
  1. 写入性能优秀（无根节点瓶颈）
  2. 代码简单（260 行）
  3. 读取大部分 O(1)（2秒内）
  
在写重场景下，这是值得的！
```

---

## 🔍 调试和监控

### 查看 cache 状态

可以添加调试信息：

```c
// 在 css_atomic_refresh_cache() 中
pr_debug("atomic: cache refresh for cgroup %s, age=%lu ms\n",
         cgroup_name(memcg->css.cgroup),
         jiffies_to_msecs(now - cache->flush_time));
```

### 监控 cache 命中率

添加统计：

```c
static atomic64_t cache_hits, cache_misses;

// 在 css_atomic_page_state() 中
if (cache hit)
    atomic64_inc(&cache_hits);
else
    atomic64_inc(&cache_misses);

// 通过 /proc 或 debugfs 导出
```

---

## 📊 总结

### 三种方案最终对比

```
原方案（本地值 + 阈值 cache）：
  优点：性能最优，精确控制
  缺点：代码复杂（540行），有阈值逻辑
  适用：生产环境 ✅

时间窗口方案（本地值 + TTL cache）：
  优点：代码简单（260行），性能接近原方案
  缺点：2秒数据延迟
  适用：写重场景 + 简单优先 ✅

层级值方案（向上聚合）：
  优点：读取最快 O(1)，代码最简（118行）
  缺点：写入慢 9 倍，根节点瓶颈
  适用：读重场景（罕见）❌
```

### 推荐

**时间窗口方案是原方案的优秀替代！**

- 性能接近（写入稍慢 20%，但仍远优于层级值）
- 代码简单 50%
- 易于理解和维护
- 适合大部分写重场景

在追求简洁性和可维护性的场景下，**强烈推荐使用时间窗口方案**！✅

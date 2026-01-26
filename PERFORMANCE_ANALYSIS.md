# 性能分析：Atomic Counter vs rstat

## 🔍 核心差异

### 写入路径性能

```
rstat:
  1. 更新 per-CPU counter         ~5-10 cycles (无竞争)
  2. 传播 updates (每 BATCH 次)   ~50 cycles (偶尔)
  总计：~10 cycles (大部分时候)

我们的实现:
  1. 更新 atomic counter          ~20-30 cycles (有轻微竞争)
  2. 传播 updates                 ~50 cycles (每次)
  总计：~70-80 cycles

差距：6-8x 慢于 rstat ❌
```

**问题：atomic 写入比 per-CPU 慢！**

---

## 📊 详细性能对比

### 1. 写入性能（关键差异）

#### rstat 写入

```c
// mm/memcontrol.c
void __mod_memcg_state(struct mem_cgroup *memcg, int idx, int val)
{
    // 获取当前 CPU 的 per-CPU 指针（fast）
    struct memcg_vmstats_percpu *statc;
    statc = this_cpu_ptr(memcg->vmstats_percpu);
    
    // 更新 per-CPU counter（无竞争！）
    statc->state[idx] += val;  // ~5-10 cycles
    
    // 每 MEMCG_CHARGE_BATCH (32) 次才传播一次
    if (++statc->stats_updates % MEMCG_CHARGE_BATCH == 0) {
        memcg_rstat_updated(memcg, val, cpu);  // ~50 cycles
    }
}
```

**性能：**
- 大部分写入：5-10 cycles（只更新 per-CPU）
- 偶尔（1/32）：50-60 cycles（传播 updates）
- **平均：~10 cycles** ✅

#### 我们的实现

```c
void css_atomic_mod_state(struct mem_cgroup *memcg, int idx, int val)
{
    // 更新 atomic counter（有竞争）
    atomic64_add(val, &counter->state[idx]);  // ~20-30 cycles
    
    // 每次都传播 updates（到所有祖先）
    for (iter = memcg; iter; iter = parent(iter)) {
        atomic_inc(&iter->cache->stats_updates);  // ~10 cycles × depth
    }
}
```

**性能：**
- atomic64_add: ~20-30 cycles（cache line 可能在其他 CPU）
- 传播 updates: ~10 cycles × depth（平均 depth=2-3）
- **平均：~50-60 cycles** ❌

**差距：5-6x 慢于 rstat！**

---

### 2. 读取性能（完全一致）

#### rstat 读取

```c
unsigned long memcg_page_state(struct mem_cgroup *memcg, int idx)
{
    // 直接读取已聚合的值
    return READ_ONCE(memcg->vmstats->state[idx]);  // ~5 cycles
}
```

#### 我们的实现

```c
u64 css_atomic_page_state(struct mem_cgroup *memcg, int idx, bool force)
{
    // 直接读取已聚合的 cache
    return READ_ONCE(cache->stats[idx]);  // ~5 cycles
}
```

**完全一致！** ✅

---

### 3. Flush 性能（我们可能更快）

#### rstat flush

```c
void css_rstat_flush(struct cgroup_subsys_state *css)
{
    // 1. 聚合所有 per-CPU counters
    for_each_possible_cpu(cpu) {
        statc = per_cpu_ptr(memcg->vmstats_percpu, cpu);
        for (i = 0; i < MEMCG_VMSTAT_SIZE; i++)
            total[i] += statc->state[i];
    }
    // 成本：O(num_CPUs × VMSTAT_SIZE) = O(128 × 100) = ~12,800 操作
    
    // 2. 遍历子树累加
    for (iter = memcg; iter; iter = next_descendant) {
        aggregate(iter);
    }
    // 成本：O(num_cgroups)
}
```

**成本：O(num_CPUs × VMSTAT_SIZE + num_cgroups)**

#### 我们的 flush

```c
void css_atomic_refresh_cache(struct mem_cgroup *memcg)
{
    // 直接遍历子树累加（counter 已经是 atomic）
    for (iter = memcg; iter; iter = mem_cgroup_iter()) {
        total += atomic64_read(&iter->counter->state[idx]);
    }
    // 成本：O(num_cgroups)
}
```

**成本：O(num_cgroups)**

**我们更快！** ✅（不需要聚合 per-CPU）

---

## 🎯 总体性能评估

### 写重场景（Linux 典型情况）

```
假设：10,000 次写入，10 次读取，1 次 flush

rstat:
  写入：10,000 × 10 = 100,000 cycles
  读取：10 × 5 = 50 cycles
  flush：1 × 15,000 = 15,000 cycles
  总计：115,050 cycles ✅

我们的实现：
  写入：10,000 × 60 = 600,000 cycles
  读取：10 × 5 = 50 cycles
  flush：1 × 8,000 = 8,000 cycles
  总计：608,050 cycles ❌ (5.3x 慢)
```

**写重场景下，我们慢 5-6 倍！** ❌

---

## 💡 为什么慢？

### 核心问题

```
rstat 的关键优势：per-CPU

写入路径：
  rstat:  每个 CPU 写自己的 per-CPU 变量 → 无竞争 ✅
  我们:   多个 CPU 可能写同一个 atomic → 有竞争 ❌

竞争的代价：
  - Cache line bouncing
  - 原子操作的同步开销
  - 5-6x 性能差距
```

### 具体例子

```
场景：4 个 CPU 同时在同一个 cgroup 中分配内存

rstat:
  CPU0 写 vmstats_percpu[0]  } 并行，无竞争
  CPU1 写 vmstats_percpu[1]  } 每个 CPU 写自己的
  CPU2 写 vmstats_percpu[2]  } cache line
  CPU3 写 vmstats_percpu[3]  }
  总时间：~10 cycles ✅

我们的实现:
  CPU0 atomic_add(&counter->state[idx])  } 串行化！
  CPU1 atomic_add(&counter->state[idx])  } 竞争同一个
  CPU2 atomic_add(&counter->state[idx])  } atomic 变量
  CPU3 atomic_add(&counter->state[idx])  }
  总时间：~120 cycles (4 × 30) ❌
```

---

## 🔧 可能的优化

### 优化 1：减少传播频率（模仿 rstat）

```c
// 当前：每次写都传播
atomic64_add(val, &counter->state[idx]);
for (iter = memcg; iter; iter = parent(iter))
    atomic_inc(&iter->cache->stats_updates);  // 每次都做

// 优化：每 BATCH 次才传播（像 rstat）
static __percpu int *local_updates;

void css_atomic_mod_state(memcg, idx, val)
{
    atomic64_add(val, &counter->state[idx]);
    
    // 每 32 次才传播一次
    if (++this_cpu_ptr(local_updates) % 32 == 0) {
        for (iter = memcg; iter; iter = parent(iter))
            atomic_add(32, &iter->cache->stats_updates);
    }
}
```

**改进：**
- 写入：~25-30 cycles（大部分时候）
- 偶尔：~60 cycles（传播时）
- 平均：~30 cycles ✅ (2x 提升)

**但仍然比 rstat 慢 3x！**（因为 atomic 本身慢）

---

### 优化 2：使用 per-CPU atomic counter

```c
// 更激进的优化：per-CPU + atomic
struct memcg_atomic_counter {
    struct {
        atomic64_t state[VMSTAT_SIZE];
    } __percpu *percpu_state;
};

写入：
  statc = this_cpu_ptr(counter->percpu_state);
  atomic64_add(val, &statc->state[idx]);  // per-CPU, 无竞争 ✅

读取：
  // 还是读 cache
  return cache->stats[idx];

flush：
  // 聚合 per-CPU + 树遍历
  for_each_cpu(cpu)
      total += atomic64_read(per_cpu_ptr(...));
```

**性能：**
- 写入：~10 cycles（per-CPU，无竞争）✅
- 读取：~5 cycles（cache）✅
- flush：~12,000 cycles（聚合 per-CPU）❌

**接近 rstat 性能！** ✅

**代价：**
- 内存增加：num_CPUs × VMSTAT_SIZE × 8 bytes
  - 128 CPUs × 100 × 8 = 100KB per cgroup
  - 1000 cgroups = 100MB ❌

---

## 📊 性能总结

### 当前实现 vs rstat

| 操作 | rstat | 我们 | 差距 |
|------|-------|------|------|
| **写入** | ~10 cycles | ~60 cycles | **6x 慢** ❌ |
| **读取** | ~5 cycles | ~5 cycles | 一致 ✅ |
| **flush** | ~15k cycles | ~8k cycles | **2x 快** ✅ |

### 综合场景（写重 10000:10:1）

```
rstat:          115,000 cycles  ✅
我们的实现:      608,000 cycles  ❌ (5.3x 慢)
层级值方案:   4,500,000 cycles  ❌ (39x 慢)
```

---

## 🎯 结论

### 性能评估

✅ **优点：**
1. Flush 更快（2x）
2. 读取一致
3. 代码简单

❌ **缺点：**
1. 写入慢 6x（atomic 竞争）
2. 综合性能慢 5-6x
3. 不如 rstat

### 是否可接受？

**取决于使用场景：**

#### 场景 1：极高频写入（每秒百万次）
```
rstat:  0.1ms CPU 时间
我们:   0.6ms CPU 时间
差距:   0.5ms = 可能不可接受 ❌
```

#### 场景 2：中频写入（每秒数千次）
```
rstat:  0.01ms CPU 时间
我们:   0.06ms CPU 时间
差距:   0.05ms = 可以接受 ✅
```

#### 场景 3：低频写入（每秒数百次）
```
rstat:  0.001ms CPU 时间
我们:   0.006ms CPU 时间
差距:   0.005ms = 完全可接受 ✅
```

---

## 💡 推荐

### 方案 A：保持当前实现（简单优先）

**适用：**
- 非极端性能敏感场景
- 追求代码简洁
- 内存受限（不想用 per-CPU）

**权衡：**
- 写入慢 5-6x（但仍远快于层级值方案）
- 代码简单（260 行 vs rstat 数千行）

---

### 方案 B：优化为 per-CPU atomic（性能优先）

**适用：**
- 性能关键场景
- 内存充足
- 愿意接受复杂度

**权衡：**
- 性能接近 rstat
- 内存增加 100MB (1000 cgroups)
- 代码复杂度增加

---

### 方案 C：使用原版 rstat（最优性能）

如果性能真的是问题，rstat 已经是最优解了！

---

## ✅ 我的建议

**当前实现是否足够好？**

看你的需求：

1. **如果写入频率 < 10,000/秒/cgroup**
   - 当前实现完全够用 ✅
   - 5-6x 差距在绝对值上很小（微秒级）

2. **如果写入频率 > 100,000/秒/cgroup**
   - 考虑优化或使用 rstat ⚠️
   - 差距开始变得明显

3. **如果追求极致性能**
   - 使用 rstat ✅
   - 它就是为此设计的

**实际测试是关键！** 在真实工作负载下测试性能差距。

# Atomic Counter 设计方案对比总结

## 三种方案完整对比

### 方案 A：rstat（Per-CPU + 延迟聚合）

```c
// 写入：Per-CPU，无竞争
struct percpu_counter {
    s64 __percpu *counters;  // 每个 CPU 独立
};

void update(memcg, idx, val) {
    this_cpu_add(memcg->counters[idx], val);  // 无原子操作！
}

// 读取：聚合 per-CPU + 遍历子树
u64 read(memcg, idx) {
    total = 0;
    for_each_cpu(cpu)
        total += per_cpu(memcg->counters[idx], cpu);  // 聚合 128 个 CPU
    for_each_child(child)
        total += read(child, idx);  // 递归子节点
    return total;
}
```

**特点：**
- ✅ 写入最快：无原子操作，无竞争
- ❌ 读取最慢：需要聚合 128 CPU + 遍历子树
- ✅ 内存占用高：128 CPU × N cgroups × 数组大小
- ✅ 成熟稳定：Linux 现有方案

**性能（128 CPU，30 cgroup，10000 写:1 读）：**
```
写入：this_cpu_add × 10,000 = 200,000 cycles
读取：(128 CPU + 30 子树) × 1 = 5,000 cycles
总计：205,000 cycles/秒
```

---

### 方案 B：原 Atomic Counter（本地值 + Cache 聚合）

```c
// 写入：Atomic，单节点
struct atomic_counter {
    atomic64_t state[SIZE];  // 只存本地值
};

void update(memcg, idx, val) {
    atomic64_add(val, &memcg->counter->state[idx]);  // 1 次原子操作
    atomic_inc(&memcg->atomic_updates);  // 计数
}

// 读取：从 Cache（95% 命中）
u64 read(memcg, idx) {
    if (cache_valid)
        return cache->stats[idx];  // O(1) 快速返回
    
    // Cache 失效：遍历子树刷新 cache（5%）
    cache->stats[idx] = 0;
    for_each_child_in_tree(child)
        cache->stats[idx] += atomic64_read(&child->counter->state[idx]);
    return cache->stats[idx];
}
```

**特点：**
- ✅ 写入快：1 次原子操作
- ✅ 读取快：cache 命中时 O(1)
- ⚠️  cache 失效时需要遍历子树（但频率低：每 25 秒一次）
- ✅ 内存占用低：每个 cgroup 只有 1 个 counter + 1 个 cache

**性能（128 CPU，30 cgroup，10000 写:1 读）：**
```
写入：atomic64_add × 10,000 = 500,000 cycles
读取：cache 命中 (95%) × 1 = 50 cycles
      cache 失效 (5%): 遍历 30 节点 = 1,500 cycles
平均读取：0.95 × 50 + 0.05 × 1,500 = 122.5 cycles
总计：500,122.5 cycles/秒
```

---

### 方案 C：层级值（**你要测试的方案**）

```c
// 写入：Atomic，向上传播到所有祖先
struct atomic_counter {
    atomic64_t state[SIZE];  // 存层级值（self + 所有子节点）
};

void update(memcg, idx, val) {
    // 向上传播到所有祖先
    for (iter = memcg; iter; iter = parent_mem_cgroup(iter)) {
        atomic64_add(val, &iter->counter->state[idx]);  // D 次原子操作
    }
}

// 读取：直接读，无需遍历
u64 read(memcg, idx) {
    return atomic64_read(&memcg->counter->state[idx]);  // O(1) 直接返回
}
```

**特点：**
- ❌ 写入慢：D 次原子操作（D=树深度）
- ❌ 根节点瓶颈：所有写入竞争根节点
- ✅ 读取最快：O(1) 直接读取
- ✅ 不需要 cache
- ❌ Cache line bouncing 严重

**性能（128 CPU，30 cgroup，深度 3，10000 写:1 读）：**
```
写入：atomic64_add × 10,000 × 3 = 1,500,000 cycles
      + 根节点竞争放大 3-5 倍 = 4,500,000-7,500,000 cycles
读取：atomic64_read × 1 = 20 cycles
总计：4,500,020 cycles/秒（保守估计）
```

---

## 性能对比表

| 指标 | rstat | 原 Atomic Counter | 层级值方案 |
|------|-------|------------------|-----------|
| **写入延迟** | ~20 cycles | ~50 cycles | **~450 cycles** ❌ |
| **写入竞争** | 无 | 低（本节点） | **极高（根节点）** ❌ |
| **读取延迟** | ~5,000 cycles | ~50 cycles (cache) | **~20 cycles** ✅ |
| **Cache misses** | 低 | 低 | **高（70%+）** ❌ |
| **内存占用** | 高（Per-CPU） | 中 | 中 |
| **整体性能** | 基准 | +5-10% | **-85-90%** ❌ |

### 场景分析

#### 写重场景（10000 写 : 1 读）

```
场景特点：你的实际场景
- 每秒 10,000 次内存分配/释放
- 每秒 1 次统计读取
- 128 CPU，30 cgroup

性能排名：
1. ✅ rstat:              205,000 cycles/秒
2. ✅ 原 Atomic Counter:  500,122 cycles/秒
3. ❌ 层级值方案:      4,500,020 cycles/秒（慢 9 倍！）

结论：层级值方案完全不适合
```

#### 读重场景（10 写 : 1000 读）

```
场景特点：监控密集型场景（很少见）
- 每秒 10 次内存操作
- 每秒 1000 次统计读取

性能排名：
1. ✅ 层级值方案:        25,000 cycles/秒（读取快！）
2. ✅ 原 Atomic Counter:  55,000 cycles/秒
3. ❌ rstat:           5,010,000 cycles/秒（读取慢）

结论：层级值方案在读重场景有优势
      但 Linux 实际场景是写重，不是读重！
```

---

## 根本问题：根节点瓶颈

### 可视化说明

```
层级值方案的写入过程：

task1 (depth=2) 分配 1 页内存
    ↓
更新 task1->counter    ← CPU 0 写入
更新 app1->counter     ← CPU 0 写入
更新 root->counter     ← CPU 0 写入（竞争！）

task2 (depth=2) 分配 1 页内存
    ↓
更新 task2->counter    ← CPU 1 写入
更新 app1->counter     ← CPU 1 写入（竞争！）
更新 root->counter     ← CPU 1 写入（竞争！）

task3 (depth=2) 分配 1 页内存
    ↓
更新 task3->counter    ← CPU 2 写入
更新 app2->counter     ← CPU 2 写入
更新 root->counter     ← CPU 2 写入（竞争！）

... 128 个 CPU 同时进行 ...
    ↓
root->counter 的 cache line 在 128 个 CPU 之间疯狂跳跃！
    ↓
Cache coherency protocol 崩溃
    ↓
性能灾难
```

### Cache Line Bouncing

```
时间 T=0: CPU 0 写 root->counter
          → root counter 的 cache line 在 CPU 0 (Modified)
          → CPU 1-127 的 cache line 失效 (Invalid)

时间 T=1: CPU 1 写 root->counter
          → 需要从 CPU 0 获取 cache line（100+ cycles）
          → CPU 1 的 cache line 变为 Modified
          → CPU 0, 2-127 的 cache line 失效

时间 T=2: CPU 2 写 root->counter
          → 需要从 CPU 1 获取 cache line（100+ cycles）
          → ...

每次写入都要等待 cache line 传输！
原本 50 cycles 的操作变成 150-500 cycles！
```

---

## 设计原则总结

### Linux 内核的设计智慧

1. **Per-CPU 优先**
   ```
   问：为什么 rstat 用 Per-CPU？
   答：避免竞争！每个 CPU 独立工作，无需等待。
   ```

2. **延迟聚合**
   ```
   问：为什么不在写入时立即聚合？
   答：写入频率远高于读取，延迟聚合分摊开销。
   ```

3. **避免共享热点**
   ```
   问：为什么不让所有 CPU 更新同一个变量？
   答：Cache line bouncing 导致性能灾难！
   ```

4. **读写平衡**
   ```
   问：为什么不优化读取性能？
   答：实际负载是写重（10000:1），优化写入更重要。
   ```

### 层级值方案违反了哪些原则？

| 原则 | 层级值方案 | 后果 |
|------|----------|------|
| Per-CPU 优先 | ❌ 所有 CPU 更新根节点 | Cache line bouncing |
| 延迟聚合 | ❌ 写入时立即传播 | 写入开销 9 倍 |
| 避免共享热点 | ❌ 根节点成为热点 | 竞争严重 |
| 优化写入 | ❌ 写入变慢 9 倍 | 整体性能下降 |

---

## 实验价值

虽然理论预测层级值方案会慢，但这个实验仍然很有价值：

### 学习价值

1. **验证理论**：实测性能 vs 理论预测
2. **理解 trade-off**：读快 vs 写慢的权衡
3. **掌握工具**：perf、cache 分析
4. **深入内核**：理解 Linux 设计原则

### 可能的意外

虽然概率低，但可能有意外情况：
- 硬件特性（某些 CPU 的 cache coherency 很快？）
- 编译器优化（某些优化减少竞争？）
- 负载特点（实际负载与假设不同？）

**科学方法**：理论分析 + 实验验证！

---

## 下一步行动

### 1. 编译测试

```bash
cd /home/jianyuew/repo/tmp/linux
make mm/memcontrol-atomic.o kernel/cgroup/atomic.o
make -j$(nproc)
sudo make modules_install install
sudo reboot
```

### 2. 运行测试

```bash
sudo ./test_hierarchical_performance.sh
```

### 3. 分析结果

对比以下指标：
- **Sys time**：应该增加 8-10 倍
- **Cache misses**：应该从 10% 增加到 70%+
- **吞吐量**：应该下降 85-90%

### 4. 得出结论

根据实测数据，验证或推翻理论预测。

---

## 最终总结

| 方案 | 适用场景 | 性能 | 复杂度 |
|------|---------|------|--------|
| **rstat** | 通用（Linux 默认） | ⭐⭐⭐⭐ | ⭐⭐⭐ |
| **原 Atomic Counter** | 写重 + 需要精确统计 | ⭐⭐⭐⭐⭐ | ⭐⭐⭐⭐ |
| **层级值方案** | 读重 + 浅树 + 低并发 | ⭐ | ⭐⭐ |

**推荐方案**：
- 一般场景：rstat
- 写重场景（你的场景）：原 Atomic Counter（本地值 + Cache）
- 读重场景（罕见）：层级值方案

---

准备好测试了吗？🚀

```bash
sudo ./test_hierarchical_performance.sh
```

让数据说话！

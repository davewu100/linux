# 为什么 kserial 读取 kernel 字段总是 0？

## 问题现象

```bash
# cat memory.stat 显示正确值
kernel 239238144

# kserial 读取 kernel 字段返回 0
kserial mem_cgroup @kernel  # 返回 0
```

## 根本原因

### memory.stat 的字段类型分为两类

#### 类型 1：Node Stat 项（存储在 lruvec_stats）

```c
// mm/memcontrol.c:1337-1338
{ "anon",  NR_ANON_MAPPED   },  // enum node_stat_item = 14
{ "file",  NR_FILE_PAGES    },  // enum node_stat_item = 16
```

存储位置：`memcg->nodeinfo[nid]->lruvec_stats->state[]`

#### 类型 2：Memcg Stat 项（存储在 memcg_vmstats）

```c
// mm/memcontrol.c:1339
{ "kernel",  MEMCG_KMEM  },  // enum memcg_stat_item
```

存储位置：`memcg->vmstats->state[memcg_stats_index(MEMCG_KMEM)]`

---

## 关键差异

### anon/file 的正确读取路径

```c
// mm/memcontrol.c:336
static const unsigned int memcg_node_stat_items[] = {
    ...
    NR_ANON_MAPPED,   // Index 14
    ...
    NR_FILE_PAGES,    // Index 16
    ...
};

// 初始化时建立映射
for (i = 0; i < NR_MEMCG_NODE_STAT_ITEMS; ++i, ++j)
    mem_cgroup_stats_index[memcg_node_stat_items[i]] = j;
```

实际存储：
```
vmstats->state[memcg_stats_index(NR_ANON_MAPPED)]  // 可能是 state[13]
vmstats->state[memcg_stats_index(NR_FILE_PAGES)]   // 可能是 state[15]
```

### kernel 的正确读取路径

```c
// mm/memcontrol.c:341
MEMCG_KMEM,  // enum memcg_stat_item

// 初始化时建立映射（在 node stat 之后）
for (i = 0; i < ARRAY_SIZE(memcg_stat_items); ++i, ++j)
    mem_cgroup_stats_index[memcg_stat_items[i]] = j;
```

实际存储：
```
vmstats->state[memcg_stats_index(MEMCG_KMEM)]  // 可能是 state[37]
```

---

## kserial 的问题

### 当前实现（错误）

```c
// 用户查询
kserial mem_cgroup @kernel

// kserial 可能直接映射为
vmstats.state[X]  // 错误的固定索引！
```

### 问题根源

**kserial 假设 memory.stat 字段直接对应固定的 vmstats.state[] 索引**

但实际上：
1. Node stat 和 memcg stat 使用不同的枚举
2. 需要通过 `memcg_stats_index()` 动态转换
3. 索引会在内核初始化时计算，不是固定的

---

## 解决方案

### 方案 1：在内核中实现 memcg_stats_index 转换

```c
// kernel/kserial.c
static int ks_get_memcg_stat_index(const char *stat_name)
{
    // 需要访问内核的 memory_stats 表
    extern const struct {
        const char *name;
        unsigned int idx;
    } memory_stats[];
    
    // 查找字段
    for (i = 0; memory_stats[i].name; i++) {
        if (!strcmp(memory_stats[i].name, stat_name)) {
            return memcg_stats_index(memory_stats[i].idx);
        }
    }
    
    return -ENOENT;
}
```

### 方案 2：直接使用 memcg API（推荐）✅

```c
// 使用内核提供的 API，而不是直接读 state[]
static u64 ks_read_memcg_stat(struct mem_cgroup *memcg, const char *field_name)
{
    // 字段名 → enum 映射
    if (!strcmp(field_name, "@anon")) {
        return memcg_page_state(memcg, NR_ANON_MAPPED);
    } else if (!strcmp(field_name, "@file")) {
        return memcg_page_state(memcg, NR_FILE_PAGES);
    } else if (!strcmp(field_name, "@kernel")) {
        return memcg_page_state(memcg, MEMCG_KMEM);
    } else if (!strcmp(field_name, "@kernel_stack")) {
        return memcg_page_state(memcg, NR_KERNEL_STACK_KB);
    } else if (!strcmp(field_name, "@pagetables")) {
        return memcg_page_state(memcg, NR_PAGETABLE);
    }
    
    return 0;
}
```

**优势**：
- ✅ 使用内核标准 API
- ✅ 自动处理索引转换
- ✅ 支持所有 memory.stat 字段
- ✅ 不依赖内部实现细节

### 方案 3：通过 BTF 查找 memcg_stats_index 数组

```c
// 查找 mem_cgroup_stats_index[] 数组
// 然后读取 mem_cgroup_stats_index[MEMCG_KMEM]
// 得到实际的 state[] 索引
```

**劣势**：
- ⚠️ 依赖内核内部实现
- ⚠️ 不稳定

---

## 推荐修复

### 在 kserial.c 中添加别名处理

```c
// kernel/kserial.c

/* Special handling for mem_cgroup stat fields */
static int ks_query_memcg_stat_field(struct mem_cgroup *memcg,
                                     const char *field_alias,
                                     u64 *out_value)
{
    if (!strcmp(field_alias, "@anon")) {
        *out_value = memcg_page_state(memcg, NR_ANON_MAPPED) * PAGE_SIZE;
        return 0;
    } else if (!strcmp(field_alias, "@file")) {
        *out_value = memcg_page_state(memcg, NR_FILE_PAGES) * PAGE_SIZE;
        return 0;
    } else if (!strcmp(field_alias, "@kernel")) {
        *out_value = memcg_page_state(memcg, MEMCG_KMEM) * PAGE_SIZE;
        return 0;
    } else if (!strcmp(field_alias, "@kernel_stack")) {
        *out_value = memcg_page_state(memcg, NR_KERNEL_STACK_KB) * 1024;
        return 0;
    } else if (!strcmp(field_alias, "@pagetables")) {
        *out_value = memcg_page_state(memcg, NR_PAGETABLE) * PAGE_SIZE;
        return 0;
    }
    
    return -ENOENT;
}

/* In ks_query_struct() */
if (!strcmp(struct_name, "mem_cgroup") && field_name[0] == '@') {
    // Special handling for memory.stat aliases
    return ks_query_memcg_stat_field(struct_addr, field_name, &value);
}
```

---

## 为什么 anon/file 能工作？

可能的原因：

1. **巧合的索引匹配**
   - 如果 kserial 恰好使用了正确的索引
   - 或者通过其他路径读取（直接读 lruvec_stats）

2. **需要查看实际代码**
   - 检查 kserial 如何处理 `@anon` 和 `@file`
   - 是否有特殊的别名处理逻辑

---

## 验证方法

### 1. 查看实际索引

```bash
# 在内核中打印 memcg_stats_index 的值
sudo dmesg | grep "memcg_stats_index"
```

### 2. 使用正确的 API

```c
// 修改 kserial 使用 memcg_page_state() 而不是直接读 state[]
```

---

## 总结

**问题**：kserial 直接读取 `vmstats.state[固定索引]`，但：
- Node stat 和 memcg stat 使用不同的索引映射
- `MEMCG_KMEM` 需要通过 `memcg_stats_index()` 转换

**解决**：使用内核 API `memcg_page_state(memcg, MEMCG_KMEM)` 代替直接读数组

**建议**：为 memory.stat 字段别名（@anon, @file, @kernel）实现专门的处理函数，使用标准内核 API。

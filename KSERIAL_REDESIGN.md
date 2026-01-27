# kserial 通用化设计 - 移除硬编码的 -m 参数

## 🎯 问题分析

### 当前设计的问题

```bash
# 当前: 需要 -m 参数来查询 memory.stat
sudo kserial -m anon file kernel

# 问题:
# ❌ 硬编码: 只能查 memory.stat 字段
# ❌ 不灵活: 不能查其他字段
# ❌ 维护困难: 每增加快捷方式都要改代码
# ❌ 索引问题: state[9] 等索引在不同内核版本可能不同
```

### 理想的设计

```bash
# 通用: 直接指定结构体和字段
sudo kserial mem_cgroup vmstats.state[9] vmstats.state[10]

# 查询其他结构体也一样简单
sudo kserial cgroup level nr_descendants
sudo kserial task_struct pid comm state

# 不需要任何特殊参数！
```

---

## ✅ 新设计方案

### 方案 A: 简洁语法（推荐）⭐⭐⭐⭐⭐

```bash
# 语法: kserial <struct_type> <field1> [field2] [...]

# 查询 mem_cgroup
sudo kserial mem_cgroup vmstats.state[9] vmstats.state[10]

# 查询 cgroup
sudo kserial cgroup level nr_descendants self.id

# 查询 task_struct (当前进程)
sudo kserial task_struct pid comm state

# 默认结构体 = cgroup (向后兼容)
sudo kserial level nr_descendants
# 等价于: sudo kserial cgroup level nr_descendants
```

**优点**:
- ✅ 非常简洁
- ✅ 无需记忆参数
- ✅ 自然的语法
- ✅ 向后兼容

**实现**:
```c
// main() 伪代码
if (argc < 2) usage();

// 检查第一个参数是否是已知结构体类型
if (is_valid_struct_type(argv[1])) {
    struct_type = argv[1];
    field_start = 2;
} else {
    struct_type = "cgroup";  // 默认
    field_start = 1;
}

// 其余参数都是字段名
for (i = field_start; i < argc; i++) {
    add_field(schema, argv[i]);
}
```

---

### 方案 B: 保留 -s 参数（兼容性更好）⭐⭐⭐⭐

```bash
# 显式指定结构体
sudo kserial -s mem_cgroup vmstats.state[9] vmstats.state[10]

# 默认 cgroup (不需要 -s)
sudo kserial level nr_descendants

# 其他结构体
sudo kserial -s task_struct pid comm
```

**优点**:
- ✅ 完全向后兼容
- ✅ 更明确（显式 -s 参数）
- ✅ 支持其他选项（-j, -r, -v）

---

### 方案 C: 别名机制（可选）⭐⭐⭐

保留简化的别名，但不硬编码在工具中：

```bash
# ~/.kserial_aliases
anon=vmstats.state[9]
file=vmstats.state[10]
kernel=vmstats.state[40]
kernel_stack=vmstats.state[20]
pagetables=vmstats.state[25]

# 使用别名
sudo kserial mem_cgroup @anon @file @kernel

# 或直接用字段名
sudo kserial mem_cgroup vmstats.state[9] vmstats.state[10]
```

**优点**:
- ✅ 灵活：用户可自定义别名
- ✅ 可维护：不改代码就能调整
- ✅ 适应性：不同内核版本用不同配置

---

## 🔧 实现细节

### 移除硬编码的 memcg_stat_fields

**删除**:
```c
// 删除这些硬编码
static const struct memcg_stat_field memcg_stat_fields[] = {
	{"anon",         "vmstats.state[9]"},
	{"file",         "vmstats.state[10]"},
	{"kernel",       "vmstats.state[40]"},
	// ...
};
static bool memcg_stat_mode = false;
```

**简化**:
```c
// 只需要一个简单的参数解析
int main(int argc, char *argv[]) {
    struct ks_schema schema = {0};
    const char *struct_type = "cgroup";  // 默认
    int field_idx = 0;
    
    // 解析选项
    int opt;
    while ((opt = getopt(argc, argv, "s:jrvh")) != -1) {
        switch (opt) {
        case 's':
            struct_type = optarg;
            break;
        case 'j':
            format = FORMAT_JSON;
            break;
        // ... 其他选项
        }
    }
    
    // 剩余参数都是字段名
    for (int i = optind; i < argc; i++) {
        if (field_idx >= KS_MAX_FIELDS) {
            fprintf(stderr, "Too many fields (max %d)\n", KS_MAX_FIELDS);
            return 1;
        }
        strncpy(schema.field_names[field_idx++], argv[i], 
                KS_FIELD_NAME_LEN - 1);
    }
    
    schema.nr_fields = field_idx;
    strncpy(schema.struct_name, struct_type, KS_FIELD_NAME_LEN - 1);
    
    // 查询
    struct ks_result result = {0};
    if (query_cgroup(&schema, &result) < 0)
        return 1;
    
    // 打印结果
    print_result(&result, &schema);
    return 0;
}
```

---

## 📊 使用对比

### 旧设计 (有 -m)

```bash
# 只能查 memory.stat
sudo kserial -m anon file kernel

# 查其他字段？不行！
sudo kserial -m level  # 错误：level 不是 memory.stat 字段

# 需要切换模式
sudo kserial -s cgroup level  # OK
sudo kserial -s mem_cgroup -m anon  # 混乱！
```

### 新设计 (无 -m)

```bash
# 查 memory.stat 字段 - 直接指定
sudo kserial mem_cgroup vmstats.state[9] vmstats.state[10]

# 查 cgroup 字段 - 同样简单
sudo kserial cgroup level nr_descendants

# 查 task_struct - 一致的语法
sudo kserial task_struct pid comm

# 默认 cgroup (向后兼容)
sudo kserial level nr_descendants
```

---

## 🎯 migration 指南

### 迁移旧命令

| 旧命令 | 新命令 | 说明 |
|--------|--------|------|
| `kserial -m anon` | `kserial mem_cgroup vmstats.state[9]` | 直接指定字段路径 |
| `kserial -m anon file` | `kserial mem_cgroup vmstats.state[9] vmstats.state[10]` | 多个字段 |
| `kserial level` | `kserial level` | 默认 cgroup，无变化 |
| `kserial -s cgroup level` | `kserial cgroup level` | 不再需要 -s |

### 为用户提供帮助

创建辅助脚本 `kserial-memstat`:
```bash
#!/bin/bash
# kserial-memstat: Helper for querying memory.stat fields
# 
# This provides the old -m behavior using the new flexible syntax

# Field name to array index mapping (adjust for your kernel)
declare -A FIELDS=(
    [anon]=9
    [file]=10
    [kernel]=40
    [kernel_stack]=20
    [pagetables]=25
    [sock]=41
    [percpu]=42
    [vmalloc]=43
    [shmem]=15
)

ARGS=()
for field in "$@"; do
    if [[ -n "${FIELDS[$field]}" ]]; then
        ARGS+=("vmstats.state[${FIELDS[$field]}]")
    else
        echo "Unknown field: $field"
        exit 1
    fi
done

exec kserial mem_cgroup "${ARGS[@]}"
```

使用:
```bash
# 旧语法通过包装脚本实现
kserial-memstat anon file kernel

# 等价于
kserial mem_cgroup vmstats.state[9] vmstats.state[10] vmstats.state[40]
```

---

## 🚀 扩展性

### 支持更多结构体

```bash
# 网络相关
sudo kserial net_device mtu flags

# 文件系统相关
sudo kserial inode i_size i_ino

# 调度器相关
sudo kserial task_struct prio nice policy

# 内存管理相关
sudo kserial mm_struct total_vm locked_vm
```

### 支持表达式（未来）

```bash
# 计算字段
sudo kserial mem_cgroup "vmstats.state[9]+vmstats.state[10]"

# 条件输出
sudo kserial task_struct pid comm --filter "state==TASK_RUNNING"
```

---

## 📝 总结

### 核心改进

| 方面 | 旧设计 | 新设计 |
|------|--------|--------|
| **灵活性** | ❌ 只能查预定义字段 | ✅ 任意字段 |
| **通用性** | ❌ 硬编码 memory.stat | ✅ 任意结构体 |
| **维护性** | ❌ 每次都要改代码 | ✅ 无需改代码 |
| **语法** | ⚠️ 需要记 -m 参数 | ✅ 直观自然 |
| **扩展性** | ❌ 受限 | ✅ 无限扩展 |

### 推荐实现顺序

1. **Phase 1**: 移除 `-m` 和 `memcg_stat_fields[]` ✅
   - 简化代码
   - 统一语法

2. **Phase 2**: 实现别名机制（可选）
   - 用户配置文件
   - 运行时加载

3. **Phase 3**: 增强功能（可选）
   - 表达式计算
   - 条件过滤
   - 批量查询

### 最终建议

**立即实施**：
- ✅ 移除 `-m` 参数
- ✅ 采用 `kserial <struct> <field...>` 语法
- ✅ 提供迁移脚本 `kserial-memstat` 给习惯旧命令的用户

**结果**：
- 🎯 更简洁
- 🎯 更通用
- 🎯 更易维护
- 🎯 更强大

**核心原则**: kserial 应该是一个**通用的内核结构查询工具**，而不是针对 memory.stat 的特殊工具。

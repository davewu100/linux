# memory.stat 字段的 k-serial 查询方法

## 问题

```bash
cat /sys/fs/cgroup/memory.stat
anon 2930991104           # 如何用 k-serial 查询？
file 2526576640           # 如何用 k-serial 查询？
kernel 287387648          # 如何用 k-serial 查询？
kernel_stack 28213248     # 如何用 k-serial 查询？
```

## 数据存储位置

这些值存储在 `struct mem_cgroup` 的嵌套结构中：

```c
struct mem_cgroup {
    ...
    struct memcg_vmstats *vmstats;  // 指针！
    ...
};

struct memcg_vmstats {
    long state[MEMCG_VMSTAT_SIZE];     // 统计数据数组
    unsigned long events[NR_MEMCG_EVENTS];
    ...
};
```

**关键点**：
- `anon` = `vmstats->state[NR_ANON_MAPPED]`
- `file` = `vmstats->state[NR_FILE_PAGES]`
- `kernel` = `vmstats->state[MEMCG_KMEM]`
- `kernel_stack` = `vmstats->state[NR_KERNEL_STACK_KB]`

## 当前 k-serial 的能力

### ✅ 可以做到

```bash
# 1. 查询 mem_cgroup 的直接字段
kserial -s mem_cgroup <direct_field>

# 2. 查询嵌套字段（如果不是指针）
kserial -s mem_cgroup self.id

# 3. 解引用指针并访问字段
kserial -s mem_cgroup vmstats.state[0]  # 理论上应该可以
```

### 当前实现的问题

**k-serial 的 Phase 2 支持指针解引用，Phase 3 支持数组索引**。

所以理论上 `vmstats.state[0]` 应该可以工作：
- `vmstats` 是指针 → Phase 2 解引用
- `.state[0]` 是数组访问 → Phase 3 数组索引

## 实际测试

### 方法 1: 直接查询（推荐测试）

```bash
# 尝试查询 vmstats 指针解引用 + 数组访问
kserial -s mem_cgroup vmstats.state[0]
kserial -s mem_cgroup vmstats.state[1]
kserial -s mem_cgroup vmstats.state[2]
```

**问题**：需要知道枚举值对应的索引

### 方法 2: 查看枚举定义

```bash
# 查看 enum node_stat_item 定义
grep -n "enum node_stat_item" include/linux/mmzone.h -A 100

# 或查看 BTF
bpftool btf dump file /sys/kernel/btf/vmlinux format c | grep -A 100 "enum node_stat_item"
```

**关键枚举值**（近似）：
```c
enum node_stat_item {
    NR_INACTIVE_ANON = 0,
    NR_ACTIVE_ANON,
    NR_INACTIVE_FILE,
    NR_ACTIVE_FILE,
    NR_UNEVICTABLE,
    NR_SLAB_RECLAIMABLE_B,
    NR_SLAB_UNRECLAIMABLE_B,
    NR_ISOLATED_ANON,
    NR_ISOLATED_FILE,
    NR_ANON_MAPPED,      // ~第9个
    NR_FILE_PAGES,       // ~第10个
    ...
    NR_KERNEL_STACK_KB,  // 更后面
    ...
};
```

## 实际演示

### 测试命令

```bash
# 查询 struct mem_cgroup
kserial -s mem_cgroup vmstats

# 如果 vmstats 是指针，会返回指针值
# 输出类似: vmstats = 0xffff888012345678

# 尝试解引用指针并访问数组
kserial -s mem_cgroup vmstats.state[9]   # NR_ANON_MAPPED 大概是索引 9
kserial -s mem_cgroup vmstats.state[10]  # NR_FILE_PAGES 大概是索引 10
```

## 当前限制

### 问题 1: 需要知道枚举值

```bash
# 问题：用户需要手动查找枚举值
anon → vmstats.state[?]  # 索引是多少？
file → vmstats.state[?]  # 索引是多少？
```

**解决方案**：
1. 查看内核头文件找到枚举定义
2. 或创建辅助工具打印枚举值

### 问题 2: 嵌套指针 + 数组的复杂性

当前 k-serial 的路径解析：
- `vmstats` → 解析为指针字段 ✓
- `.state` → 需要解引用指针后访问 ✓（Phase 2）
- `[0]` → 数组索引 ✓（Phase 3）

**理论上应该支持 `vmstats.state[0]`！**

## 解决方案

### 短期（使用现有接口）

```bash
# 方法 1: 直接读 memory.stat 文件
cat /sys/fs/cgroup/memory.stat | grep anon

# 方法 2: 解析 memory.stat
awk '/^anon / {print $2}' /sys/fs/cgroup/memory.stat
```

### 中期（使用 k-serial）

```bash
# 步骤 1: 找到枚举值
grep "NR_ANON_MAPPED" include/linux/mmzone.h

# 步骤 2: 用 k-serial 查询
kserial -s mem_cgroup vmstats.state[INDEX]
```

### 长期（增强 k-serial）

**Phase 4: 添加符号枚举支持**

```bash
# 理想的接口：使用枚举名而不是数字
kserial -s mem_cgroup vmstats.state[NR_ANON_MAPPED]
kserial -s mem_cgroup vmstats.state[NR_FILE_PAGES]

# 或者提供快捷方式
kserial -s mem_cgroup --stat anon file kernel
```

## 测试当前实现

让我们测试一下当前的 k-serial 是否支持 `vmstats.state[0]`：

```bash
# 编译测试工具
cd tools/testing/selftests/cgroup
make -f Makefile.kserial test_memcg_query

# 测试查询
sudo ./test_memcg_query

# 或使用 CLI 工具
sudo kserial -s mem_cgroup vmstats.state[0]
```

## 预期结果

### 如果成功
```bash
$ sudo kserial -s mem_cgroup vmstats.state[9]
vmstats.state[9]               = 2930991104

# 对应 anon 值！
```

### 如果失败
```bash
$ sudo kserial -s mem_cgroup vmstats.state[0]
Error: field 'vmstats.state[0]' not found
# 或
Error: cannot parse nested array path
```

**如果失败**，说明当前实现需要增强，使其支持 `pointer.array[index]` 语法。

## 下一步

1. **测试**：运行上面的命令，看是否支持 `vmstats.state[0]`
2. **如果成功**：找到正确的枚举索引值
3. **如果失败**：增强 k-serial 支持 `pointer.field.array[index]` 语法

## 总结

**memory.stat 的字段可以通过 k-serial 查询，但需要**：
1. ✅ 知道字段在 `vmstats->state[]` 数组中
2. ✅ 使用 `-s mem_cgroup` 指定结构体类型
3. ⚠️ 知道具体的数组索引值（枚举常量）
4. ⚠️ k-serial 当前可能需要增强以支持 `vmstats.state[0]` 这样的嵌套路径

**最简单的验证方法**：
```bash
sudo kserial -s mem_cgroup vmstats.state[0]
```

如果这个能工作，我们就可以找到正确的索引来查询 anon, file 等值！

# kserial 目标选择功能设计

## 🎯 问题分析

### 当前限制

```c
// kernel/kserial_procfs.c
if (!strcmp(struct_name, "task_struct")) {
    target_struct = current;  // ❌ 只能查询当前进程
}

if (!strcmp(struct_name, "mem_cgroup")) {
    target_struct = mem_cgroup_from_task(current);  // ❌ 只能查询当前进程的 cgroup
}
```

**问题**：
- ❌ 无法查询其他进程的 task_struct
- ❌ 无法查询其他进程的 mem_cgroup
- ❌ 限制了 kserial 的实用性

### 用户需求

```bash
# 想要查询 PID 12345 的进程信息
kserial task_struct pid comm state --pid=12345

# 想要查询特定进程的内存统计
kserial mem_cgroup vmstats.state[*] --pid=12345

# 想要查询特定 cgroup 的信息
kserial cgroup level nr_descendants --cgroup-path=/user.slice/
```

---

## 🔧 设计方案

### 方案 A: 添加 PID 参数 ⭐⭐⭐⭐⭐ (推荐)

#### 内核接口扩展

```c
// 扩展 ks_schema
struct ks_schema {
    __u32 nr_fields;
    __u32 flags;
    char struct_name[KS_FIELD_NAME_LEN];
    
    /* Target selection (new) */
    __u32 target_pid;         // 目标进程 PID (0 = current)
    __u32 target_type;        // 目标类型
    char target_path[256];    // 目标路径（用于 cgroup 路径）
    
    /* Block read parameters */
    __u32 block_offset;
    __u32 block_size;
    __u32 array_start;
    __u32 array_count;
    
    char field_names[KS_MAX_FIELDS][KS_FIELD_NAME_LEN];
};

// 目标类型
#define KS_TARGET_CURRENT   0  // 当前进程（默认）
#define KS_TARGET_PID       1  // 指定 PID
#define KS_TARGET_PATH      2  // 指定路径（cgroup）
```

#### 内核实现

```c
// kernel/kserial_procfs.c
static ssize_t ks_proc_write(struct file *file, const char __user *buf,
                              size_t count, loff_t *ppos)
{
    struct ks_proc_data *data = file->private_data;
    struct task_struct *target_task = NULL;
    void *target_struct = NULL;
    int ret;
    
    /* Copy schema from userspace */
    if (copy_from_user(data->schema, buf, sizeof(*data->schema)))
        return -EFAULT;
    
    /* Determine target task */
    if (data->schema->target_pid == 0) {
        /* Default: current process */
        target_task = current;
    } else {
        /* Find task by PID */
        rcu_read_lock();
        target_task = find_task_by_vpid(data->schema->target_pid);
        if (!target_task) {
            rcu_read_unlock();
            pr_warn("k-serial: PID %u not found\n", 
                    data->schema->target_pid);
            return -ESRCH;
        }
        get_task_struct(target_task);  // 增加引用计数
        rcu_read_unlock();
    }
    
    /* Get target struct based on type */
    rcu_read_lock();
    
    if (!strcmp(struct_name, "task_struct")) {
        target_struct = target_task;
    } else if (!strcmp(struct_name, "mem_cgroup")) {
        target_struct = mem_cgroup_from_task(target_task);
    } else if (!strcmp(struct_name, "cgroup")) {
        target_struct = task_dfl_cgroup(target_task);
    }
    
    /* Query the struct */
    ret = ks_query_struct(target_struct, struct_name, 
                          data->schema, data->result);
    
    rcu_read_unlock();
    
    /* Release task reference if we acquired one */
    if (target_task != current)
        put_task_struct(target_task);
    
    /* ... rest of the code ... */
}
```

#### 用户工具

```bash
# 查询当前进程（默认）
kserial task_struct pid comm

# 查询指定 PID
kserial task_struct pid comm --pid=12345

# 查询指定进程的内存统计
kserial mem_cgroup vmstats.state[*] --pid=12345
```

#### Python 工具实现

```python
class KSerialSchema:
    def __init__(self):
        self.nr_fields = 0
        self.flags = 0
        self.struct_name = ""
        self.target_pid = 0        # 0 = current process
        self.target_type = 0
        self.target_path = ""
        self.block_offset = 0
        # ...

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('struct_type')
    parser.add_argument('fields', nargs='+')
    parser.add_argument('--pid', type=int, default=0, 
                        help='Target process PID (0=current)')
    parser.add_argument('--cgroup-path', 
                        help='Target cgroup path')
    
    args = parser.parse_args()
    
    schema = KSerialSchema()
    schema.struct_name = args.struct_type
    schema.target_pid = args.pid
    
    if args.cgroup_path:
        schema.target_type = KS_TARGET_PATH
        schema.target_path = args.cgroup_path
    elif args.pid > 0:
        schema.target_type = KS_TARGET_PID
    
    # ... query ...
```

---

## 💻 使用示例

### 示例 1: 查询其他进程信息

```bash
# 查看 nginx 进程（PID 1234）的信息
kserial task_struct pid comm state --pid=1234

# 输出:
# pid: 1234
# comm: nginx
# state: 0
```

### 示例 2: 查询其他进程的内存统计

```bash
# 查看 PID 5678 的内存使用
kserial mem_cgroup vmstats.state[9] vmstats.state[10] --pid=5678

# 输出:
# vmstats.state[9]: 1048576000   # anon
# vmstats.state[10]: 524288000   # file
```

### 示例 3: 批量查询多个进程

```bash
# 查询所有 nginx 进程的 PID 和 RSS
for pid in $(pgrep nginx); do
    echo "PID $pid:"
    kserial task_struct pid rss --pid=$pid
done

# 输出:
# PID 1234:
#   pid: 1234
#   rss: 102400
# PID 1235:
#   pid: 1235
#   rss: 98304
```

### 示例 4: 进程对比

```bash
# 对比两个进程的内存使用
echo "Process A (PID 100):"
kserial mem_cgroup vmstats.state[0..20] --pid=100

echo "Process B (PID 200):"
kserial mem_cgroup vmstats.state[0..20] --pid=200
```

---

## 🔒 安全性考虑

### 1. 权限检查

```c
/* 检查是否有权限访问目标进程 */
static int ks_check_access_permission(struct task_struct *target)
{
    /* 同一用户的进程可以互相查询 */
    if (uid_eq(current_uid(), task_uid(target)))
        return 0;
    
    /* root 可以查询所有进程 */
    if (capable(CAP_SYS_ADMIN))
        return 0;
    
    return -EPERM;
}
```

### 2. PID namespace 隔离

```c
/* 只能查询同一 PID namespace 中的进程 */
if (task_active_pid_ns(current) != task_active_pid_ns(target_task)) {
    pr_warn("k-serial: PID namespace mismatch\n");
    return -EINVAL;
}
```

### 3. 进程存在性检查

```c
/* 确保进程在查询期间不会消失 */
rcu_read_lock();
target_task = find_task_by_vpid(pid);
if (!target_task) {
    rcu_read_unlock();
    return -ESRCH;  // No such process
}
get_task_struct(target_task);  // 增加引用计数
rcu_read_unlock();

/* ... 查询操作 ... */

put_task_struct(target_task);  // 释放引用
```

---

## 📊 性能影响

### PID 查找开销

```c
/* find_task_by_vpid() 性能 */
Hash table lookup: O(1)
Average time: ~0.5μs

对性能影响很小：
- 当前: 2μs (直接访问 current)
- 带 PID: 2.5μs (0.5μs 查找 + 2μs 查询)
```

### 引用计数开销

```c
/* get_task_struct() / put_task_struct() */
Atomic operations: ~10ns each
Total overhead: ~20ns

可忽略不计
```

---

## 🎯 替代方案对比

### 方案 B: 使用 /proc/<pid>/kserial

**优点**:
- 符合 /proc 文件系统设计理念
- 自然的 PID 选择方式

**缺点**:
- 需要为每个进程创建文件
- 管理开销大
- 难以实现

**示例**:
```bash
# 查询 PID 1234
cat > /proc/1234/kserial <<EOF
struct: task_struct
fields: pid,comm,state
EOF
cat /proc/1234/kserial
```

### 方案 C: 传递 task_struct 地址

**优点**:
- 最直接的方式

**缺点**:
- ❌ 安全风险极大（任意内存访问）
- ❌ KASLR 使地址不稳定
- ❌ 需要 root 权限读取地址

**不推荐使用**

---

## 🚀 实现优先级

### Phase 1: PID 参数支持 ✅ (推荐立即实现)

**收益**:
- 解决核心问题
- 实现简单
- 性能影响小

**实现**:
1. 扩展 `ks_schema` 添加 `target_pid`
2. 修改 `ks_proc_write()` 支持 PID 查找
3. 添加权限检查
4. 更新用户工具

### Phase 2: Cgroup 路径支持

**收益**:
- 查询特定 cgroup
- 不依赖进程 PID

**实现**:
- 添加 `target_path` 字段
- 实现路径到 cgroup 的转换

### Phase 3: 批量查询优化

**收益**:
- 一次查询多个进程
- 减少系统调用

**实现**:
- 支持 PID 列表
- 批量返回结果

---

## 📝 实现示例代码

### 内核侧完整实现

```c
// include/linux/kserial.h
struct ks_schema {
    __u32 nr_fields;
    __u32 flags;
    char struct_name[KS_FIELD_NAME_LEN];
    __u32 target_pid;         // NEW: 0 = current process
    __u32 reserved[3];        // For future use
    __u32 block_offset;
    __u32 block_size;
    __u32 array_start;
    __u32 array_count;
    char field_names[KS_MAX_FIELDS][KS_FIELD_NAME_LEN];
};

// kernel/kserial_procfs.c
static struct task_struct *ks_get_target_task(u32 pid)
{
    struct task_struct *task;
    
    if (pid == 0)
        return current;
    
    rcu_read_lock();
    task = find_task_by_vpid(pid);
    if (!task) {
        rcu_read_unlock();
        return ERR_PTR(-ESRCH);
    }
    
    /* Check permissions */
    if (!uid_eq(current_uid(), task_uid(task)) && 
        !capable(CAP_SYS_ADMIN)) {
        rcu_read_unlock();
        return ERR_PTR(-EPERM);
    }
    
    get_task_struct(task);
    rcu_read_unlock();
    
    return task;
}
```

### 用户工具完整实现

```python
#!/usr/bin/env python3
import argparse
import struct

def query_kserial(struct_type, fields, pid=0):
    schema = KSerialSchema()
    schema.struct_name = struct_type
    schema.target_pid = pid
    schema.nr_fields = len(fields)
    
    for i, field in enumerate(fields):
        schema.field_names[i] = field
    
    with open('/proc/kserial', 'r+b') as f:
        f.write(schema.pack())
        result = f.read()
    
    return parse_result(result)

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('struct_type')
    parser.add_argument('fields', nargs='+')
    parser.add_argument('--pid', type=int, default=0)
    
    args = parser.parse_args()
    
    values = query_kserial(args.struct_type, args.fields, args.pid)
    
    for field, value in zip(args.fields, values):
        print(f"{field}: {value}")
```

---

## 🎉 总结

### 核心改进

| 功能 | 之前 | 之后 |
|------|------|------|
| **查询范围** | 仅当前进程 | 任意进程 |
| **灵活性** | 受限 | 完全灵活 |
| **实用性** | 有限 | 大幅提升 |

### 推荐方案

**立即实现 Phase 1: PID 参数支持**

```bash
# 简单实用的语法
kserial task_struct pid comm --pid=12345
kserial mem_cgroup vmstats.state[*] --pid=12345
```

**收益**:
- ✅ 解决核心问题
- ✅ 实现简单（~100行代码）
- ✅ 性能影响极小（<1μs）
- ✅ 安全性可控

这个改进会让 kserial 变得真正实用！🚀

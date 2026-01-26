# BPF 查询命令示例大全

## 📋 快速对比

| 方式 | 命令复杂度 | 安装要求 | 实时性 | 灵活性 |
|------|-----------|---------|--------|--------|
| **bpftrace 一行** | ⭐⭐⭐⭐ | bpftrace | 实时 | 中 |
| **bpftrace 脚本** | ⭐⭐⭐⭐⭐ | bpftrace | 实时 | 高 |
| **BCC/Python** | ⭐⭐⭐ | python3-bcc | 实时 | 非常高 |
| **bpftool** | ⭐⭐⭐ | 内核自带 | 实时 | 中 |
| **k-serial** | ⭐⭐⭐⭐⭐ | 无需安装 | 快照 | 高 |

---

## 🔥 方式 1：bpftrace 一行命令（最快）

### 基础查询
```bash
# 查询 anon 和 file
sudo bpftrace -e 'BEGIN { 
    $m = ((struct task_struct *)curtask)->memcg->vmstats; 
    printf("anon=%ld file=%ld\n", $m->state[9], $m->state[10]); 
    exit(); 
}'
```

### 完整查询（类似 memory.stat）
```bash
sudo bpftrace -e 'BEGIN { 
    $m = ((struct task_struct *)curtask)->memcg->vmstats; 
    printf("anon           %15ld\n", $m->state[9]); 
    printf("file           %15ld\n", $m->state[10]); 
    printf("kernel         %15ld\n", $m->state[40]); 
    printf("kernel_stack   %15ld\n", $m->state[20]); 
    exit(); 
}'
```

### 持续监控（每秒输出）
```bash
sudo bpftrace -e '
interval:s:1 {
    $m = ((struct task_struct *)curtask)->memcg->vmstats;
    printf("%s: anon=%ldMB file=%ldMB\n", 
           strftime("%H:%M:%S", nsecs),
           $m->state[9] / 1048576,
           $m->state[10] / 1048576);
}'
```

**优点**：
- ✅ 命令行直接执行，无需文件
- ✅ 实时查询，延迟极低
- ✅ 可以持续监控

**缺点**：
- ❌ 需要安装 bpftrace
- ❌ 命令行较长，不易记忆
- ❌ 需要 root 权限

---

## 📜 方式 2：bpftrace 脚本（推荐）

### 使用预定义脚本
```bash
# 运行脚本
sudo bpftrace query_memcg_stats.bt

# 输出示例：
# Memory cgroup statistics:
# ─────────────────────────
# anon           = 2930991104
# file           = 2526576640
# kernel         = 287387648
# kernel_stack   = 28213248
```

### 或者使用 Shell 包装器
```bash
sudo ./tools/testing/selftests/cgroup/query_memcg_bpf.sh

# 输出：
# anon=2930991104
# file=2526576640
# kernel=287387648
# kernel_stack=28213248
```

**优点**：
- ✅ 可读性好，易于维护
- ✅ 可以添加复杂逻辑
- ✅ 可以复用和分享

**缺点**：
- ❌ 需要创建文件
- ❌ 需要 root 权限

---

## 🐍 方式 3：BCC/Python（最强大）

### 安装 BCC
```bash
# Ubuntu/Debian
sudo apt-get install bpfcc-tools python3-bpfcc

# Fedora/RHEL
sudo dnf install bcc-tools python3-bcc

# 验证安装
python3 -c "import bcc; print('BCC version:', bcc.__version__)"
```

### 使用 Python 脚本
```bash
sudo python3 tools/testing/selftests/cgroup/query_memcg_bcc.py

# 输出：
# Memory cgroup statistics:
# ─────────────────────────
# anon           2,930,991,104 bytes
# file           2,526,576,640 bytes
# kernel           287,387,648 bytes
# kernel_stack      28,213,248 bytes
# pagetables        12,345,678 bytes
# sock               1,234,567 bytes
```

### Python 代码核心部分
```python
from bcc import BPF

bpf_text = """
int get_memcg_stats(struct pt_regs *ctx) {
    struct task_struct *task = (struct task_struct *)bpf_get_current_task();
    struct mem_cgroup *memcg = task->memcg;
    
    if (!memcg || !memcg->vmstats) return 0;
    
    // Read stats
    u64 anon, file;
    bpf_probe_read_kernel(&anon, sizeof(u64), &memcg->vmstats->state[9]);
    bpf_probe_read_kernel(&file, sizeof(u64), &memcg->vmstats->state[10]);
    
    bpf_trace_printk("anon=%ld file=%ld", anon, file);
    return 0;
}
"""

b = BPF(text=bpf_text)
b.attach_tracepoint(tp="syscalls:sys_enter_getpid", fn_name="get_memcg_stats")
```

**优点**：
- ✅ 功能最强大，可以做复杂分析
- ✅ Python 生态系统丰富
- ✅ 可以集成到监控系统
- ✅ 可以使用 CO-RE（一次编译，到处运行）

**缺点**：
- ❌ 需要安装 BCC 依赖
- ❌ 代码较复杂
- ❌ 首次加载较慢

---

## 🔧 方式 4：bpftool（内核自带）

### 探索 BTF 信息
```bash
# 查看 mem_cgroup 结构定义
sudo bpftool btf dump file /sys/kernel/btf/vmlinux format c | \
     grep -A 50 "struct mem_cgroup {"

# 查看 vmstats 字段
sudo bpftool btf dump file /sys/kernel/btf/vmlinux format c | \
     grep -A 20 "struct memcg_vmstats {"
```

### 配合 bpftrace 使用
```bash
sudo ./tools/testing/selftests/cgroup/query_memcg_bpftool.sh
```

**优点**：
- ✅ 内核自带，无需安装
- ✅ 可以探索内核数据结构
- ✅ 与其他 BPF 工具配合好

**缺点**：
- ❌ 不能直接读取运行时数据
- ❌ 主要用于调试和探索

---

## 🎯 方式 5：k-serial（最简单）

### 基础查询
```bash
# 直接查询（需要提前知道索引）
sudo kserial -s mem_cgroup vmstats.state[9] vmstats.state[10]

# 使用友好名称（推荐）
sudo kserial --memcg-stat anon file kernel kernel_stack

# 输出：
# memory.stat equivalent:
# anon            2930991104
# file            2526576640
# kernel          287387648
# kernel_stack    28213248
```

### JSON 输出
```bash
sudo kserial -mj anon file

# 输出：
# {
#   "anon": 2930991104,
#   "file": 2526576640
# }
```

### 脚本中使用
```bash
ANON=$(sudo kserial -mr anon)
echo "Anonymous memory: $ANON bytes"
```

**优点**：
- ✅ 无需额外安装（内核模块）
- ✅ 命令简单，易记
- ✅ 支持 JSON 输出
- ✅ 性能稳定

**缺点**：
- ❌ 需要内核支持（k-serial 模块）
- ❌ 是快照，不适合持续监控（每次需要 4 次系统调用）
- ❌ 索引可能因内核版本而异

**为什么不适合实时监控？**

kserial 是"拉取"模式，每次查询需要：
```
1. open(/proc/kserial)    ← 系统调用
2. write(schema)          ← 系统调用
3. read(result)           ← 系统调用
4. close()                ← 系统调用
```

如果每秒采样 10 次 = **40 次系统调用/秒**

而 bpftrace 是"推送"模式：
```
1. 加载 BPF 程序（一次性）  ← 1 次系统调用
2. 内核定时器触发采样       ← 在内核态，0 次系统调用
3. 数据推送到用户空间       ← 无阻塞
```

每秒采样 10 次 = **0 次系统调用**（采样阶段）

性能差距：**40 倍以上！**

---

## 🏆 实战场景选择

### 场景 1：快速查看当前值
**推荐**：`cat /sys/fs/cgroup/memory.stat` 或 `kserial -m`
```bash
# 最简单
cat /sys/fs/cgroup/memory.stat | head -4

# 或使用 k-serial
sudo kserial -m anon file kernel
```

### 场景 2：持续监控内存变化
**推荐**：bpftrace interval
```bash
sudo bpftrace -e '
interval:s:1 {
    $m = ((struct task_struct *)curtask)->memcg->vmstats;
    printf("%s: anon=%ldMB file=%ldMB delta=%ldMB\n", 
           strftime("%H:%M:%S", nsecs),
           $m->state[9] / 1048576,
           $m->state[10] / 1048576,
           ($m->state[9] - @prev_anon) / 1048576);
    @prev_anon = $m->state[9];
}'
```

### 场景 3：集成到监控系统
**推荐**：BCC/Python
```python
# 可以接入 Prometheus, InfluxDB 等
import bcc
import prometheus_client

# ... BPF 程序 ...
# ... 收集指标并推送 ...
```

### 场景 4：调试内核问题
**推荐**：bpftool + bpftrace
```bash
# 先探索结构
sudo bpftool btf dump file /sys/kernel/btf/vmlinux | grep mem_cgroup

# 再用 bpftrace 验证
sudo bpftrace -e 'BEGIN { ... }'
```

### 场景 5：生产环境一次性查询
**推荐**：k-serial
```bash
# 简单、可靠、开销小
sudo kserial -m anon file kernel kernel_stack pagetables
```

---

## 📊 性能对比

| 方式 | 首次执行时间 | 后续执行时间 | CPU 开销 | 内存开销 |
|------|-------------|-------------|---------|---------|
| bpftrace 一行 | ~100ms | ~50ms | 低 | 低 |
| bpftrace 脚本 | ~100ms | ~50ms | 低 | 低 |
| BCC/Python | ~500ms | ~100ms | 中 | 中 |
| k-serial | ~1ms | ~1ms | 极低 | 极低 |
| cat memory.stat | ~0.1ms | ~0.1ms | 极低 | 极低 |

---

## 🔍 如何找到正确的数组索引？

### 方法 1：查看内核头文件
```bash
# 查看 memcg stat item 枚举
grep -r "enum memcg_stat_item" include/linux/memcontrol.h -A 30

# 查看 node stat item 枚举
grep -r "enum node_stat_item" include/linux/mmzone.h -A 50
```

### 方法 2：使用 BTF
```bash
# 导出枚举定义
sudo bpftool btf dump file /sys/kernel/btf/vmlinux format c | \
     grep -A 50 "enum memcg_stat_item"
```

### 方法 3：对比 memory.stat 输出
```bash
# 运行实验：逐个尝试索引
for i in {0..50}; do
    val=$(sudo kserial -s mem_cgroup -r "vmstats.state[$i]" 2>/dev/null)
    if [ $? -eq 0 ]; then
        echo "Index $i: $val"
    fi
done

# 对比 memory.stat
cat /sys/fs/cgroup/memory.stat
```

---

## 💡 总结

**日常使用推荐排序**：

1. **cat memory.stat** - 最简单，满足 99% 需求
2. **kserial -m** - 需要特定字段或编程接口时
3. **bpftrace 一行** - 需要实时监控时
4. **bpftrace 脚本** - 需要复杂逻辑时
5. **BCC/Python** - 需要集成到系统时

**所有示例文件位置**：
- `query_memcg_stats.bt` - bpftrace 脚本
- `tools/testing/selftests/cgroup/query_memcg_bpf.sh` - Shell 包装器
- `tools/testing/selftests/cgroup/query_memcg_bcc.py` - BCC/Python 脚本
- `tools/testing/selftests/cgroup/query_memcg_bpftool.sh` - bpftool 示例
- `tools/testing/selftests/cgroup/kserial_tool` - k-serial 工具

试试这些命令，找到最适合你的！

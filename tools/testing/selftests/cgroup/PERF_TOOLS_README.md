# Performance Testing Tools (func_profiler)

## 概述

基于 func_profiler 的完整性能测试工具集，用于高精度性能对比测试。

---

## 工具列表

### 1. `perf_compare` - 通用命令对比工具

**用途**：对比任意两个命令的执行性能

**用法**：
```bash
./perf_compare <iterations> <cmd1> <cmd2>
```

**示例**：
```bash
# 对比 memory.stat 和 memory.stat.ks
./perf_compare 1000 \
    "cat /sys/fs/cgroup/memory.stat > /dev/null" \
    "cat /sys/fs/cgroup/memory.stat.ks > /dev/null"

# 对比不同的 ls 命令
./perf_compare 100 "ls -l /tmp" "ls -1 /tmp"
```

**输出**：
- 每个命令的统计信息（count, avg, min, max）
- 自动计算加速比
- 性能提升百分比
- 直观的性能评级（🚀/✨/✅/⚡/⚠️）

---

### 2. `perf_benchmark` - 函数基准测试框架

**用途**：为 C 函数提供基准测试框架

**用法**：
```bash
./perf_benchmark [options]

Options:
  -m, --memstat           测试 memory.stat vs memory.stat.ks
  -f, --files <f1> <f2>   测试多个文件读取
  -n, --iterations <n>    迭代次数（默认1000）
  -h, --help              帮助信息
```

**示例**：
```bash
# 测试 memory.stat
./perf_benchmark -m

# 测试多个文件
./perf_benchmark -f /proc/meminfo /proc/cpuinfo -n 100

# 10000 次迭代
./perf_benchmark -m -n 10000
```

---

### 3. `compare_memstat_perf` - memory.stat 专用对比

**用途**：专门用于 memory.stat 和 memory.stat.ks 的详细对比

**用法**：
```bash
./compare_memstat_perf <cgroup_path>
```

**示例**：
```bash
# 测试 root cgroup
./compare_memstat_perf /sys/fs/cgroup

# 测试特定 cgroup
./compare_memstat_perf /sys/fs/cgroup/system.slice
```

**特性**：
- 1000 次迭代测试
- 数据正确性验证
- 字段数量对比
- 示例输出展示

---

## func_profiler.h API

### 基本用法

```c
#include "func_profiler.h"

struct profile_stats stats;

// 初始化
profile_init(&stats, "my_function");

// 测量
for (int i = 0; i < 1000; i++) {
    PROFILE_START(test);
    
    // 你的代码
    my_function();
    
    PROFILE_END(stats, test);
}

// 输出结果
profile_print(&stats);
```

### 对比两个实现

```c
struct profile_stats stats_old, stats_new;

profile_init(&stats_old, "old implementation");
profile_init(&stats_new, "new implementation");

// 测量两个版本...

// 自动对比
profile_compare(&stats_old, &stats_new, "Performance Improvement");
```

### API 函数

| 函数 | 说明 |
|------|------|
| `profile_init()` | 初始化统计结构 |
| `profile_record()` | 记录一次测量 |
| `profile_print()` | 打印统计结果 |
| `profile_compare()` | 对比两个统计结果 |
| `get_timestamp_ns()` | 获取纳秒级时间戳 |

### 宏

| 宏 | 说明 |
|---|------|
| `PROFILE_START(name)` | 开始计时 |
| `PROFILE_END(stats, name)` | 结束计时并记录 |

---

## 快速开始

### 编译所有工具

```bash
make -f Makefile.perf all
```

### 运行完整测试套件

```bash
./run_perf_tests.sh
```

这会运行：
1. memory.stat vs memory.stat.ks（专用工具）
2. memory.stat vs memory.stat.ks（通用工具）
3. perf_benchmark 框架测试
4. 多文件对比测试

### 单独测试

```bash
# 使用专用工具
./compare_memstat_perf /sys/fs/cgroup

# 使用通用工具
./perf_compare 1000 "cmd1" "cmd2"

# 使用基准框架
./perf_benchmark -m
```

---

## 输出示例

### profile_print() 输出

```
memory.stat (traditional)     : count=1000, avg=2.85 μs, min=2.35 μs, max=9.20 μs, total=2.85 ms
```

### profile_compare() 输出

```
=== memory.stat vs memory.stat.ks ===
Baseline (memory.stat (traditional)):
  Average: 2.85 μs
Optimized (memory.stat.ks (kserial)):
  Average: 1.15 μs
Improvement:
  Speedup: 2.48x faster
  Time saved: 1.70 μs (59.6% reduction)
  ✅ Good! More than 2x faster!
```

### 性能评级

- 🚀 **Excellent!** - 10x+ faster
- ✨ **Great!** - 5x+ faster
- ✅ **Good!** - 2x+ faster
- ⚡ **Faster** - 1x+ faster
- ⚠️ **Slower or same**

---

## 高级用法

### 自定义基准测试

```c
#include "func_profiler.h"

void my_benchmark(void)
{
    struct profile_stats stats;
    profile_init(&stats, "My Custom Test");
    
    for (int i = 0; i < 1000; i++) {
        uint64_t start = get_timestamp_ns();
        
        // 你的代码
        do_something();
        
        uint64_t end = get_timestamp_ns();
        profile_record(&stats, start, end);
    }
    
    profile_print(&stats);
}
```

### 集成到现有代码

```c
// 只需包含头文件
#include "func_profiler.h"

// 在关键路径添加测量
void critical_function(void)
{
    static struct profile_stats stats;
    static int initialized = 0;
    
    if (!initialized) {
        profile_init(&stats, "critical_function");
        initialized = 1;
    }
    
    PROFILE_START(exec);
    
    // 实际工作
    do_work();
    
    PROFILE_END(stats, exec);
    
    // 定期打印
    if (stats.count % 1000 == 0) {
        profile_print(&stats);
    }
}
```

---

## 性能特性

### 计时精度

- **方法**：`clock_gettime(CLOCK_MONOTONIC)`
- **精度**：纳秒级（< 1 ns）
- **开销**：< 100 ns per call

### 内存占用

```c
sizeof(struct profile_stats) = 40 bytes

Per measurement:
- 2 × uint64_t timestamp = 16 bytes (临时)
- 1 × uint64_t elapsed calculation = 8 bytes (临时)
```

### 适用场景

✅ **适合**：
- 微基准测试（μs 级）
- 函数性能对比
- 回归测试
- 性能监控

❌ **不适合**：
- 纳秒级操作（开销占比过大）
- 多线程并发测试（无锁保护）
- 生产环境（额外开销）

---

## 故障排查

### 编译错误

```bash
# 缺少 -lrt
gcc perf_compare.c -o perf_compare -lrt

# 完整编译命令
gcc -O2 -Wall -o perf_compare perf_compare.c -lrt
```

### 权限问题

```bash
# 读取 cgroup 文件需要 root
sudo ./compare_memstat_perf /sys/fs/cgroup

# 或添加用户到 cgroup 组
sudo usermod -aG <cgroup_group> $USER
```

### 结果异常

```bash
# 增加迭代次数减少噪声
./perf_compare 10000 "cmd1" "cmd2"

# 检查系统负载
uptime
top
```

---

## 扩展开发

### 添加新工具

1. 创建新的 `.c` 文件
2. 包含 `func_profiler.h`
3. 使用 API 进行测量
4. 更新 `Makefile.perf`

示例：
```c
// my_tool.c
#include "func_profiler.h"

int main() {
    struct profile_stats stats;
    profile_init(&stats, "my_test");
    
    // 测试代码...
    
    profile_print(&stats);
    return 0;
}
```

```makefile
# Makefile.perf
TARGETS += my_tool

my_tool: my_tool.c func_profiler.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
```

---

## 最佳实践

### 1. 预热（Warmup）

```c
// 预热 10 次
for (int i = 0; i < 10; i++) {
    function_to_test();
}

// 然后开始测量
for (int i = 0; i < 1000; i++) {
    PROFILE_START(test);
    function_to_test();
    PROFILE_END(stats, test);
}
```

### 2. 足够的迭代次数

```c
// ❌ 太少
for (int i = 0; i < 10; i++) { ... }

// ✅ 合适
for (int i = 0; i < 1000; i++) { ... }

// ✅ 更稳定
for (int i = 0; i < 10000; i++) { ... }
```

### 3. 隔离测试环境

```bash
# 停止不必要的服务
sudo systemctl stop <service>

# 设置 CPU 频率为固定值
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# 减少系统噪声
nice -n -20 ./perf_benchmark -m
```

---

## 参考

- **源码**：`func_profiler.h`
- **示例**：`compare_memstat_perf.c`, `perf_compare.c`, `perf_benchmark.c`
- **测试**：`run_perf_tests.sh`, `test_memstat_comparison.sh`
- **来源**：Ported from atomic_counter_impl_v5

---

**完整的性能测试工具集！** 🚀

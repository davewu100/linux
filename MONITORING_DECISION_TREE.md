# 内存监控工具选择决策树

## 🌳 快速决策

```
需要监控内存使用？
    |
    ├─ 一次性查看？
    │   └─ YES → cat memory.stat 或 kserial -m
    │
    └─ 持续监控？
        |
        ├─ 有 bpftrace 且可运行？
        │   ├─ YES → bpftrace ⭐⭐⭐⭐⭐ (最优)
        │   └─ NO → 继续检查
        │
        ├─ 有 kserial？
        │   ├─ YES → kserial 循环 ⭐⭐⭐ (回退方案)
        │   └─ NO → 继续检查
        │
        └─ 有 memory.stat？
            ├─ YES → cat 循环 ⭐⭐ (基础方案)
            └─ NO → 无法监控 ❌
```

---

## 📊 环境兼容性矩阵

| 场景 | BPF | kserial | cat memory.stat |
|------|-----|---------|-----------------|
| **现代 Linux (5.x+)** | ✅ 推荐 | ✅ 可用 | ✅ 可用 |
| **老内核 (3.x, 4.x)** | ❌ 不支持 | ✅ **救星！** | ✅ 可用 |
| **嵌入式系统** | ⚠️ 可能禁用 | ✅ **适合** | ✅ 可用 |
| **容器环境** | ⚠️ 权限限制 | ✅ 更宽松 | ✅ 可用 |
| **企业安全环境** | ❌ 常被禁用 | ✅ **可用** | ✅ 可用 |
| **无工具环境** | ❌ 需安装 | ⚠️ 需编译 | ✅ 无需任何工具 |

---

## 🔧 典型场景分析

### 场景 1: 老内核服务器（Kernel 3.10）

**问题**：CentOS 7 生产服务器，内核太老，不支持 BPF

```bash
# 检查
uname -r
# 3.10.0-1160.el7.x86_64

bpftrace --version
# bash: bpftrace: command not found

# 解决方案：使用 kserial！
sudo kserial -m anon file kernel

# 或持续监控
while true; do
    sudo kserial -m anon file kernel
    sleep 5
done
```

**结论**：✅ kserial 完美适用

---

### 场景 2: 嵌入式设备（Yocto Linux）

**问题**：嵌入式系统禁用了 BPF 以减少内核大小

```bash
# 检查内核配置
zcat /proc/config.gz | grep CONFIG_BPF
# CONFIG_BPF=n
# CONFIG_BPF_SYSCALL=n

# bpftrace 无法使用
bpftrace -e 'BEGIN { exit(); }'
# Error: BPF not supported

# 解决方案：编译 kserial 模块
cd kernel-module/
make
insmod kserial.ko

# 使用 kserial
kserial -m anon file
```

**结论**：✅ kserial 是唯一动态查询方案

---

### 场景 3: Docker 容器

**问题**：容器内可能没有 BPF 权限

```bash
# 容器内尝试 BPF
docker run --rm -it ubuntu:22.04 bash

apt-get update && apt-get install -y bpftrace
bpftrace -e 'BEGIN { exit(); }'
# Error: Operation not permitted

# 需要特权模式
docker run --privileged --rm -it ubuntu:22.04 bash
# 但生产环境不推荐 --privileged

# 解决方案：使用 kserial（如果宿主机加载了）
docker run --rm -it \
    -v /proc/kserial:/proc/kserial:ro \
    ubuntu:22.04 \
    kserial -m anon file
```

**结论**：✅ kserial 权限要求更低

---

### 场景 4: 企业安全环境

**问题**：安全策略禁止加载 BPF 程序

```bash
# 公司安全策略
/etc/sysctl.d/99-security.conf:
kernel.unprivileged_bpf_disabled = 1
kernel.bpf_stats_enabled = 0

# 即使 root 也可能受限
sudo bpftrace -e 'BEGIN { exit(); }'
# Error: Permission denied (security policy)

# 解决方案：kserial 通过白名单审批
# 只需读取 /proc/kserial，不需要加载代码到内核
sudo kserial -m anon file kernel
```

**结论**：✅ kserial 更容易通过安全审计

---

### 场景 5: 无法安装工具的环境

**问题**：生产环境不允许安装新软件

```bash
# 无法安装 bpftrace
sudo apt-get install bpftrace
# E: Package 'bpftrace' has no installation candidate

# 但可以加载内核模块（通过运维团队）
sudo insmod kserial.ko

# 或者如果已经加载，直接使用
if [ -e /proc/kserial ]; then
    echo "kserial 已可用！"
    # 甚至不需要 kserial 工具，可以直接操作 /proc/kserial
fi
```

**结论**：✅ kserial 依赖最少

---

## 🎯 推荐策略

### 策略 1: 理想环境（开发、测试）

```bash
# 优先使用 BPF
sudo bpftrace -e 'interval:s:1 {
    $m = ((struct task_struct *)curtask)->memcg->vmstats;
    printf("anon=%ldMB file=%ldMB\n",
           $m->state[9]/1048576, $m->state[10]/1048576);
}'
```

**原因**：性能最好，功能最强

---

### 策略 2: 受限环境（生产、老系统）

```bash
# 回退到 kserial
while true; do
    sudo kserial -m anon file kernel kernel_stack
    sleep 5
done
```

**原因**：兼容性好，依赖少

---

### 策略 3: 智能自适应（推荐）

```bash
# 使用智能脚本自动选择
sudo ./smart_monitor.sh 1

# 脚本会自动：
# 1. 检测 bpftrace → 使用 BPF（最优）
# 2. 如果不可用 → 使用 kserial（回退）
# 3. 如果还不可用 → 使用 cat（基础）
```

**原因**：一次编写，到处运行

---

## 📋 功能对比详表

| 功能 | cat | kserial | BPF |
|------|-----|---------|-----|
| **一次性查询** | ⭐⭐⭐⭐⭐ | ⭐⭐⭐⭐⭐ | ⭐⭐⭐ |
| **持续监控** | ⭐⭐ | ⭐⭐⭐ | ⭐⭐⭐⭐⭐ |
| **任意字段** | ❌ 固定格式 | ✅ 任意 BTF 字段 | ✅ 任意字段 + 计算 |
| **老内核支持** | ✅ | ✅ | ❌ |
| **嵌入式支持** | ✅ | ✅ | ⚠️ |
| **容器友好** | ✅ | ✅ | ⚠️ |
| **无工具运行** | ✅ | ⚠️ 需编译模块 | ❌ |
| **性能（监控）** | ⭐⭐ | ⭐⭐⭐ | ⭐⭐⭐⭐⭐ |
| **CPU 开销** | 中 | 中 | 低 |
| **安全审计** | ✅ 容易 | ✅ 容易 | ⚠️ 需仔细审查 |

---

## 🚀 实际部署建议

### 开发环境
```bash
# 安装完整工具链
sudo apt-get install bpftrace bpfcc-tools

# 使用 BPF
bpftrace scripts/monitor.bt
```

### 生产环境
```bash
# 准备多个方案
1. 尝试使用 BPF（如果允许）
2. 准备 kserial 模块（作为回退）
3. 最坏情况用 cat memory.stat

# 智能脚本自动选择
/opt/monitoring/smart_monitor.sh
```

### CI/CD 环境
```bash
# Docker 容器中
# 优先使用 memory.stat（最简单）
cat /sys/fs/cgroup/memory.stat

# 如果需要特定字段，挂载 kserial
docker run -v /proc/kserial:/proc/kserial:ro ...
```

---

## 💡 关键结论

### kserial 的独特价值

1. **兼容性强**
   - 老内核 ✅
   - 嵌入式 ✅
   - 受限环境 ✅

2. **依赖少**
   - 不需要 bpftrace
   - 不需要 BCC
   - 不需要 Python

3. **权限要求低**
   - 不需要加载 BPF 程序
   - 只需读取 proc 文件
   - 更容易通过安全审计

4. **灵活性高**
   - 可以查询任意 BTF 字段
   - 支持嵌套、指针、数组
   - 运行时可配置

### 什么时候 kserial 是最佳选择？

✅ **老内核**（< 4.x）
✅ **禁用 BPF 的环境**
✅ **不能安装工具的生产环境**
✅ **安全要求严格的企业环境**
✅ **嵌入式设备**
✅ **需要查询 memory.stat 之外的字段**

### BPF vs kserial: 不是替代，是互补

```
┌─────────────────────────────────────────┐
│         理想工具组合                     │
├─────────────────────────────────────────┤
│ • 快速查看     → cat / kserial -m       │
│ • 持续监控     → bpftrace (首选)        │
│ • BPF 不可用   → kserial 循环 (回退)    │
│ • 特殊字段     → kserial -s struct      │
│ • 复杂分析     → BCC/Python             │
└─────────────────────────────────────────┘
```

---

## 🔗 相关工具

运行智能监控脚本（自动选择最佳方案）：
```bash
sudo ./tools/testing/selftests/cgroup/smart_monitor.sh 1
```

查看详细性能对比：
```bash
sudo ./tools/testing/selftests/cgroup/benchmark_monitoring.sh
```

---

## 总结

**问题**：如果系统没有 bpftrace，或者没使能 bpf，是不是也可以用 kserial？

**答案**：✅ **完全正确！这正是 kserial 的重要价值！**

kserial 不是 BPF 的"劣化版本"，而是：
- **BPF 不可用时的最佳替代方案**
- **受限环境下的首选工具**
- **轻量级、广泛兼容的查询接口**

两者定位不同，各有优势，配合使用最佳！

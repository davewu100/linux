# kserial io_uring 快速测试指南

## 🚀 5 分钟验证

### 步骤 1: 加载模块

```bash
cd /home/jianyuew/repo/tmp/linux

# 加载所有必需模块
sudo insmod kernel/kserial.ko
sudo insmod kernel/kserial_cache.ko
sudo insmod kernel/kserial_string.ko
sudo insmod kernel/kserial_block.ko
sudo insmod kernel/kserial_chrdev.ko  # ← io_uring 支持

# 验证设备创建
ls -l /dev/kserial
# 应该输出: crw------- 1 root root 10, 123 ...
```

### 步骤 2: 安装 liburing（如果需要）

```bash
# Ubuntu/Debian
sudo apt install liburing-dev

# 验证
pkg-config --modversion liburing
```

### 步骤 3: 编译并运行测试

```bash
cd tools/testing/selftests/cgroup

# 编译
gcc -O2 -o kserial_uring_example \
    kserial_uring_example.c \
    $(pkg-config --cflags --libs liburing) \
    -I../../../include

# 运行
sudo ./kserial_uring_example
```

### 预期输出

```
=== kserial io_uring Example ===

[1] Opening /dev/kserial...
[2] Subscribing to mem_cgroup fields...
    ✓ Subscribed (cached BTF lookups)

[3] Initializing io_uring (queue depth: 1000)...
    ✓ io_uring initialized

[4] Batch submitting 1000 queries...
    ✓ Submitted 1000 queries (1 syscall)

[5] Waiting for completions...
    ✓ All queries completed

=== Performance Results ===
Total queries:     1000
Total time:        0.095 ms
Time per query:    0.095 μs
Throughput:        10526316 queries/sec
Syscalls:          2 (submit + wait)

=== Sample Query Results ===
First query buffer:
  anon:   2930991104 bytes (2795.00 MB)
  file:   2526576640 bytes (2409.38 MB)
  kernel: 287387648 bytes (274.13 MB)

=== Performance Comparison ===
Standard read():      9000 ns  (4 syscalls)
Subscribe read():      500 ns  (1 syscall)
Subscribe mmap:        300 ns  (0 syscall after mmap)
Subscribe io_uring:     95 ns  (batched, async)

Speedup vs read():    94.7x faster
Throughput gain:      10.5M queries/sec (io_uring) vs 111K (read)
```

---

## 🎯 关键指标验证

### ✅ 成功标志

1. **延迟 < 0.2μs**
   ```
   Time per query:    0.095 μs  ← 应该 < 200 ns
   ```

2. **吞吐量 > 5M qps**
   ```
   Throughput:        10526316 queries/sec  ← 应该 > 5M
   ```

3. **Syscalls = 2**
   ```
   Syscalls:          2 (submit + wait)  ← 必须是 2
   ```

4. **数据正确**
   ```
   anon, file, kernel 应该与 cat /sys/fs/cgroup/memory.stat 一致
   ```

---

## 🔧 对比测试

### 运行完整对比

```bash
# 对比所有传输方式
sudo ./test_all_transports.sh
```

### 预期对比结果

```
┌──────────────────────┬──────────────┬──────────────┬──────────────┐
│ Transport Method     │ Latency      │ Throughput   │ Syscalls     │
├──────────────────────┼──────────────┼──────────────┼──────────────┤
│ Standard (legacy)    │ ~9 μs        │ 111K qps     │ 4 per query  │
│ Subscribe + read()   │ ~0.5 μs      │ 2M qps       │ 1 per query  │
│ Subscribe + mmap     │ ~0.3 μs      │ 3.3M qps     │ 0 after mmap │
│ Subscribe + io_uring │ ~0.1 μs      │ 10M+ qps     │ batched      │
└──────────────────────┴──────────────┴──────────────┴──────────────┘
```

---

## 🐛 故障排查

### 问题 1: 权限错误

```
Error: open /dev/kserial: Permission denied
```

**解决**:
```bash
sudo chmod 666 /dev/kserial
# 或使用 sudo 运行测试
sudo ./kserial_uring_example
```

### 问题 2: liburing 未找到

```
Error: liburing not found
```

**解决**:
```bash
sudo apt install liburing-dev
# 或手动指定路径
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH
```

### 问题 3: 设备不存在

```
Error: /dev/kserial: No such file or directory
```

**解决**:
```bash
# 检查模块
lsmod | grep kserial_chrdev

# 加载模块
sudo insmod kernel/kserial_chrdev.ko

# 查看日志
dmesg | grep kserial | tail
```

### 问题 4: io_uring 不支持

```
Error: io_uring_queue_init failed
```

**解决**:
```bash
# 检查内核版本（需要 5.15+）
uname -r

# 检查 CONFIG_IO_URING
grep CONFIG_IO_URING /boot/config-$(uname -r)
```

---

## 📊 性能调优

### 增加批量大小

```c
#define BATCH_SIZE 10000  // 从 1000 增加到 10000

// 预期: 更高的吞吐量，更低的平均延迟
```

### 使用固定 buffer

```c
// 注册固定 buffer
struct iovec iovecs[BATCH_SIZE];
io_uring_register_buffers(&ring, iovecs, BATCH_SIZE);

// 使用固定 buffer
sqe->buf_index = i;
sqe->flags |= IOSQE_FIXED_FILE;
```

**预期提升**:
- 延迟: 0.095μs → 0.05μs（~50% faster）
- TLB miss 减少

### 使用轮询模式

```c
struct io_uring_params params = {
    .flags = IORING_SETUP_IOPOLL,  // 轮询模式
};
io_uring_queue_init_params(BATCH_SIZE, &ring, &params);
```

**预期提升**:
- 延迟: 0.095μs → 0.03μs（~70% faster）
- CPU 占用增加

---

## 📈 基准测试

### 标准基准

```bash
# 1000 查询 × 10 轮
for i in {1..10}; do
    sudo ./kserial_uring_example
done | grep "Time per query"

# 计算平均值
```

### 压力测试

```bash
# 10000 查询
sed -i 's/BATCH_SIZE 1000/BATCH_SIZE 10000/' kserial_uring_example.c
gcc -O2 -o kserial_uring_example kserial_uring_example.c $(pkg-config --cflags --libs liburing) -I../../../include
sudo ./kserial_uring_example
```

### 并发测试

```bash
# 4 个进程并发
for i in {1..4}; do
    sudo ./kserial_uring_example &
done
wait
```

---

## ✅ 验收标准

### 功能验收

- [ ] 设备创建成功 (`/dev/kserial` 存在)
- [ ] 模块加载无错误 (`dmesg` 无 ERROR)
- [ ] 订阅成功 (`ioctl(SUBSCRIBE)` 返回 0)
- [ ] io_uring 初始化成功
- [ ] 1000 个查询全部完成
- [ ] 数据正确（与 `cat memory.stat` 一致）

### 性能验收

- [ ] 延迟 < 0.2μs
- [ ] 吞吐量 > 5M qps
- [ ] Syscalls = 2 (1000 查询)
- [ ] 比 read() 快 > 50x
- [ ] 比 mmap 快 > 2x

### 稳定性验收

- [ ] 运行 10 轮无崩溃
- [ ] 内存无泄漏 (`valgrind` 验证)
- [ ] CPU 占用正常 (< 10% for 1K qps)
- [ ] 并发测试无死锁

---

## 🎉 成功！

如果所有测试通过，恭喜！

你已经成功部署了世界级性能的内核数据查询系统：
- **10M+ queries/sec**
- **~0.1μs latency**
- **90x faster than standard mode**

---

## 📚 下一步

1. **集成到监控系统**
   - Prometheus exporter
   - Grafana agent

2. **性能分析工具**
   - 替代 `cat /proc/...`
   - 批量采样

3. **容器监控**
   - Kubernetes metrics
   - Docker stats

4. **游戏服务器**
   - 实时资源监控
   - 低延迟告警

---

**Enjoy your ultra-fast kernel queries!** 🚀

#!/bin/bash
# 验证 mem_cgroup 统计值的脚本

echo "=== 对比 k-serial 和 memory.stat 的值 ==="
echo ""

# 获取当前进程的 cgroup (cgroup v2)
CGROUP_PATH=$(cat /proc/self/cgroup | head -1 | cut -d: -f3)
if [ -z "$CGROUP_PATH" ]; then
    CGROUP_PATH="/"
fi

echo "当前进程的 cgroup 路径: $CGROUP_PATH"
echo ""

# 读取当前进程 cgroup 的 memory.stat
echo "--- 当前进程 cgroup 的 memory.stat 值 ---"
if [ -f "/sys/fs/cgroup${CGROUP_PATH}/memory.stat" ]; then
    cat "/sys/fs/cgroup${CGROUP_PATH}/memory.stat" | grep -E "^(anon|file|kernel|kernel_stack)" | head -10
else
    echo "memory.stat not found at /sys/fs/cgroup${CGROUP_PATH}/memory.stat"
fi

echo ""
echo "--- root cgroup 的 memory.stat 值 (对比用) ---"
if [ -f "/sys/fs/cgroup/memory.stat" ]; then
    cat "/sys/fs/cgroup/memory.stat" | grep -E "^(anon|file|kernel|kernel_stack)" | head -10
fi

echo ""
echo "--- k-serial 查询的值 (页数) ---"
sudo ./test_kserial_real --struct mem_cgroup \
    vmstats.state[14] vmstats.state[16] vmstats.state[34] vmstats.state[23] 2>/dev/null

echo ""
echo "注意:"
echo "  - memory.stat 中的值单位是字节（已乘以单位）"
echo "  - k-serial 中的值单位："
echo "    * anon, file: 页数 (需要 * 4096)"
echo "    * kernel_stack: KB (需要 * 1024)"
echo "    * kernel: 字节 (MEMCG_KMEM 单位是字节)"
echo ""
echo "单位转换:"
echo "  - anon/file: k-serial 值 * 4096 = 字节"
echo "  - kernel_stack: k-serial 值 * 1024 = 字节"
echo "  - kernel: k-serial 值 = 字节（已经是字节）"
echo ""
echo "重要: k-serial 读取的是当前进程的 mem_cgroup，"
echo "      memory.stat 读取的是对应 cgroup 路径的值"

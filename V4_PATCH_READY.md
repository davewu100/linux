# V4 Patch 准备就绪

## 生成的文件

```
v4-0000-cover-letter.patch  - Cover letter (回复 JP 的邮件)
v4-0001-mm-optimize-stat-output-for-11-sys-time-reduce.patch - 主 patch
```

## V4 主要改动

### 1. JP Kobryn 的优化建议 ✅
- 在 `memcg_seq_buf_print_stat()` 中嵌入分隔符和换行符
- 减少函数调用：从 5 次降到 3 次（带 prefix）或 4 次降到 2 次（无 prefix）

### 2. numa_stat 函数优化 ✅
- `memory_numa_stat_show()` (v2)
- `memcg_numa_stat_show()` (v1)
- 用 `seq_puts()` + `seq_put_decimal_ull()` 替换 `seq_printf()`
- 添加注释说明为什么不新增专门的 API

### 3. Cover Letter 说明 ✅
- 解释了为什么 numa_stat 的格式 "N0=value0 N1=value1" 太特殊
- "N" 前缀和 node ID 是分开的，不适合 name=value 模式
- 直接用 `seq_put_decimal_ull()` 更灵活清晰

## 发送步骤

### 1. 检查 patch
```bash
# 查看 cover letter
cat v4-0000-cover-letter.patch

# 查看主 patch
cat v4-0001-mm-optimize-stat-output-for-11-sys-time-reduce.patch
```

### 2. 发送邮件
```bash
# 使用 git send-email 发送
git send-email --to akpm@linux-foundation.org \
               --cc shakeel.butt@linux.dev \
               --cc hannes@cmpxchg.org \
               --cc mhocko@kernel.org \
               --cc roman.gushchin@linux.dev \
               --cc muchun.song@linux.dev \
               --cc linux-mm@kvack.org \
               --cc cgroups@vger.kernel.org \
               --cc linux-kernel@vger.kernel.org \
               --cc inwardvessel@gmail.com \
               v4-*.patch
```

## 主要技术点

### memcg_seq_buf_print_stat() 优化
```c
char num_buf[MEMCG_DEC_U64_MAX_LEN + 2];  /* +2 for separator and newline */

num_buf[0] = sep;  // 嵌入分隔符
num_len = num_to_str(num_buf + 1, sizeof(num_buf) - 2, val, 0);
num_buf[num_len + 1] = '\n';  // 嵌入换行符

// 一次性输出 " value\n"
seq_buf_putmem(s, num_buf, num_len + 2);
```

### numa_stat 为什么不用辅助函数
输出格式：`"total=123 N0=456 N1=789"`
- `" N"` 和 `nid` 之间没有分隔符
- `nid` 和 `"="` 之间也没有分隔符
- 无法用统一的 name=value 接口表达

所以直接用：
```c
seq_put_decimal_ull(m, " N", nid);    // 输出 " N0"
seq_put_decimal_ull(m, "=", value);   // 输出 "=456"
```

## 性能数据

```
测试: 1M 次读取 memory.stat + memory.numa_stat
Before: sys 0m4.823s
After:  sys 0m4.275s
提升:   ~11.4%
```

## 邮件头部关键信息

```
In-Reply-To: <87ec59f7-2d76-4c7a-a2b0-57bc4e801d1d@gmail.com>
References: <20260122114242.72139-1-wujianyue000@gmail.com>
```

这样邮件会正确地回复到 JP 的评论线程中。

## 下一步

1. ✅ 仔细检查 patch 内容
2. ✅ 确认邮件列表和收件人
3. ✅ 发送 v4 patch
4. ⏳ 等待 reviewer 反馈
5. ⏳ 如果被接受，等待合并到 mm-unstable

祝好运！🎯

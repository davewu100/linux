# memory.stat.ks BTF Implementation Plan

## Current Status

已开始修改 mm/memcontrol.c，添加了：
- ✅ BTF 相关头文件和结构定义
- ✅ ks_memcg_context 上下文结构
- ✅ 更新了函数注释说明双模式

## Remaining Work

### 1. 修改 memory_stat_ks_show() (核心)

```c
static int memory_stat_ks_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_seq(m);
	struct ks_memcg_context *ctx = m->private;
	u64 start_ns, end_ns;
	
	start_ns = ktime_get_ns();
	
	/* Mode 1: Default (no field list) - show all fields */
	if (!ctx || !ctx->use_btf || !ctx->field_list) {
		return memory_stat_ks_show_legacy(m, memcg);
	}
	
	/* Mode 2: BTF query for selective fields */
	return memory_stat_ks_show_btf(m, memcg, ctx);
}
```

### 2. 保留原实现为 legacy 模式

```c
static int memory_stat_ks_show_legacy(struct seq_file *m, 
                                       struct mem_cgroup *memcg)
{
	/* 当前的硬编码实现 */
	/* ... 现有代码 ... */
}
```

### 3. 添加 BTF 查询模式

```c
static int memory_stat_ks_show_btf(struct seq_file *m,
                                    struct mem_cgroup *memcg,
                                    struct ks_memcg_context *ctx)
{
	struct ks_schema schema;
	struct ks_result result;
	u8 result_buf[KS_MAX_RESULT_SIZE];
	int ret, i;
	
	/* Setup query */
	schema.field_names = (const char **)ctx->field_array;
	schema.nr_fields = ctx->nr_fields;
	schema.flags = 0;
	
	result.data = result_buf;
	result.max_len = sizeof(result_buf);
	result.total_len = 0;
	
	/* Query via BTF */
	ret = ks_query_struct(memcg, "mem_cgroup", &schema, &result);
	if (ret < 0) {
		seq_printf(m, "# Error: BTF query failed: %d\n", ret);
		return ret;
	}
	
	/* Parse and display TLV results */
	return memory_stat_ks_display_results(m, &result, ctx);
}
```

### 4. 解析 TLV 结果

```c
static int memory_stat_ks_display_results(struct seq_file *m,
                                           struct ks_result *result,
                                           struct ks_memcg_context *ctx)
{
	u8 *p = result->data;
	u8 *end = p + result->total_len;
	int field_idx = 0;
	
	while (p < end && field_idx < ctx->nr_fields) {
		u8 type = *p++;
		u16 len;
		u64 value;
		
		if (p + 2 > end)
			break;
		
		/* Parse length (2 bytes) */
		len = *(u16 *)p;
		p += 2;
		
		if (p + len > end)
			break;
		
		/* Parse value (u64 for most fields) */
		if (len == 8) {
			value = *(u64 *)p;
			seq_printf(m, "%s %llu\n", 
				   ctx->field_array[field_idx], value);
		}
		
		p += len;
		field_idx++;
	}
	
	return 0;
}
```

### 5. 添加 Write 处理

```c
static ssize_t memory_stat_ks_write(struct kernfs_open_file *of,
                                     char *buf, size_t nbytes, loff_t off)
{
	struct ks_memcg_context *ctx;
	char *field_list, *pos, *token;
	int nr_fields = 0, i;
	
	/* Get or allocate context */
	ctx = of->priv;
	if (!ctx) {
		ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
		if (!ctx)
			return -ENOMEM;
		of->priv = ctx;
	}
	
	/* Free previous field list if any */
	if (ctx->field_array) {
		for (i = 0; i < ctx->nr_fields; i++)
			kfree(ctx->field_array[i]);
		kfree(ctx->field_array);
	}
	kfree(ctx->field_list);
	
	/* Store new field list */
	ctx->field_list = kstrndup(buf, nbytes, GFP_KERNEL);
	if (!ctx->field_list)
		return -ENOMEM;
	
	/* Count fields */
	pos = ctx->field_list;
	while ((token = strsep(&pos, ",\n\t ")) != NULL) {
		if (*token)
			nr_fields++;
	}
	
	if (nr_fields == 0 || nr_fields > KS_MAX_FIELDS) {
		kfree(ctx->field_list);
		ctx->field_list = NULL;
		return -EINVAL;
	}
	
	/* Allocate field array */
	ctx->field_array = kcalloc(nr_fields, sizeof(char *), GFP_KERNEL);
	if (!ctx->field_array) {
		kfree(ctx->field_list);
		ctx->field_list = NULL;
		return -ENOMEM;
	}
	
	/* Parse fields again and store */
	pos = kstrdup(ctx->field_list, GFP_KERNEL);
	if (!pos)
		return -ENOMEM;
	
	i = 0;
	while ((token = strsep(&pos, ",\n\t ")) != NULL) {
		if (*token && i < nr_fields) {
			ctx->field_array[i] = kstrdup(token, GFP_KERNEL);
			if (!ctx->field_array[i])
				break;
			i++;
		}
	}
	kfree(pos);
	
	ctx->nr_fields = i;
	ctx->use_btf = true;
	
	return nbytes;
}
```

### 6. 添加 Open 处理

```c
static int memory_stat_ks_open(struct kernfs_open_file *of)
{
	/* Context will be allocated on first write, or NULL for read-only */
	of->priv = NULL;
	return 0;
}
```

### 7. 添加 Release 处理

```c
static void memory_stat_ks_release(struct kernfs_open_file *of)
{
	struct ks_memcg_context *ctx = of->priv;
	int i;
	
	if (!ctx)
		return;
	
	/* Free field array */
	if (ctx->field_array) {
		for (i = 0; i < ctx->nr_fields; i++)
			kfree(ctx->field_array[i]);
		kfree(ctx->field_array);
	}
	
	kfree(ctx->field_list);
	kfree(ctx);
	of->priv = NULL;
}
```

### 8. 更新文件注册

```c
{
	.name = "stat.ks",
	.seq_show = memory_stat_ks_show,
	.write = memory_stat_ks_write,
	.open = memory_stat_ks_open,
	.release = memory_stat_ks_release,
},
```

## Implementation Complexity

### 代码量估算
- 新增函数：~400 行
- 修改现有函数：~100 行
- 总计：~500 行

### 风险
1. **中等**：TLV 解析需要仔细处理
2. **低**：内存管理（分配/释放）
3. **低**：字段解析（strsep）
4. **中等**：与 kserial BTF 接口集成

## Testing Plan

```bash
# Test 1: Backward compatibility (no write)
cat /sys/fs/cgroup/memory.stat.ks
# Expected: All fields (legacy mode)

# Test 2: Single field via BTF
echo "anon" > /sys/fs/cgroup/memory.stat.ks
cat /sys/fs/cgroup/memory.stat.ks
# Expected: Only anon field

# Test 3: Multiple fields via BTF
echo "anon,file,slab" > /sys/fs/cgroup/memory.stat.ks
cat /sys/fs/cgroup/memory.stat.ks
# Expected: 3 fields

# Test 4: Internal field (not in memory.stat)
echo "css.id" > /sys/fs/cgroup/memory.stat.ks
cat /sys/fs/cgroup/memory.stat.ks
# Expected: cgroup ID

# Test 5: Invalid field
echo "nonexistent_field" > /sys/fs/cgroup/memory.stat.ks
cat /sys/fs/cgroup/memory.stat.ks
# Expected: Error message or empty

# Test 6: Mixed valid/invalid
echo "anon,invalid,file" > /sys/fs/cgroup/memory.stat.ks
cat /sys/fs/cgroup/memory.stat.ks
# Expected: anon, error, file (or skip invalid)

# Test 7: Reset to default
echo "" > /sys/fs/cgroup/memory.stat.ks
cat /sys/fs/cgroup/memory.stat.ks
# Expected: Back to all fields
```

## Decision Point

这是一个较大的修改（~500行代码）。有几个选择：

### 选项 A：完整实现（推荐）
- ✅ 真正使用 BTF 查询
- ✅ memory.stat.ks 成为真正的 kserial 示例
- ✅ 灵活的字段选择
- ⚠️  代码量较大
- ⚠️  需要仔细测试

### 选项 B：简化实现
- 只添加字段过滤（不用 BTF）
- 在 show() 中用字符串匹配过滤
- 仍然用硬编码的 memory_stats[]
- 代码量小（~100行）
- 不是真正的 BTF 查询

### 选项 C：仅文档说明
- 不修改已提交代码
- 添加设计文档
- 提供 /dev/kserial 使用示例
- 说明 memory.stat.ks 的局限性
- 代码量：0

## Recommendation

建议**选项 A（完整实现）**，理由：

1. **真正展示 kserial 价值**
   - BTF 动态查询
   - 这才是 kserial 的核心

2. **更好的用户体验**
   - 自动绑定到 cgroup
   - 比 /dev/kserial 更方便

3. **性能更好**
   - 只查询需要的字段
   - 避免不必要的计算

4. **代码质量**
   - 虽然代码多，但逻辑清晰
   - 可复用的模式

## Next Steps

如果继续完整实现：
1. 完成上述 8 个函数的实现
2. 编译测试
3. 功能测试（上述 Test 1-7）
4. 性能测试
5. 提交新的 commit

如果选择其他方案：
- 选项 B：实现简化版本
- 选项 C：只提交设计文档

**您希望我继续哪个选项？**

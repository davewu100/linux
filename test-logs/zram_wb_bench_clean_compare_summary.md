# Clean zram_wb_bench 对比

## 之前为什么没跑起来（不是 OOM）

1. **`zram.service` 冲突**：系统自带的 zram swap 占用了 `/dev/zram0`，benchmark 无法配置 writeback
2. **`reset_zram` bug**：cleanup 后删了 backing.img，但 `reset_zram` 没有重新 `losetup`，写 `backing_dev` 失败
3. **开机自动跑太早**：systemd 在 zram 未就绪时启动失败

**修复**：`zram_wb_bench.sh` 的 `reset_zram` 加了 `losetup`；`zram-clean-bench.sh` 先 `stop zram.service`

---

## v0.3.1-on-ch 干净复测 (20260611_213741)

- pswpin 起始 = **0** ✅
- cold fault = **12288/12288 (100%)** ✅

| 指标 | zspool | ZRAM_WB |
|------|--------|---------|
| avg | 3,896 ns | 82,294 ns |
| p50 | 2,865 ns | 89,969 ns |
| p99 | 11,357 ns | 285,564 ns |
| bd_reads/sample | 0 | **0.750** |

## v0.3.5-on-ch 干净复测 (20260611_214209)

- pswpin 起始 = **0** ✅
- cold fault = **7678/12288 (62.5%)** ⚠️

| 指标 | zspool | ZRAM_WB |
|------|--------|---------|
| avg | 5,212 ns | 50,295 ns |
| p50 | 4,447 ns | 4,682 ns |
| p99 | 13,565 ns | 342,474 ns |
| bd_reads/sample | 0 | **0.375** |

## 对比 v0.3.1 参考 (2026-06-07)

| 指标 | v0.3.1 (06-07) | v0.3.1 clean | v0.3.5 clean |
|------|----------------|--------------|--------------|
| zspool avg | 1,812 ns | 3,896 ns | 5,212 ns |
| zspool fault% | 100% | 100% | **62.5%** |
| WB avg | 76,724 ns | 82,294 ns | 50,295 ns |
| WB bd_reads/sample | 0.75 | 0.75 | **0.375** |

## 结论

- **功能正常**，benchmark 已能稳定跑通
- v0.3.5 的 **swap_ops 重构** 改变了 readahead 行为：相同样本中只有 ~62.5% 被计为 cold fault，backing 读减半
- WB 的 avg/p50 看起来比 v0.3.1 快，但 **对比条件不一致**（cold fault 比例不同），不能直接说 v0.3.5 更快
- 建议后续在 v0.3.5 上调查 swap-in readahead 或 `page_in_core` 判定是否因 swap_ops 路径改变

日志：
- `zram_wb_bench_clean_*_20260611_213741.log` (v0.3.1)
- `zram_wb_bench_clean_*_20260611_214209.log` (v0.3.5)

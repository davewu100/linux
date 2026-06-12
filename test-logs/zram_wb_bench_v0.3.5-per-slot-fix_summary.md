# v0.3.5 per-slot fix 对比

## 代码改动

`zram_swap_submit_read()` 去掉 `if (zram->backing_dev) swap_bdev_submit_read()` 整批 fallback，
恢复 v0.3.1 的 per-slot `ZRAM_WB` 判断 + `read_from_zspool` 直连。

## zspool swap-in (cold, 3GB)

| 版本 | avg | p50 | p99 | fault_samples |
|------|-----|-----|-----|---------------|
| v0.3.5 **fix 前** | 5,212 ns | 4,447 ns | 13,565 ns | 7680/12288 (62.5%) |
| v0.3.5 **fix 后** | **1,721 ns** | **1,456 ns** | **6,260 ns** | 7680/12288 (62.5%) |
| v0.3.1 clean (reboot) | 3,896 ns | 2,865 ns | 11,357 ns | **12288/12288 (100%)** |
| v0.3.1 ref (06-07) | 1,812 ns | 1,504 ns | 6,198 ns | 12288/12288 (100%) |

**fix 后 zspool avg 从 5212 → 1721 ns（-67%）**，与 v0.3.1 参考值 1812 ns 同一量级。✅

## ZRAM_WB swap-in (cold)

| 版本 | avg | p50 | bd_reads/sample |
|------|-----|-----|-----------------|
| v0.3.5 fix 前 | 50,295 ns | 4,682 ns | 0.375 |
| v0.3.5 fix 后 | 51,767 ns | 5,117 ns | 0.375 |
| v0.3.1 clean | 82,294 ns | 89,969 ns | **0.750** |

WB 路径 fix 前后变化不大；与 v0.3.1 对比 bd_reads/sample 仍减半，因 **pswpin 累计值未清零**（无 reboot），readahead 状态不同。

## 结论

- **fix 有效**：zspool 性能恢复正常（5212 → 1721 ns avg）
- **fault 62.5% vs 100%**：需 reboot 后 pswpin=0 才能与 v0.3.1 clean 严格对比

日志：
- fix 后：`zram_wb_bench_clean_*_20260611_214613.log`
- 完整：`zram_wb_bench_v0.3.5-per-slot-fix_20260611_214639.log`

# zram writeback benchmark — v0.3.5-on-ch

- **Date**: 2026-06-11 21:26:59
- **Kernel**: `7.1.0-rc5-slot_free_notify_opt_v0.3.5-on-ch+`
- **Command**: `sudo ./zram_wb_bench.sh bench 3`
- **Flags**: `--cold --seed 42 --stride 64 --drop-caches` (default)
- **Duration**: ~13 s
- **Full log**: `zram_wb_bench_v0.3.5-on-ch_20260611_212659.log`
- **Kernel selftests**: `zram01` PASS, `zram02` PASS (`zram_selftests_v0.3.5-on-ch_*.log`)

## vs v0.3.1-on-ch (2026-06-07 reference)

### zspool swap-in (cold)

| Metric | v0.3.5 | v0.3.1 | Δ |
|--------|--------|--------|---|
| avg | 1,750 ns | 1,812 ns | ~same |
| p50 | 1,273 ns | 1,504 ns | **-15%** |
| p99 | 6,318 ns | 6,198 ns | ~same |
| p999 | 12,881 ns | 8,135 ns | +59% (tail worse) |
| fault_samples | 7,680 / 12,288 (**62.5%**) | 12,288 / 12,288 (**100%**) | ⚠️ 见下 |
| bd_reads_delta | 0 | 0 | OK |

### ZRAM_WB swap-in (cold)

| Metric | v0.3.5 | v0.3.1 | Δ |
|--------|--------|--------|---|
| avg | **39,080 ns** | 76,724 ns | **-49%** |
| p50 | **4,499 ns** | 89,557 ns | **-95%** |
| p99 | 116,356 ns | 210,488 ns | **-45%** |
| p999 | 137,359 ns | 449,236 ns | **-69%** |
| fault_avg | 60,621 ns | 76,730 ns | -21% |
| fault_p99 | 118,831 ns | 210,488 ns | -44% |
| bd_reads_delta | **4,606** | 9,215 | **-50%** |
| bd_reads/sample | **0.375** | 0.750 | **-50%** |

## 解读

**功能：无问题。** dmesg 无 zram 错误；`zram01`/`zram02` 自测全部 PASS。

**zspool：** 与 v0.3.1 同一量级，p50 略好，p999 略差，整体正常。

**ZRAM_WB：** 数字上 v0.3.5 全面更快，但需注意本次 **cold fault 采样率仅 62.5%**（v0.3.1 为 100%），且 **bd_reads/sample 只有一半**（0.37 vs 0.75）。说明更多 fault 被 swap readahead 预读进 RAM，测到的 p50/avg 偏低，**不等于 WB 路径本身快 10×**。

可能原因：
- kexec 启动后系统已有 swap/vmstat 残留（log 里 pswpin 起始非 0）
- v0.3.5 的 swap_ops 重构改变了 readahead/batching 行为

**建议：** 若要严格对比 v0.3.1，先 `sudo ./zram_wb_bench.sh cleanup && reboot` 再跑一轮，确保 pswpin=0、fault_samples≈100%。

## v0.3.5 分支变更（相对 v0.3.1）

```
90175d27d5a7 mm/page_io: let block drivers register custom swap I/O ops
4f3f1de64b08 mm/zram: handle swap read/write via swap_ops
648cd13057b4 mm/swap: route slot free notifications through swap_ops
```

swap read/write 和 slot_free_notify 统一走 swap_ops，属于架构重构；自测通过，zspool 性能正常，WB 数字需更干净环境复测确认。

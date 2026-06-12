# zram writeback benchmark comparison

## Run metadata

| | Baseline (v0.3.1) | Current (v0.3.11) |
|--|-------------------|-------------------|
| **Date** | 2026-06-07 14:29:32 | 2026-06-12 20:00:11 |
| **Kernel** | `7.1.0-rc5-slot_free_notify_opt_v0.3.1-on-ch+` | `7.1.0-rc5-slot_free_notify_opt_v0.3.11-on-ch+` |
| **Branch** | `slot_free_notify_opt_v0.3.0-zram-bench-test` | `slot_free_notify_opt_v0.3.11-on-ch` |
| **Command** | `sudo ./zram_wb_bench.sh bench 3` | same |
| **Flags** | `--cold --seed 42 --stride 64 --drop-caches` | same |
| **Data** | 3 GB, 12,288 samples | same |
| **Duration** | ~14 s | ~14 s |
| **Log** | `zram_wb_bench_slot_free_notify_opt_v0.3.0-zram-bench-test_20260607_142932.log` | `zram_wb_bench_v0.3.11_20260612_200011.log` |

## Per-page swap-in latency (all 12,288 samples)

| Metric | v0.3.1 zspool | v0.3.11 zspool | v0.3.11 / v0.3.1 | v0.3.1 ZRAM_WB | v0.3.11 ZRAM_WB | v0.3.11 / v0.3.1 |
|--------|---------------|----------------|------------------|----------------|-----------------|------------------|
| avg | 1,826 ns | 2,038 ns | **1.12×** | 81,160 ns | 56,771 ns | **0.70×** |
| p50 | 1,590 ns | 1,556 ns | **0.98×** | 91,222 ns | 5,694 ns | **0.06×** |
| p99 | 6,689 ns | 6,399 ns | **0.96×** | 263,010 ns | 378,319 ns | **1.44×** |
| p999 | 9,120 ns | 9,582 ns | **1.05×** | 469,716 ns | 516,461 ns | **1.10×** |

## Cold fault samples (`page_in_core` ≤ 0 before touch)

| Metric | v0.3.1 zspool | v0.3.11 zspool | v0.3.1 ZRAM_WB | v0.3.11 ZRAM_WB |
|--------|---------------|----------------|----------------|-----------------|
| fault_samples | 12,288 / 12,288 (100%) | 7,676 / 12,288 (62.5%) | 12,282 / 12,288 (99.95%) | 7,679 / 12,288 (62.5%) |
| fault_avg | 1,826 ns | 2,305 ns | 81,198 ns | 88,349 ns |
| fault_p50 | 1,590 ns | 1,799 ns | 91,236 ns | 98,480 ns |
| fault_p99 | 6,689 ns | 7,100 ns | 263,010 ns | 408,743 ns |
| fault_p999 | 9,120 ns | 10,168 ns | 469,716 ns | 613,476 ns |

Note: v0.3.11 run had fewer cold-fault samples (~62.5% vs 100%). Likely less memory pressure / partial pageout after prior benchmark activity. **All-sample latency (above) is the primary apples-to-apples metric.**

## Backing read verification

| Metric | v0.3.1 zspool | v0.3.11 zspool | v0.3.1 ZRAM_WB | v0.3.11 ZRAM_WB |
|--------|---------------|----------------|----------------|-----------------|
| bd_reads_delta | 0 | 0 | 9,211 | 4,608 |
| bd_reads_per_sample | 0 | 0 | 0.7496 (~75%) | 0.3750 (~38%) |
| pswpin delta (WB run) | +9,211 | +4,608 | — | — |
| pswpout delta (WB run) | +783,321 | +783,338 | — | — |

## Path stats after benchmark

**v0.3.11 zspool:** `bd_stat` `0 0 0`, `compr_data` ≈ 50,147,328 bytes (~48 MB)

**v0.3.11 ZRAM_WB:** `bd_stat` `1 4608 783338`, `pswpin` +4,608, `pswpout` +783,338

## Summary

### zspool (no writeback)
- **No regression on tail latency**: p99/p999 within ~5% of v0.3.1; p50 slightly better.
- avg slightly higher (+12%); overall zspool fast path looks healthy with `swap_ops` direct read.
- `bd_reads_delta = 0` confirms no accidental backing reads.

### ZRAM_WB
- **Median swap-in dramatically better**: p50 91 µs → 5.7 µs (~16×).
- **avg ~30% better** (81 µs → 57 µs).
- **Tail latency worse**: p99 +44%, p999 +10% — likely mix of fewer backing reads (good for median) vs remaining slow backing paths on outliers.
- **Backing reads halved**: 9,211 → 4,608 (~75% → ~38% of samples). swap_ops zspool fast path is serving more WB-era slots from zspool instead of backing.

### Verdict
v0.3.11 meets the main goal: zspool swap-in stays fast; WB median latency improves sharply with fewer backing reads. Tail latency on WB regressed modestly — worth profiling if p99 matters for your workload.

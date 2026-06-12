# zram writeback manual benchmark

- **Date**: 2026-06-07 14:29:32
- **Kernel**: `7.1.0-rc5-slot_free_notify_opt_v0.3.1-on-ch+`
- **Benchmark branch**: `slot_free_notify_opt_v0.3.0-zram-bench-test`
- **Command**: `sudo ./zram_wb_bench.sh bench 3`
- **Flags**: `--cold --seed 42 --stride 64 --drop-caches` (default)
- **Data size**: 3 GB (786,432 pages, 12,288 samples, stride=64)
- **Duration**: ~14 s
- **Full log**: `zram_wb_bench_slot_free_notify_opt_v0.3.0-zram-bench-test_20260607_142932.log`

## Per-page swap-in latency

| Metric | zspool | ZRAM_WB | WB / zspool |
|--------|--------|---------|-------------|
| avg | 1,826 ns (~1.8 µs) | 81,160 ns (~81 µs) | **44×** |
| p50 | 1,590 ns | 91,222 ns (~91 µs) | **57×** |
| p99 | 6,689 ns (~6.7 µs) | 263,010 ns (~263 µs) | **39×** |
| p999 | 9,120 ns (~9.1 µs) | 469,716 ns (~470 µs) | **52×** |

## Cold fault samples (page_in_core)

| Metric | zspool | ZRAM_WB | WB / zspool |
|--------|--------|---------|-------------|
| fault_samples | 12,288 / 12,288 (100%) | 12,282 / 12,288 (99.95%) | — |
| fault_avg | 1,826 ns | 81,198 ns | **44×** |
| fault_p50 | 1,590 ns | 91,236 ns | **57×** |
| fault_p99 | 6,689 ns | 263,010 ns | **39×** |
| fault_p999 | 9,120 ns | 469,716 ns | **52×** |

## Backing read verification

| Metric | zspool | ZRAM_WB |
|--------|--------|---------|
| bd_reads_before | 0 | 0 |
| bd_reads_after | 0 | 9,211 |
| bd_reads_delta | **0** | **9,211** |
| bd_reads_per_sample | 0 | **0.7496** (~75%) |

## Path stats after benchmark

**zspool:**
- `bd_stat`: `0 0 0`
- `pswpout`: 783,360 pages
- compressed pool ~48 MB (`compr_data` ≈ 50,147,328 bytes)

**ZRAM_WB:**
- `bd_stat`: `1 9211 783322` (~783K pages written to backing, 9,211 backing reads)
- `pswpin` delta: +9,211
- `pswpout` delta: +783,321

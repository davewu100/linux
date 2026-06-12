# zram / swap_ops test logs

Branch: `slot_free_notify_opt_v0.3.11-on-ch_test`

## kselftest (v0.3.11, 2026-06-12)

| Test | Result | Log |
|------|--------|-----|
| zram01 | PASS | `zram_kselftest_full_20260612_201218.log` |
| zram02 | PASS | same |
| zram03 (swap_ops integrity) | PASS | same |
| zram04 (loop swap regression) | PASS | same |

Summary: `zram_kselftest_v0.3.11_summary.md`

Run: `cd tools/testing/selftests/zram && make zram_swap_int && sudo ./zram.sh`

## writeback benchmark (3 GB, cold/seed 42/stride 64)

| Kernel / branch | Summary | Raw log |
|-----------------|---------|---------|
| v0.3.1 baseline | `zram_wb_bench_slot_free_notify_opt_v0.3.0-zram-bench-test_20260607_142932_summary.md` | `zram_wb_bench_slot_free_notify_opt_v0.3.0-zram-bench-test_20260607_142932.log` |
| v0.3.11 | `zram_wb_bench_v0.3.11_vs_v0.3.1_summary.md` | `zram_wb_bench_v0.3.11_20260612_200011.log` |

Command: `sudo ./zram_wb_bench.sh bench 3`

## Other comparisons

- `zram_wb_bench_clean_compare_summary.md` — multi-kernel clean-room runs
- `block-swap-compare/` — block swap vs zram bench matrix
- `ch-swap-compare/` — ch-swap-series variants

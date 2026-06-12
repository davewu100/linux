# Clean reboot zram_wb_bench — 7.1.0-rc5-slot_free_notify_opt_v0.3.7-on-ch+

- timestamp: 20260611_224431
- command: `./zram_wb_bench.sh bench 3` (after cleanup + reboot)
- log: `/mnt/data/kernel/clean/7.0_rc6/linux-7.0-rc6/test-logs/zram_wb_bench_clean_7.1.0-rc5-slot_free_notify_opt_v0.3.7-on-ch+_20260611_224431.log`

## Key metrics

| | zspool | ZRAM_WB |
|---|--------|---------|
| avg_ns | 4276 | 54758 |
| fault_samples | 7680 | 7678 |
| bd_reads/sample | 0 | 0.3748 |

## v0.3.1 reference (2026-06-07)

| | zspool | ZRAM_WB |
|---|--------|---------|
| avg_ns | 1812 | 76724 |
| fault_samples | 12288/12288 | 12282/12288 |
| bd_reads/sample | 0 | 0.75 |

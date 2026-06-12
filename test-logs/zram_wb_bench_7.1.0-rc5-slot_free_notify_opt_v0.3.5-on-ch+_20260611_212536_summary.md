# zram writeback manual benchmark

- **Date**: 20260611_212536
- **Kernel**: `7.1.0-rc5-slot_free_notify_opt_v0.3.5-on-ch+`
- **Branch**: slot_free_notify_opt_v0.3.5-on-ch
- **Command**: `sudo ./zram_wb_bench.sh bench 3`
- **Flags**: `--cold --seed 42 --stride 64 --drop-caches` (default)
- **Data size**: 3 GB
- **Full log**: `/mnt/data/kernel/clean/7.0_rc6/linux-7.0-rc6/test-logs/zram_wb_bench_7.1.0-rc5-slot_free_notify_opt_v0.3.5-on-ch+_20260611_212536.log`

## zspool swap-in

| avg | p99 |
|-----|-----|
| 5140 ns | 14417 ns |

## ZRAM_WB swap-in

| avg | p99 | bd_reads_delta |
|-----|-----|----------------|
| 35926 ns | 98973 ns | 4603 |

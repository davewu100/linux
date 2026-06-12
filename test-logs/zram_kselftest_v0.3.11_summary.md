# zram kselftest summary (v0.3.11)

- **Date**: 2026-06-12
- **Kernel**: `7.1.0-rc5-slot_free_notify_opt_v0.3.11-on-ch+`
- **Branch**: `slot_free_notify_opt_v0.3.11-on-ch_test`
- **Command**: `sudo ./zram.sh` (after `make zram_swap_int`)
- **Full log**: `zram_kselftest_full_20260612_201218.log`

## Results

| Test | Result | Coverage |
|------|--------|----------|
| zram01 | **PASS** | ext4 on zram, compression |
| zram02 | **PASS** | mkswap / swapon / swapoff |
| zram03 | **PASS** | swap_ops zspool integrity, slot_free_notify, writeback, swapoff cycles |
| zram04_loop_swap | **PASS** | loop block swap + stress-ng (non-zram regression) |

## zram03 subtests

| Subtest | Result | Notes |
|---------|--------|-------|
| test_swap_integrity_zspool | PASS | 64 MB, `bd_reads_delta=0`, notify_free=16384 |
| test_slot_free_notify | PASS | notify_free 0→2022, reset clears mm_stat |
| test_writeback_integrity | PASS | 128 MB WB, `bd_writes_delta=1`, data verified |
| test_swapoff_cycles | PASS | 3× swapon/swapoff/reset |

## Duration

~23 s for full `zram.sh` (zram01–04).

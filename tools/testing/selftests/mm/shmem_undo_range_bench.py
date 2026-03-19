#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0
#
# Benchmark shmem/tmpfs workloads that exercise shmem_undo_range() paths.
#
# Examples:
#   python3 tools/testing/selftests/mm/shmem_undo_range_bench.py
#   sudo python3 tools/testing/selftests/mm/shmem_undo_range_bench.py --iterations 20
#   python3 tools/testing/selftests/mm/shmem_undo_range_bench.py --targets tmpfs
#   python3 tools/testing/selftests/mm/shmem_undo_range_bench.py \
#       --rollback-request-mb 96 --rollback-safe-total-mb 1024

import argparse
import ctypes
import json
import math
import mmap
import os
import pathlib
import statistics
import sys
import tempfile
import time
from dataclasses import dataclass, asdict
from typing import Callable, Dict, List, Optional, Sequence, Tuple


PAGE_SIZE = os.sysconf("SC_PAGE_SIZE")
HUGEPAGE_SIZE = 2 * 1024 * 1024
FALLOC_FL_KEEP_SIZE = 0x01
FALLOC_FL_PUNCH_HOLE = 0x02
MADV_HUGEPAGE = 14
MADV_PAGEOUT = 21


libc = ctypes.CDLL(None, use_errno=True)
libc.fallocate.argtypes = [
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_longlong,
    ctypes.c_longlong,
]
libc.fallocate.restype = ctypes.c_int
libc.mincore.argtypes = [
    ctypes.c_void_p,
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_ubyte),
]
libc.mincore.restype = ctypes.c_int


class SkipCase(RuntimeError):
    pass


def align_down(value: int, align: int) -> int:
    return value // align * align


def align_up(value: int, align: int) -> int:
    return align_down(value + align - 1, align)


def format_ns(ns: int) -> str:
    if ns >= 1_000_000:
        return f"{ns / 1_000_000:.3f} ms"
    if ns >= 1_000:
        return f"{ns / 1_000:.3f} us"
    return f"{ns} ns"


def percentile(sorted_values: Sequence[int], q: float) -> int:
    if not sorted_values:
        return 0
    if len(sorted_values) == 1:
        return sorted_values[0]
    pos = (len(sorted_values) - 1) * q
    lower = math.floor(pos)
    upper = math.ceil(pos)
    if lower == upper:
        return sorted_values[lower]
    lower_v = sorted_values[lower]
    upper_v = sorted_values[upper]
    return int(lower_v + (upper_v - lower_v) * (pos - lower))


def read_text(path: str) -> str:
    try:
        return pathlib.Path(path).read_text(encoding="utf-8")
    except OSError:
        return ""


def swap_is_available() -> bool:
    try:
        with open("/proc/swaps", "r", encoding="utf-8") as handle:
            lines = handle.readlines()
    except OSError:
        return False
    return len(lines) > 1


def shmem_thp_mode() -> str:
    return read_text("/sys/kernel/mm/transparent_hugepage/shmem_enabled").strip()


def memfd_supported() -> bool:
    return hasattr(os, "memfd_create")


def checked_fallocate(fd: int, mode: int, offset: int, length: int) -> None:
    rc = libc.fallocate(fd, mode, offset, length)
    if rc == 0:
        return
    err = ctypes.get_errno()
    raise OSError(err, os.strerror(err))


def mincore_resident_fraction(mm: mmap.mmap, size: int) -> float:
    pages = align_up(size, PAGE_SIZE) // PAGE_SIZE
    vec = (ctypes.c_ubyte * pages)()
    addr = ctypes.addressof(ctypes.c_char.from_buffer(mm))
    rc = libc.mincore(ctypes.c_void_p(addr), ctypes.c_size_t(size), vec)
    if rc != 0:
        err = ctypes.get_errno()
        raise OSError(err, os.strerror(err))
    resident = sum(1 for idx in range(pages) if vec[idx] & 1)
    return resident / pages if pages else 0.0


def touch_mapping(mm: mmap.mmap, size: int, step: int = PAGE_SIZE) -> None:
    for offset in range(0, size, step):
        mm[offset:offset + 1] = bytes([(offset // PAGE_SIZE) & 0xFF])


def maybe_madvise(mm: mmap.mmap, advice: int) -> bool:
    if not hasattr(mm, "madvise"):
        return False
    try:
        mm.madvise(advice)
        return True
    except (AttributeError, OSError, ValueError):
        return False


def pageout_mapping(mm: mmap.mmap, size: int, timeout_s: float) -> Tuple[bool, str]:
    if not swap_is_available():
        return False, "swap is not configured on this system"
    if not maybe_madvise(mm, MADV_PAGEOUT):
        return False, "Python mmap.madvise(MADV_PAGEOUT) is unavailable"

    start = time.monotonic()
    baseline = mincore_resident_fraction(mm, size)
    target = min(0.80, baseline - 0.10)

    while time.monotonic() - start < timeout_s:
        time.sleep(0.05)
        current = mincore_resident_fraction(mm, size)
        if current <= target:
            return True, f"resident fraction dropped from {baseline:.2f} to {current:.2f}"

    current = mincore_resident_fraction(mm, size)
    return False, (
        "MADV_PAGEOUT did not reclaim enough pages "
        f"(resident fraction {baseline:.2f} -> {current:.2f})"
    )


@dataclass
class TargetHandle:
    kind: str
    fd: int
    path: Optional[str]

    def cleanup(self) -> None:
        try:
            os.close(self.fd)
        finally:
            if self.path:
                try:
                    os.unlink(self.path)
                except FileNotFoundError:
                    pass


@dataclass
class Result:
    workload: str
    target: str
    iteration: int
    status: str
    elapsed_ns: Optional[int]
    note: str


@dataclass
class BenchConfig:
    iterations: int
    size_bytes: int
    tmpfs_dir: str
    targets: List[str]
    truncate_keep_bytes: int
    swapped_timeout_s: float
    rollback_request_mb: Optional[int]
    rollback_safe_total_mb: int
    json_out: Optional[str]


def open_target(kind: str, tmpfs_dir: str) -> TargetHandle:
    if kind == "tmpfs":
        fd, path = tempfile.mkstemp(prefix="shmem-undo-range-", dir=tmpfs_dir)
        return TargetHandle(kind=kind, fd=fd, path=path)
    if kind == "memfd":
        if not memfd_supported():
            raise SkipCase("os.memfd_create() is not available")
        fd = os.memfd_create("shmem_undo_range_bench", 0)
        return TargetHandle(kind=kind, fd=fd, path=None)
    raise ValueError(f"unsupported target: {kind}")


def prepare_shared_mapping(
    handle: TargetHandle,
    size: int,
    *,
    huge_hint: bool = False,
) -> mmap.mmap:
    os.ftruncate(handle.fd, size)
    mm = mmap.mmap(
        handle.fd,
        size,
        flags=mmap.MAP_SHARED,
        prot=mmap.PROT_READ | mmap.PROT_WRITE,
    )
    if huge_hint:
        maybe_madvise(mm, MADV_HUGEPAGE)
    touch_mapping(mm, size)
    mm.flush()
    return mm


def timed_call(fn: Callable[[], None]) -> int:
    start = time.perf_counter_ns()
    fn()
    return time.perf_counter_ns() - start


def run_aligned_resident_punch(cfg: BenchConfig, target: str) -> str:
    handle = open_target(target, cfg.tmpfs_dir)
    mm = None
    try:
        mm = prepare_shared_mapping(handle, cfg.size_bytes)
        offset = align_up(cfg.size_bytes // 4, PAGE_SIZE)
        length = align_down(cfg.size_bytes // 2, PAGE_SIZE)
        return str(
            timed_call(
                lambda: checked_fallocate(
                    handle.fd,
                    FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE,
                    offset,
                    length,
                )
            )
        )
    finally:
        if mm is not None:
            mm.close()
        handle.cleanup()


def run_aligned_resident_truncate(cfg: BenchConfig, target: str) -> str:
    handle = open_target(target, cfg.tmpfs_dir)
    mm = None
    try:
        mm = prepare_shared_mapping(handle, cfg.size_bytes)
        new_size = align_down(cfg.truncate_keep_bytes, PAGE_SIZE)
        return str(timed_call(lambda: os.ftruncate(handle.fd, new_size)))
    finally:
        if mm is not None:
            mm.close()
        handle.cleanup()


def run_aligned_swapped_punch(cfg: BenchConfig, target: str) -> str:
    handle = open_target(target, cfg.tmpfs_dir)
    mm = None
    try:
        mm = prepare_shared_mapping(handle, cfg.size_bytes)
        ok, note = pageout_mapping(mm, cfg.size_bytes, cfg.swapped_timeout_s)
        if not ok:
            raise SkipCase(note)
        offset = align_up(cfg.size_bytes // 4, PAGE_SIZE)
        length = align_down(cfg.size_bytes // 2, PAGE_SIZE)
        elapsed = timed_call(
            lambda: checked_fallocate(
                handle.fd,
                FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE,
                offset,
                length,
            )
        )
        return f"{elapsed}|{note}"
    finally:
        if mm is not None:
            mm.close()
        handle.cleanup()


def run_aligned_swapped_truncate(cfg: BenchConfig, target: str) -> str:
    handle = open_target(target, cfg.tmpfs_dir)
    mm = None
    try:
        mm = prepare_shared_mapping(handle, cfg.size_bytes)
        ok, note = pageout_mapping(mm, cfg.size_bytes, cfg.swapped_timeout_s)
        if not ok:
            raise SkipCase(note)
        new_size = align_down(cfg.truncate_keep_bytes, PAGE_SIZE)
        elapsed = timed_call(lambda: os.ftruncate(handle.fd, new_size))
        return f"{elapsed}|{note}"
    finally:
        if mm is not None:
            mm.close()
        handle.cleanup()


def run_unaligned_partial_punch(cfg: BenchConfig, target: str) -> str:
    handle = open_target(target, cfg.tmpfs_dir)
    mm = None
    try:
        mm = prepare_shared_mapping(handle, cfg.size_bytes)
        offset = PAGE_SIZE + 123
        length = min(cfg.size_bytes // 2, 2 * HUGEPAGE_SIZE + 777)
        return str(
            timed_call(
                lambda: checked_fallocate(
                    handle.fd,
                    FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE,
                    offset,
                    length,
                )
            )
        )
    finally:
        if mm is not None:
            mm.close()
        handle.cleanup()


def run_large_folio_partial_punch(cfg: BenchConfig, target: str) -> str:
    mode = shmem_thp_mode()
    if not mode:
        raise SkipCase("cannot read THP shmem mode from sysfs")
    handle = open_target(target, cfg.tmpfs_dir)
    mm = None
    try:
        size = max(cfg.size_bytes, 8 * HUGEPAGE_SIZE)
        mm = prepare_shared_mapping(handle, size, huge_hint=True)
        if not hasattr(mm, "madvise"):
            raise SkipCase("Python mmap.madvise() is unavailable")
        offset = HUGEPAGE_SIZE + PAGE_SIZE
        length = HUGEPAGE_SIZE - 2 * PAGE_SIZE
        elapsed = timed_call(
            lambda: checked_fallocate(
                handle.fd,
                FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE,
                offset,
                length,
            )
        )
        return f"{elapsed}|best-effort THP hint used; shmem_enabled={mode}"
    finally:
        if mm is not None:
            mm.close()
        handle.cleanup()


def tmpfs_capacity_bytes(path: str) -> Tuple[int, int]:
    stats = os.statvfs(path)
    total = stats.f_blocks * stats.f_frsize
    free = stats.f_bavail * stats.f_frsize
    return total, free


def run_failed_fallocate_rollback(cfg: BenchConfig, target: str) -> str:
    if target != "tmpfs":
        raise SkipCase("rollback ENOSPC workload is only auto-generated for tmpfs")

    total, free = tmpfs_capacity_bytes(cfg.tmpfs_dir)
    total_mb = total // (1024 * 1024)
    if total_mb > cfg.rollback_safe_total_mb and cfg.rollback_request_mb is None:
        raise SkipCase(
            "tmpfs is too large for safe auto-ENOSPC rollback; "
            "use --rollback-request-mb to override"
        )

    if free < 32 * 1024 * 1024:
        raise SkipCase("not enough free tmpfs space to safely trigger rollback")

    if cfg.rollback_request_mb is not None:
        request = cfg.rollback_request_mb * 1024 * 1024
    else:
        request = min(free + 16 * 1024 * 1024, total + 16 * 1024 * 1024)

    handle = open_target(target, cfg.tmpfs_dir)
    try:
        start = time.perf_counter_ns()
        checked_fallocate(handle.fd, 0, 0, request)
        elapsed = time.perf_counter_ns() - start
    except OSError as exc:
        elapsed = time.perf_counter_ns() - start
        if exc.errno not in (28, 12):
            handle.cleanup()
            raise
        note = (
            f"expected failure {exc.errno}:{exc.strerror}; "
            f"requested={request // (1024 * 1024)} MiB, "
            f"tmpfs_total={total_mb} MiB, "
            f"tmpfs_free={free // (1024 * 1024)} MiB"
        )
        handle.cleanup()
        return f"{elapsed}|{note}"
    handle.cleanup()
    raise SkipCase(
        "fallocate unexpectedly succeeded; increase --rollback-request-mb "
        "or reduce free tmpfs space"
    )


WORKLOADS: Tuple[Tuple[str, Callable[[BenchConfig, str], str]], ...] = (
    ("aligned_resident_punch", run_aligned_resident_punch),
    ("aligned_resident_truncate", run_aligned_resident_truncate),
    ("aligned_swapped_punch", run_aligned_swapped_punch),
    ("aligned_swapped_truncate", run_aligned_swapped_truncate),
    ("unaligned_partial_punch", run_unaligned_partial_punch),
    ("large_folio_partial_punch", run_large_folio_partial_punch),
    ("failed_fallocate_rollback", run_failed_fallocate_rollback),
)


def decode_elapsed_and_note(raw: str) -> Tuple[int, str]:
    if "|" not in raw:
        return int(raw), ""
    elapsed_raw, note = raw.split("|", 1)
    return int(elapsed_raw), note


def bench_once(
    cfg: BenchConfig,
    workload: str,
    func: Callable[[BenchConfig, str], str],
    target: str,
    iteration: int,
) -> Result:
    try:
        raw = func(cfg, target)
        elapsed_ns, note = decode_elapsed_and_note(raw)
        return Result(
            workload=workload,
            target=target,
            iteration=iteration,
            status="OK",
            elapsed_ns=elapsed_ns,
            note=note,
        )
    except SkipCase as exc:
        return Result(
            workload=workload,
            target=target,
            iteration=iteration,
            status="SKIP",
            elapsed_ns=None,
            note=str(exc),
        )
    except OSError as exc:
        return Result(
            workload=workload,
            target=target,
            iteration=iteration,
            status="ERROR",
            elapsed_ns=None,
            note=f"{exc.errno}:{exc.strerror}",
        )


def summarize(results: List[Result]) -> List[str]:
    lines: List[str] = []
    grouped: Dict[Tuple[str, str], List[Result]] = {}
    for result in results:
        grouped.setdefault((result.workload, result.target), []).append(result)

    for workload, target in sorted(grouped):
        bucket = grouped[(workload, target)]
        oks = [entry.elapsed_ns for entry in bucket if entry.status == "OK" and entry.elapsed_ns is not None]
        skips = [entry.note for entry in bucket if entry.status == "SKIP"]
        errors = [entry.note for entry in bucket if entry.status == "ERROR"]
        lines.append(f"[{workload}] target={target}")
        if oks:
            values = sorted(oks)
            lines.append(
                "  OK   "
                f"n={len(values)} "
                f"min={format_ns(values[0])} "
                f"p50={format_ns(percentile(values, 0.50))} "
                f"p95={format_ns(percentile(values, 0.95))} "
                f"max={format_ns(values[-1])}"
            )
        if skips:
            lines.append(f"  SKIP n={len(skips)} reason={skips[0]}")
        if errors:
            lines.append(f"  ERR  n={len(errors)} first={errors[0]}")
    return lines


def print_environment(cfg: BenchConfig) -> None:
    print("== shmem_undo_range benchmark environment ==")
    print(f"page_size={PAGE_SIZE}")
    print(f"hugepage_size={HUGEPAGE_SIZE}")
    print(f"targets={','.join(cfg.targets)}")
    print(f"iterations={cfg.iterations}")
    print(f"size={cfg.size_bytes // (1024 * 1024)} MiB")
    print(f"tmpfs_dir={cfg.tmpfs_dir}")
    print(f"swap_available={swap_is_available()}")
    mode = shmem_thp_mode()
    print(f"shmem_thp_mode={mode or 'unavailable'}")
    print("")


def parse_args(argv: Sequence[str]) -> BenchConfig:
    parser = argparse.ArgumentParser(
        description="Run tmpfs/memfd workloads that exercise shmem_undo_range()."
    )
    parser.add_argument(
        "--iterations",
        type=int,
        default=5,
        help="iterations per workload/target pair (default: 5)",
    )
    parser.add_argument(
        "--size-mb",
        type=int,
        default=128,
        help="base file size for resident/swapped workloads in MiB (default: 128)",
    )
    parser.add_argument(
        "--tmpfs-dir",
        default="/dev/shm",
        help="tmpfs directory for tmpfs-backed runs (default: /dev/shm)",
    )
    parser.add_argument(
        "--targets",
        default="tmpfs,memfd",
        help="comma-separated target list: tmpfs,memfd (default: tmpfs,memfd)",
    )
    parser.add_argument(
        "--truncate-keep-mb",
        type=int,
        default=64,
        help="truncate resident/swapped files down to this size in MiB (default: 64)",
    )
    parser.add_argument(
        "--swapped-timeout-s",
        type=float,
        default=2.0,
        help="timeout for MADV_PAGEOUT reclaim detection (default: 2.0)",
    )
    parser.add_argument(
        "--rollback-request-mb",
        type=int,
        default=None,
        help="explicit fallocate size for rollback workload in MiB",
    )
    parser.add_argument(
        "--rollback-safe-total-mb",
        type=int,
        default=512,
        help="auto-run rollback only if tmpfs total size is <= this many MiB (default: 512)",
    )
    parser.add_argument(
        "--json-out",
        default=None,
        help="optional path to dump raw per-iteration results as JSON",
    )
    ns = parser.parse_args(argv)

    targets = [item.strip() for item in ns.targets.split(",") if item.strip()]
    for target in targets:
        if target not in ("tmpfs", "memfd"):
            parser.error(f"unsupported target: {target}")

    tmpfs_dir = pathlib.Path(ns.tmpfs_dir)
    if not tmpfs_dir.is_dir():
        parser.error(f"tmpfs dir does not exist: {tmpfs_dir}")

    size_bytes = ns.size_mb * 1024 * 1024
    truncate_keep_bytes = ns.truncate_keep_mb * 1024 * 1024
    if truncate_keep_bytes >= size_bytes:
        parser.error("--truncate-keep-mb must be smaller than --size-mb")

    return BenchConfig(
        iterations=ns.iterations,
        size_bytes=size_bytes,
        tmpfs_dir=str(tmpfs_dir),
        targets=targets,
        truncate_keep_bytes=truncate_keep_bytes,
        swapped_timeout_s=ns.swapped_timeout_s,
        rollback_request_mb=ns.rollback_request_mb,
        rollback_safe_total_mb=ns.rollback_safe_total_mb,
        json_out=ns.json_out,
    )


def main(argv: Sequence[str]) -> int:
    cfg = parse_args(argv)
    print_environment(cfg)

    results: List[Result] = []
    for workload, func in WORKLOADS:
        for target in cfg.targets:
            for iteration in range(1, cfg.iterations + 1):
                result = bench_once(cfg, workload, func, target, iteration)
                results.append(result)
                if result.status == "OK":
                    print(
                        f"OK   workload={workload} target={target} "
                        f"iter={iteration} elapsed={format_ns(result.elapsed_ns or 0)} "
                        f"{result.note}".rstrip()
                    )
                else:
                    print(
                        f"{result.status:<4} workload={workload} target={target} "
                        f"iter={iteration} note={result.note}"
                    )
            print("")

    print("== summary ==")
    for line in summarize(results):
        print(line)

    if cfg.json_out:
        with open(cfg.json_out, "w", encoding="utf-8") as handle:
            json.dump([asdict(result) for result in results], handle, indent=2)
            handle.write("\n")

    return 0 if not any(result.status == "ERROR" for result in results) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

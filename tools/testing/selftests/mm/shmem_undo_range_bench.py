#!/usr/bin/env python3
from __future__ import annotations
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
import contextlib
import ctypes
import json
import math
import mmap
import os
import pathlib
import select
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, asdict
from typing import Callable, Dict, Iterator, List, Optional, Sequence, Tuple


PAGE_SIZE = os.sysconf("SC_PAGE_SIZE")
HUGEPAGE_SIZE = 2 * 1024 * 1024
FALLOC_FL_KEEP_SIZE = 0x01
FALLOC_FL_PUNCH_HOLE = 0x02
MADV_HUGEPAGE = 14
MADV_PAGEOUT = 21
PAGEMAP_PRESENT = 1 << 63
PAGEMAP_SWAPPED = 1 << 62
PAGEMAP_ENTRY_BYTES = 8
MIB = 1024 * 1024


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


def format_mib(value: int) -> str:
    return f"{value / MIB:.2f} MiB"


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


def read_meminfo_kb(key: str) -> int:
    prefix = f"{key}:"
    try:
        with open("/proc/meminfo", "r", encoding="utf-8") as handle:
            for line in handle:
                if line.startswith(prefix):
                    return int(line.split()[1])
    except OSError:
        return 0
    return 0


def read_first_line(path: pathlib.Path) -> str:
    try:
        return path.read_text(encoding="utf-8").strip()
    except OSError:
        return ""


def swap_is_available() -> bool:
    try:
        with open("/proc/swaps", "r", encoding="utf-8") as handle:
            lines = handle.readlines()
    except OSError:
        return False
    return len(lines) > 1


def swap_total_bytes() -> int:
    return read_meminfo_kb("SwapTotal") * 1024


def vm_swappiness() -> int:
    text = read_text("/proc/sys/vm/swappiness").strip()
    return int(text) if text else -1


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


def open_pagemap_fd() -> Optional[int]:
    try:
        return os.open("/proc/self/pagemap", os.O_RDONLY)
    except OSError:
        return None


def mapping_address(mm: mmap.mmap) -> int:
    return ctypes.addressof(ctypes.c_char.from_buffer(mm))


def pagemap_entry(fd: int, vaddr: int) -> int:
    offset = (vaddr // PAGE_SIZE) * PAGEMAP_ENTRY_BYTES
    data = os.pread(fd, PAGEMAP_ENTRY_BYTES, offset)
    if len(data) != PAGEMAP_ENTRY_BYTES:
        raise OSError("short read from /proc/self/pagemap")
    return int.from_bytes(data, "little")


def count_swapped_pages_pagemap(mm: mmap.mmap, size: int) -> Optional[int]:
    fd = open_pagemap_fd()
    if fd is None:
        return None

    base = mapping_address(mm)
    pages = align_up(size, PAGE_SIZE) // PAGE_SIZE
    swapped = 0
    try:
        for idx in range(pages):
            entry = pagemap_entry(fd, base + idx * PAGE_SIZE)
            if entry & PAGEMAP_SWAPPED:
                swapped += 1
    except OSError:
        return None
    finally:
        os.close(fd)

    return swapped


def mapping_swap_kb_smaps(mm: mmap.mmap) -> Optional[int]:
    target = mapping_address(mm)
    try:
        with open("/proc/self/smaps", "r", encoding="utf-8") as handle:
            in_block = False
            for line in handle:
                if "-" in line and ":" not in line:
                    fields = line.split(None, 1)
                    start_hex, end_hex = fields[0].split("-", 1)
                    start = int(start_hex, 16)
                    end = int(end_hex, 16)
                    in_block = start <= target < end
                    continue
                if in_block and line.startswith("Swap:"):
                    return int(line.split()[1])
    except OSError:
        return None
    return None


def swapped_pages_for_mapping(mm: mmap.mmap, size: int) -> Tuple[Optional[int], str]:
    swapped = count_swapped_pages_pagemap(mm, size)
    if swapped is not None:
        return swapped, "pagemap"

    swap_kb = mapping_swap_kb_smaps(mm)
    if swap_kb is None:
        return None, "unavailable"
    return (swap_kb * 1024) // PAGE_SIZE, "smaps"


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


def global_swap_used_bytes() -> int:
    total = read_meminfo_kb("SwapTotal") * 1024
    free = read_meminfo_kb("SwapFree") * 1024
    return max(0, total - free)


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


def pages_for_fraction(size: int, fraction: float) -> int:
    total_pages = align_up(size, PAGE_SIZE) // PAGE_SIZE
    return max(1, math.ceil(total_pages * fraction))


def pageout_mapping(
    mm: mmap.mmap,
    size: int,
    timeout_s: float,
    min_fraction: float,
    pressure_desc: str,
    baseline: SwapState,
) -> Tuple[bool, str, int]:
    if not swap_is_available():
        return False, "swap is not configured on this system", 0
    if not maybe_madvise(mm, MADV_PAGEOUT):
        return False, "Python mmap.madvise(MADV_PAGEOUT) is unavailable", 0

    start = time.monotonic()
    min_swapped_pages = pages_for_fraction(size, min_fraction)
    method = "unknown"

    while time.monotonic() - start < timeout_s:
        maybe_madvise(mm, MADV_PAGEOUT)
        time.sleep(0.05)
        swapped_pages, method = swapped_pages_for_mapping(mm, size)
        current = capture_swap_state(mm, size)
        global_swap_delta = max(0, current.global_swap_bytes - baseline.global_swap_bytes)
        cgroup_swap_delta = None
        if baseline.cgroup_swap_bytes is not None and current.cgroup_swap_bytes is not None:
            cgroup_swap_delta = max(0, current.cgroup_swap_bytes - baseline.cgroup_swap_bytes)
        resident_drop_fraction = max(0.0, baseline.resident_fraction - current.resident_fraction)
        resident_dropped_pages = pages_for_fraction(size, resident_drop_fraction)
        if swapped_pages is not None and swapped_pages >= min_swapped_pages:
            return (
                True,
                f"swapped_pages={swapped_pages}/{align_up(size, PAGE_SIZE) // PAGE_SIZE} "
                f"via {method}; resident fraction {baseline.resident_fraction:.2f} -> "
                f"{current.resident_fraction:.2f}; "
                f"pressure={pressure_desc}",
                swapped_pages,
            )
        if resident_dropped_pages >= min_swapped_pages and (
            global_swap_delta >= min_swapped_pages * PAGE_SIZE
            or (cgroup_swap_delta is not None and cgroup_swap_delta >= min_swapped_pages * PAGE_SIZE)
        ):
            note = (
                f"resident drop matched swap growth; resident fraction "
                f"{baseline.resident_fraction:.2f} -> {current.resident_fraction:.2f}; "
                f"global_swap_delta={format_mib(global_swap_delta)}"
            )
            if cgroup_swap_delta is not None:
                note += f"; cgroup_swap_delta={format_mib(cgroup_swap_delta)}"
            note += f"; pressure={pressure_desc}"
            return True, note, resident_dropped_pages

    swapped_pages, method = swapped_pages_for_mapping(mm, size)
    current = capture_swap_state(mm, size)
    global_swap_delta = max(0, current.global_swap_bytes - baseline.global_swap_bytes)
    cgroup_swap_delta = None
    if baseline.cgroup_swap_bytes is not None and current.cgroup_swap_bytes is not None:
        cgroup_swap_delta = max(0, current.cgroup_swap_bytes - baseline.cgroup_swap_bytes)
    return (
        False,
        "swap verification did not reach threshold "
        f"({0 if swapped_pages is None else swapped_pages} pages via {method}; "
        f"resident fraction {baseline.resident_fraction:.2f} -> {current.resident_fraction:.2f}; "
        f"global_swap_delta={format_mib(global_swap_delta)}"
        f"{'' if cgroup_swap_delta is None else f'; cgroup_swap_delta={format_mib(cgroup_swap_delta)}'}; "
        f"pressure={pressure_desc})",
        0 if swapped_pages is None else swapped_pages,
    )


def swap_failure_hint(cfg: BenchConfig) -> str:
    mem_available = read_meminfo_kb("MemAvailable") * 1024
    configured = (
        cfg.swap_pressure_bytes
        if cfg.swap_pressure_bytes is not None
        else default_swap_pressure_bytes(cfg)
    )
    return (
        f"configured_pressure={format_mib(configured)}, "
        f"mem_available={format_mib(mem_available)}, "
        "if swap stays at 0 pages, increase --swap-pressure-mb substantially "
        "or use --swap-mode memcg on a writable cgroup v2 system"
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
    range_bytes: int
    range_pages: int
    ns_per_page: Optional[float]
    ns_per_mib: Optional[float]


@dataclass
class BenchConfig:
    iterations: int
    size_bytes: int
    tmpfs_dir: str
    targets: List[str]
    truncate_keep_bytes: int
    swapped_timeout_s: float
    swapped_min_fraction: float
    swap_mode: str
    swap_pressure_bytes: Optional[int]
    swap_pressure_chunk_bytes: int
    swap_pressure_ready_timeout_s: float
    swap_memcg_max_bytes: Optional[int]
    swap_memcg_swap_max_bytes: Optional[int]
    rollback_request_mb: Optional[int]
    rollback_safe_total_mb: int
    rollback_private_tmpfs_mb: int
    json_out: Optional[str]
    trace_suggest: bool


@dataclass
class WorkloadOutcome:
    elapsed_ns: int
    range_bytes: int
    range_pages: int
    note: str = ""


@dataclass
class SwapState:
    resident_fraction: float
    global_swap_bytes: int
    cgroup_swap_bytes: Optional[int]


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


def bytes_to_pages(size: int) -> int:
    return align_up(size, PAGE_SIZE) // PAGE_SIZE


def pressure_child(
    ready_fd: int,
    stop_fd: int,
    total_bytes: int,
    chunk_bytes: int,
) -> None:
    allocations: List[mmap.mmap] = []
    allocated = 0
    try:
        while allocated < total_bytes:
            chunk = min(chunk_bytes, total_bytes - allocated)
            mm = mmap.mmap(
                -1,
                chunk,
                flags=mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS,
                prot=mmap.PROT_READ | mmap.PROT_WRITE,
            )
            touch_mapping(mm, chunk)
            allocations.append(mm)
            allocated += chunk
            os.write(ready_fd, f"{allocated}\n".encode("ascii"))
            time.sleep(0.02)
        os.write(ready_fd, b"DONE\n")
        os.read(stop_fd, 1)
    finally:
        for mm in allocations:
            mm.close()
        os._exit(0)


@contextlib.contextmanager
def pressure_process(
    total_bytes: int,
    chunk_bytes: int,
    ready_timeout_s: float,
) -> Iterator[str]:
    if total_bytes <= 0:
        yield "pressure=disabled"
        return

    ready_r, ready_w = os.pipe()
    stop_r, stop_w = os.pipe()
    pid = os.fork()
    if pid == 0:
        os.close(ready_r)
        os.close(stop_w)
        pressure_child(ready_w, stop_r, total_bytes, chunk_bytes)

    os.close(ready_w)
    os.close(stop_r)
    try:
        progress = 0
        done = False
        buf = b""
        deadline = time.monotonic() + ready_timeout_s
        while not done:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise SkipCase(
                    f"pressure helper timed out after {ready_timeout_s:.1f}s "
                    f"at progress={format_mib(progress)}"
                )
            readable, _, _ = select.select([ready_r], [], [], remaining)
            if not readable:
                raise SkipCase(
                    f"pressure helper timed out after {ready_timeout_s:.1f}s "
                    f"at progress={format_mib(progress)}"
                )
            data = os.read(ready_r, 4096)
            if not data:
                raise SkipCase(
                    f"pressure helper exited early before target pressure; "
                    f"progress={format_mib(progress)}"
                )
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("ascii", "replace").strip()
                if not text:
                    continue
                if text == "DONE":
                    done = True
                    break
                progress = int(text)
        yield (
            f"mode=process helper_pid={pid} "
            f"final_progress={format_mib(progress)} "
            f"target={format_mib(total_bytes)} chunk={format_mib(chunk_bytes)}"
        )
    finally:
        try:
            os.write(stop_w, b"q")
        except OSError:
            pass
        os.close(stop_w)
        os.close(ready_r)
        os.waitpid(pid, 0)


def current_cgroup_relpath() -> str:
    try:
        with open("/proc/self/cgroup", "r", encoding="utf-8") as handle:
            for line in handle:
                if line.startswith("0::"):
                    rel = line.strip().split("::", 1)[1]
                    return rel if rel else "/"
    except OSError:
        return "/"
    return "/"


def cgroup_v2_root() -> Optional[pathlib.Path]:
    root = pathlib.Path("/sys/fs/cgroup")
    if (root / "cgroup.controllers").exists():
        return root
    return None


def current_cgroup_dir() -> Optional[pathlib.Path]:
    root = cgroup_v2_root()
    if root is None:
        return None
    rel = current_cgroup_relpath().strip("/")
    return root / rel if rel else root


def write_text(path: pathlib.Path, value: str) -> None:
    path.write_text(value, encoding="utf-8")


def read_int_from_path(path: pathlib.Path) -> Optional[int]:
    text = read_first_line(path)
    if not text:
        return None
    try:
        return int(text)
    except ValueError:
        return None


def controller_enabled(path: pathlib.Path, controller: str) -> bool:
    value = read_first_line(path / "cgroup.subtree_control")
    return controller in value.split()


def controller_available(path: pathlib.Path, controller: str) -> bool:
    value = read_first_line(path / "cgroup.controllers")
    return controller in value.split()


def ensure_memory_controller(parent: pathlib.Path) -> None:
    if controller_enabled(parent, "memory"):
        return
    if not controller_available(parent, "memory"):
        raise SkipCase(f"parent cgroup {parent} does not expose memory controller")
    try:
        write_text(parent / "cgroup.subtree_control", "+memory\n")
    except OSError as exc:
        raise SkipCase(
            f"cannot enable memory controller in {parent}/cgroup.subtree_control: "
            f"{exc.errno}:{exc.strerror}"
        ) from exc


def checked_group_mkdir(path: pathlib.Path) -> None:
    try:
        path.mkdir()
    except OSError as exc:
        raise SkipCase(
            f"cannot create cgroup {path}: {exc.errno}:{exc.strerror}"
        ) from exc


def checked_group_write(path: pathlib.Path, value: str, desc: str) -> None:
    try:
        write_text(path, value)
    except OSError as exc:
        raise SkipCase(
            f"cannot write {desc} at {path}: {exc.errno}:{exc.strerror}"
        ) from exc


@contextlib.contextmanager
def maybe_swap_memcg(cfg: BenchConfig) -> Iterator[str]:
    if cfg.swap_mode != "memcg":
        yield "mode=process"
        return

    root = cgroup_v2_root()
    if root is None:
        raise SkipCase("cgroup v2 is unavailable for --swap-mode memcg")
    if cfg.swap_memcg_max_bytes is None:
        raise SkipCase("--swap-memcg-max-mb is required for --swap-mode memcg")

    rel = current_cgroup_relpath().strip("/")
    parent = root / rel if rel else root
    name = f"shmem_undo_range_{os.getpid()}_{int(time.time())}"
    group = parent / name
    original = parent / "cgroup.procs"
    ensure_memory_controller(parent)
    checked_group_mkdir(group)
    try:
        checked_group_write(
            group / "memory.max",
            f"{cfg.swap_memcg_max_bytes}\n",
            "memory.max",
        )
        if cfg.swap_memcg_swap_max_bytes is not None:
            checked_group_write(
                group / "memory.swap.max",
                f"{cfg.swap_memcg_swap_max_bytes}\n",
                "memory.swap.max",
            )
        checked_group_write(group / "cgroup.procs", f"{os.getpid()}\n", "cgroup.procs")
        yield (
            f"mode=memcg path={group} "
            f"memory.max={format_mib(cfg.swap_memcg_max_bytes)} "
            f"memory.swap.max="
            f"{'max' if cfg.swap_memcg_swap_max_bytes is None else format_mib(cfg.swap_memcg_swap_max_bytes)}"
        )
    finally:
        try:
            checked_group_write(original, f"{os.getpid()}\n", "restore parent cgroup.procs")
        except SkipCase:
            pass
        try:
            group.rmdir()
        except OSError:
            pass


def default_swap_pressure_bytes(cfg: BenchConfig) -> int:
    mem_available = read_meminfo_kb("MemAvailable") * 1024
    reserve = 512 * MIB
    upper = max(0, mem_available - reserve)
    target = max(cfg.size_bytes * 8, int(mem_available * 0.75))
    if upper:
        return min(target, upper) if upper >= target else upper
    return target


def capture_swap_state(mm: mmap.mmap, size: int) -> SwapState:
    cgdir = current_cgroup_dir()
    cgroup_swap = None
    if cgdir is not None:
        cgroup_swap = read_int_from_path(cgdir / "memory.swap.current")
    return SwapState(
        resident_fraction=mincore_resident_fraction(mm, size),
        global_swap_bytes=global_swap_used_bytes(),
        cgroup_swap_bytes=cgroup_swap,
    )


def make_outcome(elapsed_ns: int, range_bytes: int, note: str = "") -> WorkloadOutcome:
    return WorkloadOutcome(
        elapsed_ns=elapsed_ns,
        range_bytes=range_bytes,
        range_pages=bytes_to_pages(range_bytes),
        note=note,
    )


def run_aligned_resident_punch(cfg: BenchConfig, target: str) -> WorkloadOutcome:
    handle = open_target(target, cfg.tmpfs_dir)
    mm = None
    try:
        mm = prepare_shared_mapping(handle, cfg.size_bytes)
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
        return make_outcome(elapsed, length)
    finally:
        if mm is not None:
            mm.close()
        handle.cleanup()


def run_aligned_resident_truncate(cfg: BenchConfig, target: str) -> WorkloadOutcome:
    handle = open_target(target, cfg.tmpfs_dir)
    mm = None
    try:
        mm = prepare_shared_mapping(handle, cfg.size_bytes)
        new_size = align_down(cfg.truncate_keep_bytes, PAGE_SIZE)
        elapsed = timed_call(lambda: os.ftruncate(handle.fd, new_size))
        return make_outcome(elapsed, cfg.size_bytes - new_size)
    finally:
        if mm is not None:
            mm.close()
        handle.cleanup()


def run_aligned_swapped_punch(cfg: BenchConfig, target: str) -> WorkloadOutcome:
    pressure_bytes = (
        cfg.swap_pressure_bytes
        if cfg.swap_pressure_bytes is not None
        else default_swap_pressure_bytes(cfg)
    )
    with maybe_swap_memcg(cfg) as memcg_note:
        handle = open_target(target, cfg.tmpfs_dir)
        mm = None
        try:
            mm = prepare_shared_mapping(handle, cfg.size_bytes)
            baseline = capture_swap_state(mm, cfg.size_bytes)
            with pressure_process(
                pressure_bytes,
                cfg.swap_pressure_chunk_bytes,
                cfg.swap_pressure_ready_timeout_s,
            ) as pressure_note:
                ok, note, _swapped_pages = pageout_mapping(
                    mm,
                    cfg.size_bytes,
                    cfg.swapped_timeout_s,
                    cfg.swapped_min_fraction,
                    f"{memcg_note}; {pressure_note}",
                    baseline,
                )
                if not ok:
                    raise SkipCase(
                        f"{note}; {swap_failure_hint(cfg)}"
                    )
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
                return make_outcome(elapsed, length, note)
        finally:
            if mm is not None:
                mm.close()
            handle.cleanup()


def run_aligned_swapped_truncate(cfg: BenchConfig, target: str) -> WorkloadOutcome:
    pressure_bytes = (
        cfg.swap_pressure_bytes
        if cfg.swap_pressure_bytes is not None
        else default_swap_pressure_bytes(cfg)
    )
    with maybe_swap_memcg(cfg) as memcg_note:
        handle = open_target(target, cfg.tmpfs_dir)
        mm = None
        try:
            mm = prepare_shared_mapping(handle, cfg.size_bytes)
            baseline = capture_swap_state(mm, cfg.size_bytes)
            with pressure_process(
                pressure_bytes,
                cfg.swap_pressure_chunk_bytes,
                cfg.swap_pressure_ready_timeout_s,
            ) as pressure_note:
                ok, note, _swapped_pages = pageout_mapping(
                    mm,
                    cfg.size_bytes,
                    cfg.swapped_timeout_s,
                    cfg.swapped_min_fraction,
                    f"{memcg_note}; {pressure_note}",
                    baseline,
                )
                if not ok:
                    raise SkipCase(
                        f"{note}; {swap_failure_hint(cfg)}"
                    )
                new_size = align_down(cfg.truncate_keep_bytes, PAGE_SIZE)
                elapsed = timed_call(lambda: os.ftruncate(handle.fd, new_size))
                return make_outcome(elapsed, cfg.size_bytes - new_size, note)
        finally:
            if mm is not None:
                mm.close()
            handle.cleanup()


def run_unaligned_partial_punch(cfg: BenchConfig, target: str) -> WorkloadOutcome:
    handle = open_target(target, cfg.tmpfs_dir)
    mm = None
    try:
        mm = prepare_shared_mapping(handle, cfg.size_bytes)
        offset = PAGE_SIZE + 123
        length = min(cfg.size_bytes // 2, 2 * HUGEPAGE_SIZE + 777)
        elapsed = timed_call(
            lambda: checked_fallocate(
                handle.fd,
                FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE,
                offset,
                length,
            )
        )
        return make_outcome(elapsed, length)
    finally:
        if mm is not None:
            mm.close()
        handle.cleanup()


def run_large_folio_partial_punch(cfg: BenchConfig, target: str) -> WorkloadOutcome:
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
        return make_outcome(
            elapsed,
            length,
            f"best-effort THP hint used; shmem_enabled={mode}",
        )
    finally:
        if mm is not None:
            mm.close()
        handle.cleanup()


def tmpfs_capacity_bytes(path: str) -> Tuple[int, int]:
    stats = os.statvfs(path)
    total = stats.f_blocks * stats.f_frsize
    free = stats.f_bavail * stats.f_frsize
    return total, free


def write_filler_file(directory: str, fill_bytes: int) -> TargetHandle:
    if fill_bytes <= 0:
        raise SkipCase("rollback filler size must be positive")

    handle = open_target("tmpfs", directory)
    buf = b"\0" * min(4 * MIB, fill_bytes)
    written = 0
    try:
        while written < fill_bytes:
            chunk = min(len(buf), fill_bytes - written)
            n = os.write(handle.fd, buf[:chunk])
            if n <= 0:
                raise OSError("short write while creating rollback filler")
            written += n
        os.fsync(handle.fd)
        return handle
    except Exception:
        handle.cleanup()
        raise


@contextlib.contextmanager
def rollback_tmpfs_dir(cfg: BenchConfig) -> Iterator[str]:
    if cfg.rollback_private_tmpfs_mb <= 0:
        yield cfg.tmpfs_dir
        return

    mount_point = tempfile.mkdtemp(prefix="shmem-undo-range-rollback-")
    cmd = [
        "mount",
        "-t",
        "tmpfs",
        "-o",
        f"size={cfg.rollback_private_tmpfs_mb}M",
        "tmpfs",
        mount_point,
    ]
    try:
        subprocess.run(cmd, check=True, capture_output=True, text=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        shutil.rmtree(mount_point, ignore_errors=True)
        raise SkipCase(
            f"failed to mount private rollback tmpfs ({exc}); set --rollback-private-tmpfs-mb 0 to disable"
        )

    try:
        yield mount_point
    finally:
        subprocess.run(["umount", mount_point], check=False)
        shutil.rmtree(mount_point, ignore_errors=True)


def choose_rollback_request_bytes(total: int, cfg: BenchConfig) -> int:
    if cfg.rollback_request_mb is not None:
        request = cfg.rollback_request_mb * MIB
    else:
        request = min(128 * MIB, align_down(total // 2, PAGE_SIZE))

    request = align_down(request, PAGE_SIZE)
    request = min(request, max(PAGE_SIZE, align_down(total - 16 * MIB, PAGE_SIZE)))
    if request < PAGE_SIZE:
        raise SkipCase("tmpfs is too small for rollback request sizing")
    return request


def run_failed_fallocate_rollback(cfg: BenchConfig, target: str) -> WorkloadOutcome:
    if target != "tmpfs":
        raise SkipCase("rollback ENOSPC workload is only auto-generated for tmpfs")

    with rollback_tmpfs_dir(cfg) as rollback_dir:
        total, free = tmpfs_capacity_bytes(rollback_dir)
        total_mb = total // MIB
        if total_mb > cfg.rollback_safe_total_mb and cfg.rollback_request_mb is None:
            raise SkipCase(
                "tmpfs is too large for safe auto-ENOSPC rollback; "
                "use --rollback-request-mb or private rollback tmpfs override"
            )

        if free < 32 * MIB:
            raise SkipCase("not enough free tmpfs space to safely trigger rollback")

        request = choose_rollback_request_bytes(total, cfg)
        target_free = align_down(max(16 * MIB, request // 2), PAGE_SIZE)
        if target_free >= request:
            target_free = align_down(request - PAGE_SIZE, PAGE_SIZE)
        fill_bytes = align_down(max(0, free - target_free), PAGE_SIZE)
        if fill_bytes <= 0:
            raise SkipCase("rollback filler sizing produced no mid-allocation failure window")

        filler = write_filler_file(rollback_dir, fill_bytes)
        handle = open_target(target, rollback_dir)
        try:
            start = time.perf_counter_ns()
            checked_fallocate(handle.fd, 0, 0, request)
            elapsed = time.perf_counter_ns() - start
        except OSError as exc:
            elapsed = time.perf_counter_ns() - start
            if exc.errno not in (28, 12):
                filler.cleanup()
                handle.cleanup()
                raise
            note = (
                f"expected mid-allocation failure {exc.errno}:{exc.strerror}; "
                f"requested={request // MIB} MiB, "
                f"rollback_tmpfs_total={total_mb} MiB, "
                f"prefill={fill_bytes // MIB} MiB, "
                f"target_free={target_free // MIB} MiB"
            )
            filler.cleanup()
            handle.cleanup()
            return make_outcome(elapsed, request, note)

        filler.cleanup()
        handle.cleanup()
        raise SkipCase(
            "fallocate unexpectedly succeeded; increase --rollback-request-mb "
            "or reduce remaining tmpfs space inside the rollback mount"
        )


WORKLOADS: Tuple[Tuple[str, Callable[[BenchConfig, str], WorkloadOutcome]], ...] = (
    ("aligned_resident_punch", run_aligned_resident_punch),
    ("aligned_resident_truncate", run_aligned_resident_truncate),
    ("aligned_swapped_punch", run_aligned_swapped_punch),
    ("aligned_swapped_truncate", run_aligned_swapped_truncate),
    ("unaligned_partial_punch", run_unaligned_partial_punch),
    ("large_folio_partial_punch", run_large_folio_partial_punch),
    ("failed_fallocate_rollback", run_failed_fallocate_rollback),
)


def bench_once(
    cfg: BenchConfig,
    workload: str,
    func: Callable[[BenchConfig, str], WorkloadOutcome],
    target: str,
    iteration: int,
) -> Result:
    try:
        outcome = func(cfg, target)
        ns_per_page = (
            outcome.elapsed_ns / outcome.range_pages
            if outcome.range_pages
            else None
        )
        ns_per_mib = (
            outcome.elapsed_ns / (outcome.range_bytes / MIB)
            if outcome.range_bytes
            else None
        )
        return Result(
            workload=workload,
            target=target,
            iteration=iteration,
            status="OK",
            elapsed_ns=outcome.elapsed_ns,
            note=outcome.note,
            range_bytes=outcome.range_bytes,
            range_pages=outcome.range_pages,
            ns_per_page=ns_per_page,
            ns_per_mib=ns_per_mib,
        )
    except SkipCase as exc:
        return Result(
            workload=workload,
            target=target,
            iteration=iteration,
            status="SKIP",
            elapsed_ns=None,
            note=str(exc),
            range_bytes=0,
            range_pages=0,
            ns_per_page=None,
            ns_per_mib=None,
        )
    except OSError as exc:
        return Result(
            workload=workload,
            target=target,
            iteration=iteration,
            status="ERROR",
            elapsed_ns=None,
            note=f"{exc.errno}:{exc.strerror}",
            range_bytes=0,
            range_pages=0,
            ns_per_page=None,
            ns_per_mib=None,
        )


def format_ratio(values: Sequence[float], unit: str) -> str:
    sorted_values = sorted(values)
    if not sorted_values:
        return ""
    return (
        f"p50={percentile([int(v) for v in sorted_values], 0.50):.0f} {unit} "
        f"p95={percentile([int(v) for v in sorted_values], 0.95):.0f} {unit}"
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
        per_page = [entry.ns_per_page for entry in bucket if entry.ns_per_page is not None]
        per_mib = [entry.ns_per_mib for entry in bucket if entry.ns_per_mib is not None]
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
        if per_page:
            lines.append(f"  RATE ns/page {format_ratio(per_page, 'ns/page')}")
        if per_mib:
            lines.append(f"  RATE ns/MiB  {format_ratio(per_mib, 'ns/MiB')}")
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
    print(f"swap_total={format_mib(swap_total_bytes())}")
    print(f"vm_swappiness={vm_swappiness()}")
    print(f"swap_mode={cfg.swap_mode}")
    print(f"cgroup_path={current_cgroup_relpath()}")
    print(f"swapped_min_fraction={cfg.swapped_min_fraction}")
    print(f"swap_pressure_ready_timeout_s={cfg.swap_pressure_ready_timeout_s}")
    mode = shmem_thp_mode()
    print(f"shmem_thp_mode={mode or 'unavailable'}")
    print("")


def print_trace_suggestions(cfg: BenchConfig) -> None:
    script = pathlib.Path(__file__)
    print("")
    print("== trace suggestions ==")
    print(
        "tracepoint summary: "
        f"sudo trace-cmd record -e shmem:shmem_undo_range_stats "
        f"python3 {script} --iterations 1 --targets tmpfs"
    )
    print(
        "function_graph: "
        f"sudo trace-cmd record -p function_graph -l shmem_undo_range "
        f"-l find_lock_entries -l find_get_entries -l truncate_inode_partial_folio "
        f"-l shmem_free_swap -l shmem_confirm_swap "
        f"python3 {script} --iterations 1 --targets tmpfs"
    )
    print(
        "perf probe example: "
        f"sudo perf stat -e probe:shmem_undo_range python3 {script} --iterations 1"
    )
    print(
        "bpftrace latency example: "
        "sudo bpftrace -e 'kprobe:shmem_undo_range { @ts[tid] = nsecs; } "
        "kretprobe:shmem_undo_range /@ts[tid]/ { @us = hist((nsecs-@ts[tid])/1000); delete(@ts[tid]); }'"
    )
    print(
        "bpftrace unfalloc example: "
        "sudo bpftrace -e 'kprobe:shmem_undo_range { @[arg3] = count(); }'"
    )


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
        "--swapped-min-fraction",
        type=float,
        default=0.10,
        help="minimum fraction of pages that must verify as swapped (default: 0.10)",
    )
    parser.add_argument(
        "--swap-mode",
        choices=("process", "memcg"),
        default="process",
        help="swap pressure mode: helper process or cgroup-v2-constrained helper (default: process)",
    )
    parser.add_argument(
        "--swap-pressure-mb",
        type=int,
        default=None,
        help="maximum helper-process memory pressure in MiB",
    )
    parser.add_argument(
        "--swap-pressure-chunk-mb",
        type=int,
        default=64,
        help="allocation chunk for the pressure helper in MiB (default: 64)",
    )
    parser.add_argument(
        "--swap-pressure-ready-timeout-s",
        type=float,
        default=120.0,
        help="how long to wait for the pressure helper to reach its target allocation (default: 120)",
    )
    parser.add_argument(
        "--swap-memcg-max-mb",
        type=int,
        default=None,
        help="memory.max in MiB for --swap-mode memcg",
    )
    parser.add_argument(
        "--swap-memcg-swap-max-mb",
        type=int,
        default=None,
        help="memory.swap.max in MiB for --swap-mode memcg; omit for max",
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
        "--rollback-private-tmpfs-mb",
        type=int,
        default=256,
        help="size of a private tmpfs mount used only for rollback workload; 0 disables it (default: 256)",
    )
    parser.add_argument(
        "--json-out",
        default=None,
        help="optional path to dump raw per-iteration results as JSON",
    )
    parser.add_argument(
        "--trace-suggest",
        action="store_true",
        help="print suggested trace-cmd/perf/bpftrace commands after the summary",
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
        swapped_min_fraction=ns.swapped_min_fraction,
        swap_mode=ns.swap_mode,
        swap_pressure_bytes=(None if ns.swap_pressure_mb is None else ns.swap_pressure_mb * MIB),
        swap_pressure_chunk_bytes=ns.swap_pressure_chunk_mb * MIB,
        swap_pressure_ready_timeout_s=ns.swap_pressure_ready_timeout_s,
        swap_memcg_max_bytes=(None if ns.swap_memcg_max_mb is None else ns.swap_memcg_max_mb * MIB),
        swap_memcg_swap_max_bytes=(
            None if ns.swap_memcg_swap_max_mb is None else ns.swap_memcg_swap_max_mb * MIB
        ),
        rollback_request_mb=ns.rollback_request_mb,
        rollback_safe_total_mb=ns.rollback_safe_total_mb,
        rollback_private_tmpfs_mb=ns.rollback_private_tmpfs_mb,
        json_out=ns.json_out,
        trace_suggest=ns.trace_suggest,
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
                        f"range={format_mib(result.range_bytes)} "
                        f"ns/page={result.ns_per_page:.0f} "
                        f"ns/MiB={result.ns_per_mib:.0f} "
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

    if cfg.trace_suggest:
        print_trace_suggestions(cfg)

    if cfg.json_out:
        with open(cfg.json_out, "w", encoding="utf-8") as handle:
            json.dump([asdict(result) for result in results], handle, indent=2)
            handle.write("\n")

    return 0 if not any(result.status == "ERROR" for result in results) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

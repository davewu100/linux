#!/usr/bin/env python3
"""
Example userspace code for parsing TLV-encoded memory cgroup statistics.

The kernel encodes memory cgroup statistics in TLV (Type-Length-Value) format.
This script parses the binary TLV data and displays human-readable statistics.

Usage:
    python3 test_usage.py [tlv_file]

If no file is specified, reads from /sys/kernel/debug/memcg_stats/memory.stat_bin
"""

import struct
import sys
from pathlib import Path

# TLV type definitions
# The kernel uses stat_idx + 1 as TLV type, where stat_idx is the enum value
# from node_stat_item or memcg_stat_item enums.
#
# Note: memory.stat_bin only encodes non-NUMA-aware stats. Most stats like
# anon, file, shmem are NUMA-aware and are NOT in the binary version.
TLV_TYPES = {
    # Non-NUMA-aware memory stats (encoded in order from memory_stats[] array)
    # These are the only stats encoded in memory.stat_bin (non-NUMA-aware ones)
    # Type mapping uses stat_idx + 1, where stat_idx is the enum value
    # Based on actual encoding order and values:
    53: "sock",             # MEMCG_SOCK (pages, needs * PAGE_SIZE)
    54: "percpu",           # MEMCG_PERCPU_B (bytes, unit=1, no conversion)
    55: "vmalloc",          # MEMCG_VMALLOC (pages, needs * PAGE_SIZE)
    56: "kernel",           # MEMCG_KMEM (pages, needs * PAGE_SIZE)
    57: "zswap",            # MEMCG_ZSWAP_B (bytes, unit=1, no conversion)
    58: "zswapped",         # MEMCG_ZSWAPPED (count, unit=1, no conversion)
    
    # Combined slab stat (TLV_TYPE_MEMORY_SLAB)
    199: "slab",            # Combined slab (bytes, unit=1, no conversion)

    # VM events (starting at 200, dynamically mapped from memcg_vm_event_stat[])
    # These are event counts, not memory sizes
    # Mapping: type = 200 + array_index (without CONFIG_MEMCG_V1)
    200: "pswpin",
    201: "pswpout",
    202: "pgscan_kswapd",
    203: "pgscan_direct",
    204: "pgscan_khugepaged",
    205: "pgscan_proactive",
    206: "pgsteal_kswapd",
    207: "pgsteal_direct",
    208: "pgsteal_khugepaged",
    209: "pgsteal_proactive",
    210: "pgfault",           # Index 10
    211: "pgmajfault",        # Index 11
    212: "pgrefill",
    213: "pgactivate",
    214: "pgdeactivate",
    215: "pglazyfree",
    216: "pglazyfreed",
    217: "swpin_zero",
    218: "swpout_zero",
    219: "zswpin",
    220: "zswpout",
    221: "zswpwb",
    222: "thp_fault_alloc",
    223: "thp_collapse_alloc",
    224: "thp_swpout",        # Note: text file uses "thp_swpout", binary uses "thp_swout"
    225: "thp_swpout_fallback",  # Note: text file uses "thp_swpout_fallback", binary uses "thp_swout_fallback"
    226: "numa_page_migrate",
    227: "numa_pte_updates",
    228: "numa_hint_faults",
}

# Statistics that need unit conversion from pages to bytes
# Based on memcg_page_state_unit():
# - MEMCG_PERCPU_B, MEMCG_ZSWAP_B, NR_SLAB_*_B: unit=1 (already bytes)
# - NR_KERNEL_STACK_KB: unit=SZ_1K (KB, not pages)
# - Others: unit=PAGE_SIZE (pages, need * PAGE_SIZE)
STATS_NEED_PAGE_CONVERSION = {
    "kernel",      # MEMCG_KMEM
    "vmalloc",     # MEMCG_VMALLOC
    "sock",        # MEMCG_SOCK
    # Note: percpu, zswap, zswapped, slab are already in bytes
    # Note: VM events are counts, not memory sizes, so no conversion needed
}

# TLV format constants
TLV_HEADER_SIZE = 4  # Type (2 bytes) + Length (2 bytes)
TLV_U64_VALUE_SIZE = 8
TLV_ENTRY_SIZE = TLV_HEADER_SIZE + TLV_U64_VALUE_SIZE
TLV_CONTAINER_HEADER_SIZE = 4  # Container type (2 bytes) + Length (2 bytes)

# Container type constants (from tlv.rs)
TLV_TYPE_MEMORY_STATS_CONTAINER = 0xFFFE
TLV_TYPE_NUMA_STATS_CONTAINER = 0xFFFF

# Page size for unit conversion (typically 4096 bytes)
PAGE_SIZE = 4096


def parse_memory_stats(data):
    """
    Parse TLV-encoded memory statistics from binary data.
    
    Args:
        data: Binary TLV data (bytes)
        
    Returns:
        Dictionary mapping statistic names to values
    """
    if len(data) < TLV_CONTAINER_HEADER_SIZE:
        return {}
    
    stats = {}
    pos = 0
    
    # Parse container header
    container_type, container_len = struct.unpack('>HH', data[pos:pos+TLV_CONTAINER_HEADER_SIZE])
    pos += TLV_CONTAINER_HEADER_SIZE
    
    # Validate container type
    if container_type not in (TLV_TYPE_MEMORY_STATS_CONTAINER, TLV_TYPE_NUMA_STATS_CONTAINER):
        # If not a recognized container, try parsing from the beginning
        pos = 0
    
    # Parse TLV entries
    while pos + TLV_ENTRY_SIZE <= len(data):
        # Parse TLV header (big-endian)
        tlv_type, length = struct.unpack('>HH', data[pos:pos+TLV_HEADER_SIZE])
        pos += TLV_HEADER_SIZE
        
        # Validate length
        if length != TLV_U64_VALUE_SIZE:
            # Skip entries with unexpected length
            if pos + length > len(data):
                break
            pos += length
            continue
        
        # Parse uint64 value (big-endian)
        if pos + TLV_U64_VALUE_SIZE > len(data):
            break
        
        value = struct.unpack('>Q', data[pos:pos+TLV_U64_VALUE_SIZE])[0]
        pos += TLV_U64_VALUE_SIZE
        
        # Map TLV type to statistic name
        stat_name = TLV_TYPES.get(tlv_type, f"unknown_{tlv_type}")

        # Convert units: some stats are in pages, need to convert to bytes
        # Based on memcg_page_state_unit() in kernel
        if stat_name in STATS_NEED_PAGE_CONVERSION:
            # Convert pages to bytes
            value = value * PAGE_SIZE

        stats[stat_name] = value

    return stats

def format_value(value):
    """
    Format a statistic value for display.
    
    Args:
        value: Numeric value (typically in bytes or pages)
        
    Returns:
        Formatted string with appropriate units
    """
    if value == 0:
        return "0"
    
    # For large values, show in human-readable format
    for unit, factor in [("GB", 1024**3), ("MB", 1024**2), ("KB", 1024)]:
        if value >= factor:
            return f"{value / factor:.2f} {unit} ({value:,})"
    
    return f"{value:,}"


def main():
    """Main entry point."""
    # Parse command line arguments
    if len(sys.argv) > 2:
        print("Usage: python3 test_usage.py [tlv_file]", file=sys.stderr)
        print("If no file specified, reads from /sys/kernel/debug/memcg_stats/memory.stat_bin", file=sys.stderr)
        sys.exit(1)
    
    tlv_file = sys.argv[1] if len(sys.argv) > 1 else "/sys/kernel/debug/memcg_stats/memory.stat_bin"
    
    # Check if file exists
    if not Path(tlv_file).exists():
        print(f"Error: File not found: {tlv_file}", file=sys.stderr)
        sys.exit(1)
    
    # Read TLV data
    try:
        with open(tlv_file, 'rb') as f:
            data = f.read()
    except IOError as e:
        print(f"Error: Failed to read file: {e}", file=sys.stderr)
        sys.exit(1)
    
    if len(data) == 0:
        print("Error: File is empty", file=sys.stderr)
        sys.exit(1)
    
    # Parse statistics
    stats = parse_memory_stats(data)
    
    if not stats:
        print("Warning: No statistics found in file", file=sys.stderr)
        sys.exit(1)
    
    # Print results
    print("Memory Cgroup Statistics:")
    print("=" * 50)
    
    # Group statistics by category for better readability
    memory_stats = []
    vm_events = []
    unknown_stats = []
    
    for name, value in sorted(stats.items()):
        if name.startswith("unknown_"):
            unknown_stats.append((name, value))
        elif name.startswith("pg") or name.startswith("psw") or name.startswith("swp") or \
             name.startswith("zsw") or name.startswith("thp") or name.startswith("numa"):
            vm_events.append((name, value))
        else:
            memory_stats.append((name, value))
    
    # Print memory statistics
    if memory_stats:
        print("\nMemory Usage:")
        for name, value in memory_stats:
            print(f"  {name:20s}: {format_value(value)}")
    
    # Print VM events
    if vm_events:
        print("\nVM Events:")
        for name, value in vm_events:
            print(f"  {name:20s}: {format_value(value)}")
    
    # Print unknown statistics (for debugging)
    if unknown_stats:
        print("\nUnknown Statistics (may indicate kernel version mismatch):")
        for name, value in unknown_stats:
            print(f"  {name:20s}: {format_value(value)}")
    
    print(f"\nTotal statistics: {len(stats)}")


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""
Example userspace code for parsing TLV-encoded memory cgroup statistics
"""

import struct
import sys
from pathlib import Path

# TLV type definitions (should match tlv.rs)
TLV_TYPES = {
    1: "anon",
    2: "file",
    3: "kernel",
    4: "kernel_stack",
    5: "pagetables",
    6: "sec_pagetables",
    7: "percpu",
    8: "sock",
    9: "vmalloc",
    10: "shmem",
    11: "zswap",
    12: "zswapped",
    13: "file_mapped",
    14: "file_dirty",
    15: "file_writeback",
    16: "swapcached",
    17: "anon_thp",
    18: "file_thp",
    19: "shmem_thp",
    20: "inactive_anon",
    21: "active_anon",
    22: "inactive_file",
    23: "active_file",
    24: "unevictable",
    25: "slab_reclaimable",
    26: "slab_unreclaimable",
    27: "hugetlb",
    28: "slab",
    100: "workingset_refault_anon",
    101: "workingset_refault_file",
    102: "workingset_activate_anon",
    103: "workingset_activate_file",
    104: "workingset_restore_anon",
    105: "workingset_restore_file",
    106: "workingset_nodereclaim",
    200: "pgscan",
    201: "pgsteal",
    202: "pswpin",
    203: "pswpout",
    204: "pgfault",
    205: "pgmajfault",
    206: "pgrefill",
    207: "pgactivate",
    208: "pgdeactivate",
    209: "pglazyfree",
}

def parse_memory_stats(data):
    """Parse TLV-encoded memory statistics"""
    stats = {}
    pos = 0

    # Skip container header (first TLV entry)
    if len(data) >= 4:
        container_type, container_len = struct.unpack('<HH', data[0:4])
        pos = 4 + container_len

    while pos < len(data):
        if pos + 4 > len(data):
            break

        tlv_type, length = struct.unpack('<HH', data[pos:pos+4])

        if pos + 4 + length > len(data):
            break

        if length == 8:  # uint64 value
            value = struct.unpack('<Q', data[pos+4:pos+4+length])[0]
        else:
            value = data[pos+4:pos+4+length]

        stat_name = TLV_TYPES.get(tlv_type, f"unknown_{tlv_type}")
        stats[stat_name] = value

        pos += 4 + length

    return stats

def main():
    if len(sys.argv) == 1:
        # Default path for testing
        tlv_file = "/sys/kernel/debug/memcg_stats/memory.stat_bin"
    else:
        tlv_file = sys.argv[1]

    if not Path(tlv_file).exists():
        print(f"File not found: {tlv_file}")
        print("Usage: python test_usage.py [tlv_file]")
        print("If no file specified, reads from /sys/kernel/debug/memcg_stats/memory.stat_bin")
        sys.exit(1)

    # Read TLV data
    with open(tlv_file, 'rb') as f:
        data = f.read()

    # Parse statistics
    stats = parse_memory_stats(data)

    # Print results
    print("Memory Cgroup Statistics:")
    print("=" * 40)

    for name, value in sorted(stats.items()):
        if isinstance(value, int):
            print("25")
        else:
            print("25")

if __name__ == '__main__':
    main()

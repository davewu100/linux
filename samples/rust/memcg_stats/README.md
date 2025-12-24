# Memory Cgroup Statistics TLV Module

This module provides a new way for userspace applications to access memory cgroup statistics using TLV (Type-Length-Value) encoding with serde JSON serialization support.

## Overview

The traditional memory cgroup statistics are exposed through text files (`memory.stat`, `memory.numa_stat`) which require parsing. This module provides:

1. **TLV-encoded binary interface** - Efficient, structured data access
2. **Serde JSON serialization** - Type-safe data structures for userspace
3. **Rust-based parsing** - Compile-time safety and automatic JSON output

## Integration

The TLV encoding functions have been integrated directly into the kernel's memory cgroup subsystem. Each memory cgroup now has corresponding binary files:

- `memory.stat_bin` - Binary version of `memory.stat`
- `memory.numa_stat_bin` - Binary version of `memory.numa_stat`

These files appear automatically in all memory cgroup directories, including subdirectories.

## Files

- `tlv.rs` - TLV encoding/decoding implementation
- `tlv_to_json.rs` - Main converter tool with serde JSON serialization
- `Cargo.toml` - Rust project configuration with serde dependencies
- `build_and_test.sh` - Build and test automation script

## Kernel Module

The module creates a debugfs file for accessing binary memory statistics:

- `/sys/kernel/debug/memcg_stats/memory.stat_bin` - Memory statistics in TLV format

### Usage

After building the kernel with the integrated TLV support:

```bash
# Each memory cgroup automatically has binary files
ls -la /sys/fs/cgroup/
# memory.stat  memory.stat_bin  memory.numa_stat  memory.numa_stat_bin

# Read root cgroup's binary stats
cat /sys/fs/cgroup/memory.stat_bin > root_stats.tlv

# Read specific cgroup's binary stats
cat /sys/fs/cgroup/my_container/memory.stat_bin > container_stats.tlv

# Read nested cgroup's binary stats
cat /sys/fs/cgroup/a/b/c/memory.stat_bin > nested_stats.tlv

# Parse the binary data
python3 test_usage.py root_stats.tlv

# Run integration tests
python3 test_integration.py
```

## TLV Format

Each statistic is encoded as:

```
+--------+--------+--------+--------+
|  Type  | Length |   Value          |
+--------+--------+--------+--------+
   u16      u16      [Length] bytes
```

### Type IDs

- `1-28`: Memory statistics (anon, file, kernel, etc.)
- `100-106`: Workingset events
- `200-209`: VM events (pgscan, pgsteal, etc.)
- `1000+`: NUMA-specific statistics


## Integration

To integrate into the kernel build system:

1. Add to `samples/rust/Makefile`
2. Add Kconfig entries
3. Include in kernel configuration

## Implementation Details

### Modified Files

1. **`mm/memcontrol.c`**:
   - Added `memory.stat_bin` and `memory.numa_stat_bin` to `memory_files[]` array
   - Added `memory_stat_bin_show()` and `memory_numa_stat_bin_show()` functions
   - Added pure C TLV encoding functions: `encode_memory_stats_tlv()` and `encode_memory_numa_stats_tlv()`
   - Used fixed TLV constants instead of runtime queries

2. **`samples/rust/memcg_stats/`**:
   - Now only provides TLV library infrastructure (`tlv.rs`)
   - All encoding logic moved to C for better performance
   - Removed all exported functions

3. **Supporting files**:
   - `tlv.rs`: TLV data structure definitions and parsing utilities
   - `test_usage.py`: Example usage script
   - `test_integration.py`: Integration test script

### How It Works

1. **File Registration**: Binary files are registered in the memory cgroup's file table
2. **On Access**: When `memory.stat_bin` is read, `memory_stat_bin_show()` is called
3. **TLV Encoding**: The function calls the Rust `rust_encode_memory_stats_tlv()` function
4. **Binary Output**: TLV-encoded data is written directly to the file

### Automatic Propagation

Since the files are registered in the cgroup file table, they automatically appear in:
- `/sys/fs/cgroup/memory/memory.stat_bin` (root cgroup)
- `/sys/fs/cgroup/memory/container1/memory.stat_bin` (child cgroups)
- `/sys/fs/cgroup/memory/a/b/c/memory.stat_bin` (nested cgroups)

## Performance Considerations

**Binary vs Text Format Analysis:**

| Scenario | Binary Format | Text Format | Winner |
|----------|---------------|-------------|--------|
| **High-frequency monitoring** | TLV decode + Rust overhead | Direct text consumption | Text |
| **Low-frequency monitoring** | Protobuf validation | String parsing | Binary |
| **Data transfer size** | 540 bytes (45 stats × 12B) | ~2-3KB text | Binary |
| **Type safety** | Protobuf enforced | Manual parsing | Binary |
| **Extensibility** | Schema evolution | Text format changes | Binary |

**Recommendation**: Use text format for performance-critical monitoring, binary format for type safety and extensibility.

### Performance Implementation

**Final Architecture**: Pure C implementation with zero Rust runtime overhead
- ✅ **No FFI calls**: All encoding in C
- ✅ **Fixed constants**: Compile-time buffer sizing
- ✅ **Optimized memory**: Per-CPU buffers for memory stats, pooled allocation for NUMA stats
- ✅ **Minimal overhead**: Direct buffer writes, no intermediate allocations

## Benefits

- **Type Safety**: Rust structs with serde provide compile-time parsing safety
- **Maintainability**: Pure Rust implementation with clear type definitions
- **Extensibility**: Easy to add new statistics without breaking compatibility
- **Automatic**: No manual setup required - files appear automatically in all cgroups
- **Backwards Compatible**: Existing `memory.stat` interface remains unchanged
- **Compact**: Binary format reduces data transfer size

## Serde JSON Serialization Support

The TLV v1 format is fully compatible with user-space serde JSON serialization. You can parse TLV binary data and convert it to JSON using the provided Rust example.

### Building and Running the TLV to JSON Converter

```bash
# Build the converter
cargo build --release

# Run with TLV binary data
./target/release/tlv_to_json /sys/fs/cgroup/memory/memory.stat_bin

# Example output:
{
  "anon": 1024000,
  "file": 2048000,
  "kernel": 512000,
  "kernel_stack": 256000,
  "pagetables": 128000,
  "percpu": 64000,
  "sock": 32000,
  "vmalloc": 16000,
  "shmem": 8000,
  "zswap": 4000,
  "zswapped": 2000,
  "pgscan_kswapd": 1500,
  "pgsteal_kswapd": 1200,
  // ... more fields
}
```

### Rust API for Serde Integration

```rust
use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize)]
struct MemoryStats {
    anon: u64,
    file: u64,
    // ... all memory stat fields
}

fn parse_tlv_to_json(tlv_data: &[u8]) -> Result<String, Box<dyn std::error::Error>> {
    let stats = parse_tlv_to_memory_stats(tlv_data)?;
    let json = serde_json::to_string_pretty(&stats)?;
    Ok(json)
}
```

### Python Alternative (without serde)

```python
import struct

def parse_tlv_to_dict(data):
    stats = {}
    pos = 0

    # TLV type to field name mapping
    type_map = {
        1: 'anon', 2: 'file', 3: 'kernel', 4: 'kernel_stack',
        200: 'pgscan_kswapd', 201: 'pgscan_direct',
        # ... add more mappings
    }

    while pos + 12 <= len(data):  # TLV entry is 12 bytes
        tlv_type, length = struct.unpack('>HH', data[pos:pos+4])
        value = struct.unpack('>Q', data[pos+4:pos+12])[0]

        if tlv_type in type_map:
            stats[type_map[tlv_type]] = value

        pos += 12

    return stats

# Read and parse TLV data
with open('/sys/fs/cgroup/memory/memory.stat_bin', 'rb') as f:
    data = f.read()
    stats_dict = parse_tlv_to_dict(data)
    print(stats_dict)
```

### Format Compatibility

**TLV v1 is fully compatible with serde JSON serialization:**

✅ **Fixed binary format** - No need to modify TLV v1
✅ **Type-safe parsing** - Rust structs provide compile-time safety
✅ **Automatic JSON output** - serde handles serialization
✅ **Zero-copy parsing** - Direct binary-to-struct conversion
✅ **Extensible** - Easy to add new fields without breaking compatibility

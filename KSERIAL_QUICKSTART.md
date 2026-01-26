# k-serial Quick Start Guide

## What is k-serial?

k-serial lets you query specific fields from kernel structures (like `struct cgroup`) without hardcoded offsets. It uses BTF (BPF Type Format) to find fields by name at runtime.

**Think of it as**: SQL for kernel structs, or a reflection API for C structures.

## Quick Example

Instead of:
```c
// Brittle: offset breaks across kernel versions
int level = *(int *)((char *)cgrp + 0x48);
```

You write:
```bash
./test_kserial level nr_descendants
```

Output:
```
level           = 2
nr_descendants  = 5
```

## 5-Minute Setup

### 1. Build Kernel Module (Optional for Testing)

The MVP includes kernel code that would be compiled into the kernel:

```bash
# These would normally be built as part of the kernel
# For now, we demonstrate the userspace interface

# Kernel files created:
ls -l include/linux/kserial.h
ls -l kernel/kserial.c
ls -l kernel/kserial_procfs.c
```

### 2. Build Userspace Tools

```bash
cd tools/testing/selftests/cgroup

# Build the demo (simulated data)
gcc -o test_kserial test_kserial.c
./test_kserial level nr_descendants

# Build the real version (needs kernel module)
gcc -o test_kserial_real test_kserial_real.c
```

### 3. Test (When Kernel Module Loaded)

```bash
# Query single field
./test_kserial_real level

# Query multiple fields
./test_kserial_real level nr_descendants max_depth

# See available fields
./test_kserial_real --help
```

## Available Fields (Whitelist)

Currently supported fields from `struct cgroup`:

| Field Name | Type | Description |
|------------|------|-------------|
| `level` | int | Depth in cgroup hierarchy (0 = root) |
| `max_depth` | int | Maximum allowed depth |
| `nr_descendants` | int | Number of child cgroups |
| `nr_dying_descendants` | int | Number of dying children |
| `max_descendants` | int | Maximum children allowed |

## How It Works (Under the Hood)

```
You write:     ["level", "nr_descendants"]
                ↓
BTF finds:     offset=0x48, type=int
               offset=0x60, type=int
                ↓
Kernel reads:  cgrp->level = 2
               cgrp->nr_descendants = 5
                ↓
You get:       TLV binary: [0][4][0x02][1][4][0x05]
                ↓
Tool parses:   level = 2
               nr_descendants = 5
```

## API Overview

### C API

```c
#include <linux/kserial.h>

// 1. Create schema
struct ks_schema schema = {
    .nr_fields = 2,
    .field_names = {"level", "nr_descendants"}
};

// 2. Open interface
int fd = open("/proc/kserial", O_RDWR);

// 3. Send query
write(fd, &schema, sizeof(schema));

// 4. Read result  
struct ks_result result;
read(fd, &result, sizeof(result));

// 5. Parse TLV format
uint32_t offset = 0;
while (offset < result.total_len) {
    struct ks_tlv *tlv = (void *)(result.data + offset);
    printf("%s = %llu\n", 
           schema.field_names[tlv->field_id],
           *(uint64_t *)tlv->data);
    offset += sizeof(*tlv) + tlv->len;
}

close(fd);
```

### Kernel API (For Kernel Developers)

```c
#include <linux/kserial.h>

struct cgroup *cgrp = task_dfl_cgroup(current);
struct ks_schema schema = {...};
struct ks_result result;

int ret = ks_query_cgroup(cgrp, &schema, &result);
if (ret == 0) {
    // result.data contains TLV-encoded values
}
```

## Common Use Cases

### 1. Monitoring Tools

```bash
# Check cgroup depth distribution
for pid in $(pgrep -x app); do
    echo -n "PID $pid: "
    ./test_kserial_real level
done
```

### 2. Debugging

```bash
# Quick cgroup stats
./test_kserial_real level nr_descendants nr_dying_descendants
```

### 3. Custom Exporters (Prometheus, etc.)

```c
// metrics_exporter.c
void collect_cgroup_metrics() {
    struct ks_schema schema = {
        .nr_fields = 2,
        .field_names = {"level", "nr_descendants"}
    };
    
    query_via_kserial(&schema);
    export_to_prometheus();
}
```

## Error Handling

```bash
# Field not in whitelist
$ ./test_kserial_real invalid_field
ERROR: Query failed (Permission denied)

# Too many fields
$ ./test_kserial_real f1 f2 ... f17
ERROR: Too many fields (max 16)

# Kernel module not loaded
$ ./test_kserial_real level
ERROR: Cannot open /proc/kserial
Make sure the k-serial kernel module is loaded.
```

## Comparison with Alternatives

### vs. Direct Memory Access
```c
// ❌ Bad: Breaks across kernel versions
int level = *(int *)((char *)cgrp + 0x48);

// ✅ Good: Version-safe
query_field(cgrp, "level", &level);
```

### vs. sysfs/procfs per-field files
```bash
# ❌ Bad: Many files, many open/read/close syscalls
cat /sys/fs/cgroup/.../level
cat /sys/fs/cgroup/.../nr_descendants
cat /sys/fs/cgroup/.../max_depth

# ✅ Good: One query, all fields
./test_kserial_real level nr_descendants max_depth
```

### vs. BPF
```c
// ❌ Complex: BPF program, maps, verifier
bpf_program = load_bpf("get_cgroup_stats.o");
attach_to_tracepoint(...);
read_from_bpf_map(...);

// ✅ Simple: Direct query
ks_query_cgroup(cgrp, &schema, &result);
```

## Performance

Typical latencies (estimated):

| Operation | Time |
|-----------|------|
| First query (BTF lookup) | ~2 μs |
| Subsequent queries (cached) | ~500 ns |
| Per-field extraction | ~100 ns |
| TLV encoding | ~50 ns |

**Total for 3 fields**: ~2-3 μs (first time), ~650 ns (cached)

## Limitations (MVP)

Current limitations:

- ❌ Only `struct cgroup` supported
- ❌ Only scalar integer fields (u8/u16/u32/u64/int)
- ❌ No nested structs (e.g., can't query `cgrp->css_set->id`)
- ❌ No arrays (e.g., can't query `cgrp->node_stat[3]`)
- ❌ No pointer dereferencing
- ❌ Read-only (no field modification)

These are **design choices** for the MVP to keep it simple. Future versions can add these features.

## FAQ

### Q: Why not just use BPF?

A: BPF is powerful but complex. k-serial is simpler for the common case of "just read some fields".

### Q: Is this safe?

A: Yes. Fields are whitelisted, types are validated, and no pointers are exposed.

### Q: What's the overhead?

A: ~2-3 μs for first query (BTF lookup), ~650 ns for cached queries. Negligible for most use cases.

### Q: Can I query any kernel struct?

A: MVP only supports `struct cgroup`. The design extends to other structs, but requires whitelist updates.

### Q: What if field name changes?

A: Query returns error. This is intentional - forces explicit API versioning.

### Q: Can I query from another process's cgroup?

A: MVP only queries current process's cgroup. Future versions could add PID parameter.

## Next Steps

1. **Read the design doc**: `KSERIAL_DESIGN.md` for architecture details
2. **Try the examples**: Build and run `test_kserial_real`
3. **Extend the whitelist**: Add more cgroup fields as needed
4. **Integrate**: Use in your monitoring/debugging tools

## Getting Help

- Design document: `KSERIAL_DESIGN.md`
- Source code: `kernel/kserial.c`, `include/linux/kserial.h`
- Examples: `tools/testing/selftests/cgroup/test_kserial*.c`

---

**TL;DR**: Query kernel struct fields by name using BTF. No hardcoded offsets. Type-safe. Version-resilient.

```bash
./test_kserial_real level nr_descendants  # That's it!
```

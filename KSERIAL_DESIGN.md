# k-serial: BTF-Based Dynamic Field Subscription for Kernel Structs

## Overview

k-serial is a lightweight mechanism for userspace programs to dynamically subscribe to specific fields in kernel structures without hardcoded offsets. It uses BTF (BPF Type Format) for runtime field discovery and provides a safe, versioned interface.

## The Problem

Traditional approaches for exposing kernel data to userspace have limitations:

1. **Hardcoded offsets**: Brittle, breaks across kernel versions
2. **Full struct dumps**: Exposes unnecessary data, privacy/security concerns  
3. **Custom interfaces**: Require new code for each struct/field
4. **ioctl/sysfs**: Proliferation of interfaces, maintenance burden

## The k-serial Solution

**Core Idea**: User declares fields by name → Kernel uses BTF to resolve → Returns structured data

### Key Principles (MVP)

✅ **What we DO**:
- Query fields by name (not offset)
- Use BTF for runtime type introspection
- Whitelist-based security model
- TLV (Type-Length-Value) output format
- Support for `struct cgroup` only (MVP)

🚫 **What we DON'T do** (for MVP):
- Arbitrary kernel structs
- Pointer dereferencing / nested structs
- Bitfields
- Arrays (treated as scalars only)
- Write access (read-only)

## Architecture

```
┌─────────────────┐
│   User Space    │
│                 │
│  1. Build schema│
│     ["level",   │
│      "nr_desc"] │
└────────┬────────┘
         │ write(schema)
         ▼
┌─────────────────────────────────┐
│      /proc/kserial         │
└────────┬────────────────────────┘
         │
         ▼
┌─────────────────────────────────┐
│  Kernel: ks_query_cgroup()      │
│                                 │
│  2. Validate field names        │
│     (whitelist check)           │
│                                 │
│  3. BTF lookup                  │
│     - Find "struct cgroup"      │
│     - Find field by name        │
│     - Get type & offset         │
│                                 │
│  4. Extract value               │
│     - Calculate address         │
│     - Read value                │
│     - Encode as TLV             │
└────────┬────────────────────────┘
         │
         ▼ read(result)
┌─────────────────┐
│   User Space    │
│                 │
│  5. Parse TLV   │
│     Display     │
└─────────────────┘
```

## Data Structures

### User Space Schema (Input)

```c
#define KS_MAX_FIELDS 16
#define KS_FIELD_NAME_LEN 32

struct ks_schema {
    uint32_t nr_fields;
    char field_names[KS_MAX_FIELDS][KS_FIELD_NAME_LEN];
};
```

**Example**:
```c
struct ks_schema schema = {
    .nr_fields = 3,
    .field_names = {"level", "nr_descendants", "max_depth"}
};
```

### TLV Output Format

```c
struct ks_tlv {
    uint16_t field_id;   // Index in schema (0, 1, 2, ...)
    uint16_t len;        // Length of data in bytes
    uint8_t  data[];     // Field value
} __attribute__((packed));
```

**Binary Layout Example**:
```
┌────────┬────────┬──────────────┐
│ fid=0  │ len=4  │ value=2      │  <- level = 2
├────────┼────────┼──────────────┤
│ fid=1  │ len=4  │ value=5      │  <- nr_descendants = 5
├────────┼────────┼──────────────┤
│ fid=2  │ len=4  │ value=10     │  <- max_depth = 10
└────────┴────────┴──────────────┘
```

### Result Buffer (Output)

```c
#define KS_MAX_OUTPUT_SIZE 4096

struct ks_result {
    uint32_t total_len;  // Total bytes in data[]
    uint8_t  data[KS_MAX_OUTPUT_SIZE];  // TLV entries
};
```

## BTF-Based Field Resolution

### Step-by-step Process

1. **Get BTF Handle**:
   ```c
   const struct btf *btf = btf_get_module_btf(NULL); // vmlinux
   ```

2. **Find struct cgroup**:
   ```c
   s32 type_id = btf_find_by_name_kind(btf, "cgroup", BTF_KIND_STRUCT);
   ```

3. **Iterate members to find field**:
   ```c
   const struct btf_type *t = btf_type_by_id(btf, type_id);
   for (i = 0; i < btf_vlen(t); i++) {
       const struct btf_member *m = btf_members(t) + i;
       const char *name = btf_name_by_offset(btf, m->name_off);
       if (!strcmp(name, "level")) {
           offset = m->offset;  // bit offset
           type = m->type;      // BTF type ID
           break;
       }
   }
   ```

4. **Calculate address and read**:
   ```c
   void *field_addr = (void *)cgrp + (offset / 8);
   memcpy(&value, field_addr, size);
   ```

## Security Model

### Whitelist-Based Access Control

Only pre-approved fields can be queried:

```c
static const char *ks_cgroup_whitelist[] = {
    "level",
    "max_depth",
    "nr_descendants",
    "nr_dying_descendants",
    "max_descendants",
    NULL
};
```

**Why whitelist?**
- Prevents information leakage
- Explicit about stable ABI
- Easy to audit
- Fails closed by default

### Type Safety

- Only scalar integer types allowed (MVP)
- Pointers rejected (could expose kernel addresses)
- Arrays rejected (complex semantics)
- Bitfields rejected (alignment issues)

```c
/* Type validation */
if (!btf_type_is_int(field_type))
    return -EINVAL;  // Reject non-integer types
```

## Usage Example

### Kernel Module

```bash
# Load k-serial module
insmod kernel/kserial.ko
insmod kernel/kserial_procfs.ko

# Verify interface exists
ls -l /proc/kserial
```

### Userspace Program

```c
#include "kserial.h"

int main() {
    struct ks_schema schema = {
        .nr_fields = 2,
        .field_names = {"level", "nr_descendants"}
    };
    struct ks_result result;
    
    int fd = open("/proc/kserial", O_RDWR);
    
    // Send query
    write(fd, &schema, sizeof(schema));
    
    // Get result
    read(fd, &result, sizeof(result));
    
    // Parse TLV
    parse_tlv_result(&result, &schema);
    
    close(fd);
    return 0;
}
```

### Command Line Tool

```bash
# Query single field
./test_kserial_real level

# Query multiple fields
./test_kserial_real level nr_descendants max_depth
```

**Output**:
```
Querying 3 field(s) from current cgroup:
  [0] level
  [1] nr_descendants
  [2] max_depth

=== Query Results ===
Total data: 12 bytes

  level                     = 2
  nr_descendants            = 5
  max_depth                 = 10
```

## Implementation Files

```
linux/
├── include/linux/kserial.h           # UAPI + kernel API
├── kernel/kserial.c                  # Core BTF-based query logic
├── kernel/kserial_procfs.c           # Procfs interface
└── tools/testing/selftests/cgroup/
    ├── test_kserial.c                # Demo userspace client
    └── test_kserial_real.c           # Real procfs client
```

## Advantages Over Alternatives

| Approach | Pros | Cons |
|----------|------|------|
| **k-serial** | ✅ Dynamic field selection<br>✅ Version-safe via BTF<br>✅ No offset hardcoding<br>✅ Minimal kernel changes | ⚠️ Requires BTF<br>⚠️ Runtime overhead |
| **Direct struct access** | Fast | ❌ Offsets break across versions<br>❌ Exposes all fields |
| **sysfs** | Simple | ❌ One file per field<br>❌ String parsing overhead |
| **BPF** | Flexible | ❌ Complex setup<br>❌ Security restrictions |

## Future Extensions (Beyond MVP)

### 1. Nested Struct Support
```c
// Query: "css_set->dfl_cgrp->level"
schema.field_paths[0] = "css_set.dfl_cgrp.level";
```

### 2. Array Indexing
```c
// Query: "node_stat[2]"  
schema.field_names[0] = "node_stat[2]";
```

### 3. Generic Struct Support
```c
// Not just cgroup
ks_query_struct("task_struct", current, &schema, &result);
```

### 4. Filtering & Aggregation
```c
// Sum across all cgroups
schema.operation = KS_OP_SUM;
schema.filter = "level == 2";
```

### 5. Notification on Change
```c
// Get notified when field changes
ks_watch_field(cgrp, "nr_descendants", callback);
```

## Performance Considerations

### MVP Profile (Expected)

- **BTF lookup**: ~1-2 μs (cached after first lookup)
- **Field extraction**: ~100 ns per field
- **TLV encoding**: ~50 ns per field
- **Total for 3 fields**: ~2-3 μs

### Optimization Opportunities

1. **BTF caching**: Cache type_id and member offsets
2. **Batch queries**: Amortize BTF lookup cost
3. **Zero-copy**: Use shared memory for large results
4. **JIT**: Generate field accessors at runtime

## Testing

```bash
# Build userspace tools
cd tools/testing/selftests/cgroup
make test_kserial_real

# Test with different field combinations
./test_kserial_real level
./test_kserial_real level nr_descendants
./test_kserial_real max_depth max_descendants

# Test error cases
./test_kserial_real invalid_field  # Should reject
./test_kserial_real level level level level level level level level level \
                    level level level level level level level level  # Too many
```

## Comparison to Protobuf/IDL

k-serial is similar to protobuf in spirit:

| Aspect | Protobuf | k-serial |
|--------|----------|----------|
| Schema definition | `.proto` files | BTF (from C source) |
| Field identification | Field numbers | Field names + BTF |
| Wire format | Varint encoding | TLV binary |
| Versioning | Add/deprecated fields | Whitelist evolution |
| Language | Language-agnostic | C-specific |

**Key difference**: k-serial uses kernel's existing BTF infrastructure instead of separate IDL compiler.

## Conclusion

k-serial provides a clean, type-safe, version-resilient way for userspace to query kernel structure fields. The MVP focuses on proving the concept with `struct cgroup`, but the design naturally extends to other structs and more complex queries.

**Next Steps**:
1. ✅ Implement MVP (done)
2. Test with real workloads
3. Benchmark performance
4. Gather feedback on API ergonomics
5. Plan v2 features based on usage

.. _kserial_api:

==================
k-serial Kernel API
==================

:Date: January, 2026
:Author: Jianyue Wu <wujianyue000@gmail.com>

Overview
========

This document describes the kernel API for k-serial, a BTF-based dynamic
field subscription mechanism. This is intended for kernel developers who
want to understand or extend k-serial.

Architecture
============

k-serial consists of three main components:

1. **UAPI Layer** (``include/linux/kserial.h``): Shared structures between
   kernel and userspace
2. **Core Engine** (``kernel/kserial.c``): BTF-based field resolution
3. **Interface** (``kernel/kserial_procfs.c``): procfs interface

Data Flow
---------

::

    Userspace                 Kernel
    =========                 ======
    
    ks_schema                 /proc/cgroup_query
        |                            |
        | write()                    v
        |                     ks_proc_write()
        |                            |
        |                            v
        |                     ks_query_cgroup()
        |                            |
        |                            +-> ks_validate_field()
        |                            +-> ks_resolve_field_path()
        |                            |      |
        |                            |      +-> ks_parse_field_path()
        |                            |      +-> ks_find_field_by_name()
        |                            |      +-> BTF traversal
        |                            |
        |                            +-> ks_write_tlv()
        |                            |
        v                            v
    ks_result  <--- read() --- TLV-encoded data

Core API Functions
==================

ks_query_cgroup()
-----------------

Main query function::

    int ks_query_cgroup(struct cgroup *cgrp,
                        const struct ks_schema *schema,
                        struct ks_result *result);

**Parameters:**

- ``cgrp``: Target cgroup to query
- ``schema``: Input schema specifying fields to query
- ``result``: Output buffer for TLV-encoded results

**Returns:**

- ``0`` on success
- ``-EINVAL``: Invalid parameters
- ``-EPERM``: Field not in whitelist
- ``-ENOENT``: Field not found in struct
- ``-EFAULT``: NULL pointer encountered
- ``-ENOSPC``: Output buffer full

**Example:**

::

    struct cgroup *cgrp = task_dfl_cgroup(current);
    struct ks_schema schema = {
        .nr_fields = 2,
        .flags = 0,
        .field_names = {"level", "nr_descendants"}
    };
    struct ks_result result;
    
    int ret = ks_query_cgroup(cgrp, &schema, &result);
    if (ret)
        return ret;
    
    /* result now contains TLV-encoded values */

BTF Helper Functions
====================

ks_find_field_by_name()
------------------------

Find a struct member by name using BTF::

    static int ks_find_field_by_name(const struct btf *btf,
                                      s32 type_id,
                                      const char *field_name,
                                      const struct btf_member **member);

**Parameters:**

- ``btf``: BTF object (usually btf_vmlinux)
- ``type_id``: BTF type ID of the struct
- ``field_name``: Name of field to find
- ``member``: Output pointer to btf_member

**Returns:**

- ``0`` on success
- ``-EINVAL``: Invalid struct type
- ``-ENOENT``: Field not found

ks_parse_field_path()
----------------------

Parse field path into components::

    static int ks_parse_field_path(const char *path,
                                    char components[][32],
                                    int max_depth);

**Parameters:**

- ``path``: Field path (e.g., "self.id" or "dom_cgrp.level")
- ``components``: Output array for path components
- ``max_depth``: Maximum number of components (usually KS_MAX_PATH_DEPTH)

**Returns:**

- Number of components on success
- ``-ENAMETOOLONG``: Component name too long
- ``-E2BIG``: Path too deep

**Example:**

::

    char components[4][32];
    int depth = ks_parse_field_path("dom_cgrp.level", components, 4);
    /* depth = 2 */
    /* components[0] = "dom_cgrp" */
    /* components[1] = "level" */

ks_resolve_field_path()
------------------------

Resolve nested field path using BTF::

    static int ks_resolve_field_path(const struct btf *btf,
                                      s32 start_type_id,
                                      void *base_addr,
                                      const char *path,
                                      u32 flags,
                                      void **final_addr,
                                      u32 *final_type_id);

**Parameters:**

- ``btf``: BTF object
- ``start_type_id``: Starting struct type ID
- ``base_addr``: Base address of starting struct
- ``path``: Field path to resolve
- ``flags``: Schema flags (e.g., KS_FLAG_ALLOW_NULL)
- ``final_addr``: Output pointer to final field address
- ``final_type_id``: Output pointer to final field type ID

**Returns:**

- ``0`` on success
- ``-ENOENT``: Field not found in path
- ``-EINVAL``: Invalid type in path
- ``-EFAULT``: NULL pointer encountered (without KS_FLAG_ALLOW_NULL)
- ``-E2BIG``: Path too deep

**Algorithm:**

1. Parse path into components
2. For each component:
   
   a. Find field in current struct
   b. If pointer type: dereference it
   c. If struct type: continue with embedded struct
   d. If NULL pointer: return error or 0 (based on flags)

3. Return final field address and type

ks_get_field_size()
-------------------

Get size of a BTF type::

    static int ks_get_field_size(const struct btf *btf, u32 type_id);

**Parameters:**

- ``btf``: BTF object
- ``type_id``: BTF type ID

**Returns:**

- Size in bytes on success
- ``-EINVAL``: Unsupported type

**Supported Types:**

- Integer types (u8, u16, u32, u64, int, long, etc.)
- Pointer types (size = sizeof(void *))

**Unsupported Types:**

- Arrays
- Unions
- Function pointers
- Bitfields

Security and Validation
========================

Whitelist Validation
--------------------

All field paths must be validated against the whitelist::

    static inline bool ks_validate_field(const char *field_name)
    {
        int i;
        for (i = 0; ks_cgroup_whitelist[i]; i++) {
            if (!strcmp(field_name, ks_cgroup_whitelist[i]))
                return true;
        }
        return false;
    }

**Whitelist Definition:**

::

    static const char *ks_cgroup_whitelist[] = {
        /* Simple fields */
        "level",
        "max_depth",
        "nr_descendants",
        "nr_dying_descendants",
        "max_descendants",
        
        /* Nested fields */
        "self.id",
        "self.serial_nr",
        "dom_cgrp.level",
        "root.kf",
        
        NULL
    };

Type Safety
-----------

BTF ensures type safety at multiple levels:

1. **Struct validation**: Ensures struct exists in BTF
2. **Field validation**: Ensures field exists in struct
3. **Type checking**: Validates field types
4. **Pointer validation**: Checks pointer targets
5. **Alignment**: Verifies proper alignment

Adding New Fields
=================

To add support for new fields:

1. **Update whitelist** in ``include/linux/kserial.h``::

    static const char *ks_cgroup_whitelist[] = {
        /* ... existing fields ... */
        "new_field",              /* Add here */
        NULL
    };

2. **Test thoroughly**:

   - Simple field access
   - Nested path access (if applicable)
   - NULL pointer handling (if pointer type)
   - Performance impact

3. **Update documentation**:

   - ``Documentation/admin-guide/kserial.rst``
   - Field description and use case

4. **Security review**:

   - Ensure no sensitive data exposure
   - Verify whitelist validation works
   - Check for information leakage

Example: Adding a Simple Field
-------------------------------

::

    /* 1. Add to whitelist */
    static const char *ks_cgroup_whitelist[] = {
        /* ... */
        "nr_populated_csets",  /* New field */
        NULL
    };
    
    /* 2. Test */
    struct ks_schema schema = {
        .nr_fields = 1,
        .field_names = {"nr_populated_csets"}
    };
    /* Query and verify result */

Example: Adding a Nested Field
-------------------------------

::

    /* 1. Add to whitelist */
    static const char *ks_cgroup_whitelist[] = {
        /* ... */
        "parent.level",  /* New nested field */
        NULL
    };
    
    /* 2. Test with NULL handling */
    struct ks_schema schema = {
        .nr_fields = 1,
        .flags = KS_FLAG_ALLOW_NULL,  /* parent can be NULL */
        .field_names = {"parent.level"}
    };

Extending to New Structs
=========================

To support querying other kernel structs (e.g., task_struct):

1. **Create new query function**::

    int ks_query_task(struct task_struct *task,
                      const struct ks_schema *schema,
                      struct ks_result *result);

2. **Define struct-specific whitelist**::

    static const char *ks_task_whitelist[] = {
        "pid",
        "tgid",
        "state",
        /* ... */
        NULL
    };

3. **Add new interface** (e.g., ``/proc/PID/task_query``)

4. **Reuse core functions**:

   - ``ks_resolve_field_path()``
   - ``ks_find_field_by_name()``
   - ``ks_write_tlv()``

TLV Encoding
============

Output Format
-------------

Results are encoded in TLV (Type-Length-Value) format::

    struct ks_tlv {
        __u16 field_id;    /* Index in schema (0, 1, 2, ...) */
        __u16 len;         /* Length of data in bytes */
        __u8  data[];      /* Field value (little-endian) */
    } __attribute__((packed));

**Binary Layout:**

::

    +--------+--------+------------------+
    | fid=0  | len=4  | value (4 bytes)  |
    +--------+--------+------------------+
    | fid=1  | len=8  | value (8 bytes)  |
    +--------+--------+------------------+
    | fid=2  | len=4  | value (4 bytes)  |
    +--------+--------+------------------+

Encoding Function
-----------------

::

    static int ks_write_tlv(struct ks_result *result,
                            u16 field_id,
                            const void *data,
                            u16 len)
    {
        struct ks_tlv *tlv;
        u32 tlv_size = sizeof(struct ks_tlv) + len;
        
        if (result->total_len + tlv_size > KS_MAX_OUTPUT_SIZE)
            return -ENOSPC;
        
        tlv = (struct ks_tlv *)(result->data + result->total_len);
        tlv->field_id = field_id;
        tlv->len = len;
        memcpy(tlv->data, data, len);
        
        result->total_len += tlv_size;
        return 0;
    }

Error Codes
===========

Standard Return Codes
---------------------

========== ================================================
Error Code Meaning
========== ================================================
``0``      Success
``-EINVAL`` Invalid parameters or unsupported type
``-EPERM``  Field not in whitelist (permission denied)
``-ENOENT`` Field not found in struct
``-EFAULT`` NULL pointer encountered without NULL flag
``-ENOSPC`` Output buffer full
``-E2BIG``  Path too deep (> KS_MAX_PATH_DEPTH)
``-ENAMETOOLONG`` Field name component too long
========== ================================================

Error Handling
--------------

::

    int ret = ks_query_cgroup(cgrp, &schema, &result);
    switch (ret) {
    case 0:
        /* Success */
        break;
    case -EPERM:
        pr_warn("Field not whitelisted\n");
        break;
    case -ENOENT:
        pr_warn("Field not found\n");
        break;
    case -EFAULT:
        pr_warn("NULL pointer encountered\n");
        break;
    default:
        pr_err("Query failed: %d\n", ret);
        break;
    }

Performance Considerations
==========================

BTF Lookup Caching
------------------

BTF lookups are relatively expensive (~2 μs). For frequent queries,
consider caching:

- BTF type IDs
- Field offsets
- Member pointers

However, the MVP implementation does not include caching for simplicity.

Batch Queries
-------------

Query multiple fields in one call to amortize BTF lookup overhead::

    /* Good: One query, multiple fields */
    schema.nr_fields = 5;
    /* ~2.5 μs total */
    
    /* Bad: Five separate queries */
    for (i = 0; i < 5; i++) {
        schema.nr_fields = 1;
        ks_query_cgroup(...);
    }
    /* ~10 μs total */

Path Depth Impact
-----------------

Each nesting level adds ~50-100ns overhead:

- Simple: ``level`` → ~100 ns
- One level: ``self.id`` → ~150 ns
- Two levels: ``dom_cgrp.level`` → ~200 ns

Debugging
=========

Debug Prints
------------

k-serial uses ``pr_warn()`` for user-visible errors::

    pr_warn("k-serial: field '%s' not in whitelist\n", field_name);
    pr_warn("k-serial: NULL pointer in path '%s' at '%s'\n",
            path, component);

Enable kernel debug messages::

    # dmesg | grep k-serial

BTF Inspection
--------------

To inspect BTF information::

    # bpftool btf dump file /sys/kernel/btf/vmlinux format c | \
        grep "struct cgroup"

Testing Changes
---------------

After modifying k-serial:

1. **Compile test**::

    make kernel/kserial.o

2. **Unit test**::

    cd tools/testing/selftests/cgroup
    make -f Makefile.kserial test_kserial_phase2
    ./test_kserial_phase2

3. **Integration test**::

    ./test_kserial.sh

Future Work
===========

Planned Extensions
------------------

1. **Phase 3: Array indexing**
   
   - Syntax: ``node_stat[2]``
   - Requires BTF array type handling

2. **Phase 4: Generic structs**
   
   - Support any kernel struct
   - Per-struct whitelists

3. **Phase 5: Filtering and aggregation**
   
   - Filter: ``level == 2``
   - Operations: SUM, AVG, COUNT

4. **Performance optimizations**
   
   - BTF lookup caching
   - Offset caching
   - Zero-copy output

See Also
========

- :ref:`kserial <kserial>`: k-serial admin guide
- ``include/linux/kserial.h``: UAPI header
- ``kernel/kserial.c``: Implementation
- ``kernel/kserial_procfs.c``: procfs interface

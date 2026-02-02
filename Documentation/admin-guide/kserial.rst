.. _kserial:

========
k-serial
========

:Date: January, 2026
:Author: Jianyue Wu <wujianyue000@gmail.com>

Introduction
============

k-serial is a BTF-based dynamic field subscription mechanism that allows
userspace programs to query specific fields from kernel structures without
hardcoded offsets. It uses BTF (BPF Type Format) for runtime field discovery
and provides a safe, version-resilient interface.

The key innovation is using field names instead of memory offsets, making
queries work across different kernel versions as long as the field names
remain stable.

Key Features
------------

- **BTF-based field lookup**: No hardcoded offsets needed
- **Nested field support**: Query paths like ``self.id`` or ``dom_cgrp.level``
- **Whitelist security**: Only explicitly allowed fields are accessible
- **Type safety**: BTF validates types at every step
- **TLV output format**: Compact binary encoding for efficiency
- **Version resilient**: Works across kernel versions

Current Implementation
----------------------

The current implementation (Phase 2) supports:

- struct cgroup queries via ``/dev/kserial``
- Simple fields: ``level``, ``nr_descendants``, etc.
- Nested fields: ``self.id``, ``dom_cgrp.level``, etc.
- Pointer dereferencing with NULL safety
- Up to 4 levels of nesting
- Read-only access

Basic Usage
===========

Interface
---------

k-serial provides a procfs interface at ``/dev/kserial`` that accepts
queries for the current process's cgroup.

Query Process
~~~~~~~~~~~~~

1. **Open** the interface file::

    int fd = open("/dev/kserial", O_RDWR);

2. **Write** a schema specifying which fields to query::

    struct ks_schema schema = {
        .nr_fields = 2,
        .flags = 0,
        .field_names = {"level", "nr_descendants"}
    };
    write(fd, &schema, sizeof(schema));

3. **Read** the TLV-encoded result::

    struct ks_result result;
    read(fd, &result, sizeof(result));

4. **Parse** the result and **close** the file descriptor.

Data Structures
---------------

Schema (Input)
~~~~~~~~~~~~~~

The schema structure specifies which fields to query::

    struct ks_schema {
        uint32_t nr_fields;              /* Number of fields to query */
        uint32_t flags;                  /* Flags (e.g., KS_FLAG_ALLOW_NULL) */
        char field_names[128][64];       /* Field names or paths */
    };

Flags:

- ``KS_FLAG_ALLOW_NULL`` (0x01): Return 0 for NULL pointers instead of error
- ``KS_FLAG_FLUSH`` (0x02): Request flush before read (some paths may already flush)

Result (Output)
~~~~~~~~~~~~~~~

Results are encoded in TLV (Type-Length-Value) format::

    struct ks_result {
        uint32_t total_len;              /* Total bytes in data[] */
        uint8_t  data[4096];             /* TLV entries */
    };

Each TLV entry::

    struct ks_tlv {
        uint16_t field_id;               /* Index in schema */
        uint16_t len;                    /* Length of value */
        uint8_t  data[];                 /* Field value */
    } __attribute__((packed));

Examples
========

Simple Field Query
------------------

Query simple fields from current cgroup::

    #include <fcntl.h>
    #include <unistd.h>
    #include <stdint.h>
    #include <stdio.h>
    
    struct ks_schema {
        uint32_t nr_fields;
        uint32_t flags;
        char field_names[128][64];
    };
    
    struct ks_result {
        uint32_t total_len;
        uint8_t  data[4096];
    };
    
    int main() {
        int fd = open("/dev/kserial", O_RDWR);
        if (fd < 0) {
            perror("open");
            return 1;
        }
        
        struct ks_schema schema = {
            .nr_fields = 2,
            .flags = 0,
            .field_names = {"level", "nr_descendants"}
        };
        
        write(fd, &schema, sizeof(schema));
        
        struct ks_result result;
        read(fd, &result, sizeof(result));
        
        /* Parse TLV result... */
        
        close(fd);
        return 0;
    }

Nested Field Query (Phase 2)
-----------------------------

Query nested fields with pointer dereferencing::

    struct ks_schema schema = {
        .nr_fields = 3,
        .flags = KS_FLAG_ALLOW_NULL,
        .field_names = {
            "level",              /* Simple field */
            "self.id",            /* Embedded struct */
            "dom_cgrp.level"      /* Pointer + nested field */
        }
    };
    
    /* Query process same as above */

This queries:

- ``level``: Direct field from struct cgroup
- ``self.id``: Field from embedded struct (cgrp->self.id)
- ``dom_cgrp.level``: Pointer dereference (cgrp->dom_cgrp->level)

The ``KS_FLAG_ALLOW_NULL`` flag ensures that if ``dom_cgrp`` is NULL,
the query returns 0 instead of failing with an error.

Available Fields
================

Current Whitelist
-----------------

Only whitelisted fields can be queried. The current whitelist includes:

Simple Fields
~~~~~~~~~~~~~

- ``level``: Depth in cgroup hierarchy (0 = root)
- ``max_depth``: Maximum allowed depth
- ``nr_descendants``: Number of descendant cgroups
- ``nr_dying_descendants``: Number of dying descendants
- ``max_descendants``: Maximum descendants allowed

Nested Fields (Phase 2)
~~~~~~~~~~~~~~~~~~~~~~~~

- ``self.id``: CSS ID (cgrp->self.id)
- ``self.serial_nr``: CSS serial number (cgrp->self.serial_nr)
- ``dom_cgrp.level``: Domain cgroup level (cgrp->dom_cgrp->level)
- ``root.kf``: Root kernfs node pointer (cgrp->root->kf)

Adding New Fields
-----------------

To add new fields to the whitelist:

1. Update ``ks_cgroup_whitelist[]`` in ``include/linux/kserial.h``
2. Ensure the field is safe to expose (no sensitive data)
3. Test thoroughly
4. Update documentation

Security Considerations
=======================

Whitelist-Based Access Control
-------------------------------

All field paths must be explicitly whitelisted. This prevents:

- Accidental exposure of sensitive data
- Access to internal/unstable fields
- Information leakage through the type system

Type Validation
---------------

BTF validates every step of field resolution:

- Ensures fields exist in the struct
- Validates field types (only integers and pointers supported)
- Checks pointer validity before dereferencing
- Prevents out-of-bounds access

NULL Pointer Safety
-------------------

By default, encountering a NULL pointer returns ``-EFAULT``. Users can
opt-in to NULL-safe behavior with the ``KS_FLAG_ALLOW_NULL`` flag,
which returns 0 for NULL pointers instead.

Depth Limits
------------

Maximum nesting depth is limited to 4 levels to prevent:

- Stack overflow
- Excessive complexity
- Performance degradation

Performance
===========

Typical Latencies
-----------------

================== ========== ==================
Operation          Latency    Notes
================== ========== ==================
Simple field       ~100 ns    Direct BTF lookup
Nested (embedded)  ~150 ns    +50ns for parsing
Nested (pointer)   ~200 ns    +100ns for deref
3 field query      ~450 ns    Amortized cost
================== ========== ==================

Overhead per nesting level: approximately 50-100ns.

Optimization Tips
-----------------

1. **Batch queries**: Query multiple fields in one call to amortize
   BTF lookup overhead
2. **Cache results**: If querying frequently, cache results in userspace
3. **Use simple paths**: Prefer simple fields when possible

Limitations
===========

Current Limitations
-------------------

- **Read-only**: Cannot modify field values
- **Single struct**: Only struct cgroup supported (Phase 1-2)
- **Integer types**: Only scalar integer types and pointers
- **No arrays**: Array indexing not yet supported (planned for Phase 3)
- **Max depth**: 4 levels of nesting maximum
- **Current process**: Only queries current process's cgroup

Future Extensions
-----------------

Planned features:

- **Phase 3**: Array indexing (``node_stat[2]``)
- **Phase 4**: Generic struct support (any kernel struct)
- **Phase 5**: Filtering and aggregation operations

Troubleshooting
===============

Common Issues
-------------

Interface not available
~~~~~~~~~~~~~~~~~~~~~~~

If ``/dev/kserial`` doesn't exist::

    $ ls /dev/kserial
    ls: cannot access '/dev/kserial': No such file or directory

**Solution**: Ensure k-serial kernel module is loaded. Check kernel
configuration for ``CONFIG_KSERIAL=y``.

Permission denied
~~~~~~~~~~~~~~~~~

::

    $ cat /dev/kserial
    cat: /dev/kserial: Permission denied

**Solution**: The interface requires read/write permissions. Ensure
appropriate permissions or run with sufficient privileges.

Field not found
~~~~~~~~~~~~~~~

If a query fails with "field not in whitelist"::

    k-serial: field 'invalid_field' not in whitelist

**Solution**: Only whitelisted fields can be queried. Check the
available fields list above or add the field to the whitelist.

NULL pointer error
~~~~~~~~~~~~~~~~~~

If a query fails with -EFAULT::

    k-serial: NULL pointer in path 'dom_cgrp.level'

**Solution**: Use ``KS_FLAG_ALLOW_NULL`` flag to handle NULL pointers
gracefully, or ensure the pointer field is valid.

Testing
=======

Test Tools
----------

Several test programs are provided in
``tools/testing/selftests/cgroup/``:

test_kserial
~~~~~~~~~~~~

Demo client with simulated data (no kernel module required)::

    $ cd tools/testing/selftests/cgroup
    $ ./test_kserial level nr_descendants

test_kserial_real
~~~~~~~~~~~~~~~~~

Real client using procfs interface (requires kernel module)::

    $ ./test_kserial_real level nr_descendants

test_kserial_phase2
~~~~~~~~~~~~~~~~~~~

Phase 2 nested field tests::

    $ ./test_kserial_phase2        # Run all tests
    $ ./test_kserial_phase2 1      # Embedded struct test
    $ ./test_kserial_phase2 2      # Pointer deref test

example_monitor
~~~~~~~~~~~~~~~

Continuous monitoring application::

    $ ./example_monitor -m -i 1    # Monitor every 1 second
    $ ./example_monitor -s         # Single snapshot

Python Client
~~~~~~~~~~~~~

Python client library with JSON support::

    $ ./kserial_client.py level nr_descendants --json

Building Tests
--------------

::

    $ cd tools/testing/selftests/cgroup
    $ make -f Makefile.kserial
    $ ./test_kserial.sh            # Run full test suite

API Reference
=============

For detailed kernel API documentation, see:

- ``Documentation/core-api/kserial.rst`` (API reference)
- ``include/linux/kserial.h`` (header file)
- ``kernel/kserial.c`` (implementation)

Additional Documentation
========================

More detailed documentation:

- ``KSERIAL_README.md``: Quick reference and overview
- ``KSERIAL_QUICKSTART.md``: 5-minute tutorial
- ``KSERIAL_DESIGN.md``: Architecture and design details
- ``KSERIAL_PHASE2.md``: Phase 2 (nested fields) guide
- ``KSERIAL_IMPLEMENTATION_SUMMARY.md``: Implementation details

Use Cases
=========

Monitoring Tools
----------------

Collect cgroup metrics for monitoring systems::

    # Query multiple metrics
    ./kserial_client.py level nr_descendants --json | \
        prometheus_exporter

Debugging
---------

Inspect cgroup state during debugging::

    # Quick snapshot
    ./test_kserial_real level nr_descendants max_depth

Performance Analysis
--------------------

Track cgroup growth and relationships::

    # Continuous monitoring
    ./example_monitor -m -c

System Introspection
--------------------

Safely access kernel data structures without direct memory access,
useful for:

- Container runtime debugging
- Resource monitoring
- Hierarchy analysis
- Relationship tracking

See Also
========

- :ref:`cgroup-v2 <cgroup-v2>`: Control Group v2 documentation
- ``Documentation/core-api/cgroup.rst``: cgroup core API
- ``Documentation/bpf/btf.rst``: BTF documentation

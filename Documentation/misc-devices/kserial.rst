.. SPDX-License-Identifier: GPL-2.0

======================================================
kserial - BTF-aware binary serialization for userspace
======================================================

Overview
========

``/dev/kserial`` is a character device that accepts raw kernel struct bytes
from userspace, uses vmlinux BTF to walk each struct member, and emits a
protobuf-compatible binary message into a per-fd ring buffer.  Userspace
can consume the encoded messages via ``read()`` or by ``mmap()``-ing the
ring buffer directly for zero-copy access.

Requires ``CONFIG_KSERIAL=y``, ``CONFIG_DEBUG_INFO_BTF=y``, and
``CONFIG_BPF_SYSCALL=y``.

Protocol
========

Writing a request
-----------------

A single ``write()`` must supply the following layout::

    [struct kserial_req]           - fixed 16-byte request header
    [char  type_name[name_len]]    - NUL-terminated BTF struct name
    [u8    data[data_len]]         - raw struct bytes to serialize

The kernel looks up *type_name* in vmlinux BTF.  If found, it walks every
non-bitfield member and encodes each field as a protobuf-compatible
tag+value record.  The encoded message is appended to the per-fd ring
buffer.

Wire format of an encoded message
----------------------------------

Each message in the ring buffer is laid out as::

    [struct kserial_msg_hdr]       - fixed 16-byte message header
    [char  type_name[name_len]]    - struct name (NUL-terminated)
    [field record ...]             - protobuf-compatible tag+value pairs

Field records follow the protobuf wire format:

- **WIRE_VARINT (0)**: LEB128 varint.  Signed integers (BTF_INT_SIGNED,
  signed enums) use zigzag encoding when ``KSERIAL_FLAG_ZIGZAG`` is set in
  ``kserial_msg_hdr.flags``.
- **WIRE_I64 (1)**: 8-byte little-endian (doubles, pointers).  Note that
  64-bit integers (u64/s64) use ``WIRE_VARINT``, not ``WIRE_I64``.
- **WIRE_LEN (2)**: varint length followed by that many bytes (arrays,
  nested structs, unusual-width floats).
- **WIRE_I32 (5)**: 4-byte little-endian (floats).

Field numbers are 1-based BTF member indices, matching the protobuf
convention.  Bitfield members are silently skipped.

Consuming via read()
====================

``read()`` copies encoded messages from the ring buffer into a
userspace-provided buffer and advances the consumer automatically.  It
blocks until at least one byte is available unless ``O_NONBLOCK`` is set.

Consuming via mmap()
====================

``mmap()`` maps the per-fd ring buffer directly into userspace.  The
mapping must be exactly ``PAGE_SIZE + data_size`` bytes at offset 0.

The first page contains a ``struct kserial_rb_ctrl`` control block::

    struct kserial_rb_ctrl {
        __u32 magic;        /* KSERIAL_MAGIC */
        __u32 version;      /* KSERIAL_VERSION */
        __u64 data_size;    /* ring buffer data size in bytes */
        __u64 producer;     /* kernel-updated, use load-acquire to read */
        __u64 consumer;     /* reader-updated, use store-release to write */
        __u32 flags;
        __u32 reserved[3];
    };

Zero-copy consumption procedure:

1. Read ``producer`` with an appropriate load-acquire barrier.
2. Compute ``avail = producer - consumer``.
3. Read ``avail`` bytes from ``data[(consumer % data_size) ...]``,
   wrapping at ``data_size`` if necessary.
4. Advance ``consumer`` by writing to it directly (with a store-release
   barrier) or by issuing ``KSERIAL_IOC_CONSUME``.

.. note::

   Do not mix ``read()``-based and ``mmap()``-based consumption on the same
   file descriptor.

   Writes to ``ctrl->producer`` from userspace are ignored by the kernel;
   all kernel-side capacity calculations use an internal index that is not
   accessible through the mapping.

ioctls
======

``KSERIAL_IOC_FLUSH``
    Advance the consumer to the current producer, discarding all unread
    data.

``KSERIAL_IOC_RESET``
    Reset both producer and consumer to zero, clearing the ring buffer.

``KSERIAL_IOC_GETINFO`` (``struct kserial_rb_ctrl *``)
    Fill the provided ``kserial_rb_ctrl`` with a consistent snapshot of
    the current ring buffer state.

``KSERIAL_IOC_CONSUME`` (``__u64 *``)
    Advance the consumer by the given number of bytes (clamped to
    available data).  Intended for mmap consumers.

Protobuf schema generation
==========================

``tools/testing/kserial/kserial_gen_proto`` reads vmlinux BTF and emits a
``proto3`` schema that is wire-compatible with kserial's encoding.  The
generated field numbers match kserial's 1-based BTF member indices, so any
``protoc``-generated decoder can consume kserial messages directly without
custom BTF code at runtime.

Usage::

    ./kserial_gen_proto tcp_info > tcp_info.proto
    protoc --proto_path=. tcp_info.proto          # validate syntax
    protoc --python_out=. tcp_info.proto          # generate Python bindings

Requires ``CONFIG_DEBUG_INFO_BTF=y`` (to read ``/sys/kernel/btf/vmlinux``).
The tool is built alongside the other kserial userspace tools via
``make -C tools/testing/kserial/``.

Example
=======

A complete example (encode a ``struct timespec64`` and read it back) is
provided in ``tools/testing/kserial/kserial_test.c``.

.. code-block:: c

    /* Minimal encode + read example */
    int fd = open("/dev/kserial", O_RDWR);

    struct kserial_req req = {
        .magic    = KSERIAL_MAGIC,
        .version  = KSERIAL_VERSION,
        .name_len = sizeof("timespec64"),
        .data_len = sizeof(ts),
    };
    /* write req + "timespec64" + ts bytes in one write() */

    /* read encoded message back */
    uint8_t buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf));

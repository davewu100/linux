.. SPDX-License-Identifier: GPL-2.0

.. _kserial:

/dev/kserial - BTF-based kernel struct access
=============================================

``/dev/kserial`` is a misc character device that allows privileged userspace to
read and write specific fields of kernel data structures identified by BTF
(struct name + field path). The base pointer for the struct is obtained either
from a kernel symbol name (e.g. ``init_task``) or from the ``task_struct`` of a
given PID. It is intended for monitoring, debugging, and controlled tooling;
access is restricted by design.

Access control (intentional design)
-----------------------------------

Opening ``/dev/kserial`` requires:

- **CAP_SYS_RAWIO** – The device exposes raw access to kernel memory (struct
  fields). This capability is the same as required for ``/dev/mem`` and similar
  raw I/O devices.

- **No LOCKDOWN_DEV_MEM** – If the kernel is in lockdown with
  ``lockdown=integrity`` or ``lockdown=confidentiality``, ``security_locked_down(LOCKDOWN_DEV_MEM)``
  denies access. This is intentional: kserial allows reading and optionally
  writing arbitrary BTF-described kernel state, so it is gated by the same
  lockdown as ``/dev/mem``.

These checks are deliberate. Do not relax them without a strong security
justification; the device is meant for root-only, controlled environments
(e.g. debug or monitoring agents).

Configuration
-------------

Enable with ``CONFIG_KSERIAL=y`` (depends on ``CONFIG_BPF_SYSCALL`` for BTF).
The device appears as ``/dev/kserial``.

Each open file can have up to 128 *slots*. Each slot is configured to point at
one struct field (base + offset + size). Configuration is done by:

- **ioctl(KSERIAL_IOC_SET_TARGET)** with a ``struct kserial_target``
  (see ``include/uapi/linux/kserial.h``): set ``type`` to ``KSERIAL_OBJ_SYMBOL``
  or ``KSERIAL_OBJ_PID``, fill ``symbol_name`` or ``pid``, and
  ``struct_name`` / ``field_path`` (e.g. ``task_struct``, ``pid``).

- **Echo (write)** text lines to the device (shell-friendly)::

    echo "symbol init_task task_struct pid" > /dev/kserial
    echo "pid 1234 task_struct tgid" >> /dev/kserial

  After such a write, subsequent reads return text (see below).

Read / write / mmap
-------------------

- **Binary read/write**: ``lseek(fd, slot_index, SEEK_SET)`` then ``read()`` or
  ``write()``. The operation acts on the configured field for that slot (raw
  bytes). Slot index is 0..31.

- **Text read (after echo config)**: Without lseek or with ``lseek(fd, 0, SEEK_SET)``,
  ``read()`` returns lines of the form ``name value`` (e.g.
  ``init_task.task_struct.pid 1``). Intended for ``cat /dev/kserial``.

- **mmap**: The mapped region corresponds to the current slot’s field (selected
  by ``f_pos`` as slot index).

io_uring
--------

When using io_uring with ``IORING_OP_URING_CMD``, the command payload uses
``sqe->off`` as the slot index for read/write. See the UAPI header for opcodes.

UAPI
----

``include/uapi/linux/kserial.h`` defines ``struct kserial_target``,
``KSERIAL_IOC_SET_TARGET``, ``KSERIAL_IOC_GET_NSLOTS``, and uring opcodes.

Testing
-------

kselftest: ``tools/testing/selftests/kserial/``. Run as root::

  sudo make -C tools/testing/selftests run_tests COLLECTION=kserial

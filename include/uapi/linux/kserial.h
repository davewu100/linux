/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_KSERIAL_H
#define _UAPI_LINUX_KSERIAL_H

#include <linux/types.h>

/*
 * Object type for KSERIAL_SET_TARGET: base pointer source.
 * KSERIAL_OBJ_SYMBOL = use symbol_name from kallsyms (e.g. "init_task").
 * KSERIAL_OBJ_PID    = use task_struct of given pid (pid must be > 0).
 */
#define KSERIAL_OBJ_SYMBOL  0
#define KSERIAL_OBJ_PID     1

#define KSERIAL_SYMBOL_LEN  64
#define KSERIAL_STRUCT_LEN  64
#define KSERIAL_PATH_LEN    256
#define KSERIAL_MAX_SLOTS   128  /* max targets per fd */

struct kserial_target {
	__u32 type;    /* KSERIAL_OBJ_SYMBOL or KSERIAL_OBJ_PID */
	__u32 pid;     /* for KSERIAL_OBJ_PID: pid; ignored for SYMBOL */
	__u32 slot_id; /* slot index 0..KSERIAL_MAX_SLOTS-1; multiple configs per fd */
	char  symbol_name[KSERIAL_SYMBOL_LEN];
	char  struct_name[KSERIAL_STRUCT_LEN];
	char  field_path[KSERIAL_PATH_LEN];
};

#define KSERIAL_IOC_MAGIC   'K'
#define KSERIAL_IOC_SET_TARGET   _IOW(KSERIAL_IOC_MAGIC, 1, struct kserial_target)
#define KSERIAL_IOC_GET_NSLOTS   _IOR(KSERIAL_IOC_MAGIC, 2, __u32)

/*
 * read(2)/write(2): raw binary of the field (no string formatting).
 * Use lseek(fd, slot_index, SEEK_SET) to select which slot; then read/write that slot.
 *
 * io_uring uring_cmd: sqe->cmd_op = READ/WRITE; sqe->off = slot_id; sqe->addr = user buf; sqe->len = length.
 */
#define KSERIAL_URING_OP_READ   0
#define KSERIAL_URING_OP_WRITE  1

#endif /* _UAPI_LINUX_KSERIAL_H */

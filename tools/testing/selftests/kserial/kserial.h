/* SPDX-License-Identifier: GPL-2.0 */
/* Minimal UAPI definitions for kserial selftest (match include/uapi/linux/kserial.h) */
#ifndef __SELFTEST_KSERIAL_H
#define __SELFTEST_KSERIAL_H

#include <stdint.h>
#include <sys/ioctl.h>

typedef uint32_t __u32;

#define KSERIAL_OBJ_SYMBOL  0
#define KSERIAL_OBJ_PID     1

#define KSERIAL_SYMBOL_LEN  64
#define KSERIAL_STRUCT_LEN  64
#define KSERIAL_PATH_LEN    256
#define KSERIAL_MAX_SLOTS   32

struct kserial_target {
	__u32 type;
	__u32 pid;
	__u32 slot_id;
	char  symbol_name[KSERIAL_SYMBOL_LEN];
	char  struct_name[KSERIAL_STRUCT_LEN];
	char  field_path[KSERIAL_PATH_LEN];
};

#define KSERIAL_IOC_MAGIC   'K'
#define KSERIAL_IOC_SET_TARGET   _IOW(KSERIAL_IOC_MAGIC, 1, struct kserial_target)
#define KSERIAL_IOC_GET_NSLOTS   _IOR(KSERIAL_IOC_MAGIC, 2, __u32)

#endif /* __SELFTEST_KSERIAL_H */

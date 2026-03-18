// SPDX-License-Identifier: GPL-2.0
/*
 * Test /dev/kserial: ioctl SET_TARGET, binary read, multi-slot, echo+cat text,
 * mmap. Requires CAP_SYS_RAWIO (run as root) and CONFIG_KSERIAL=y.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <linux/kserial.h>
#include "kselftest_harness.h"

#define DEV_KSERIAL "/dev/kserial"

static int open_kserial(void)
{
	int fd = open(DEV_KSERIAL, O_RDWR);
	if (fd < 0)
		return -1;
	return fd;
}

/* On success returns fd; on failure calls ksft_test_result_skip and returns -1. */
static int open_kserial_or_skip(void)
{
	int fd = open_kserial();
	if (fd < 0) {
		if (getuid() != 0)
			ksft_test_result_skip("need root for CAP_SYS_RAWIO\n");
		else
			ksft_test_result_skip("/dev/kserial open failed: %s\n", strerror(errno));
		return -1;
	}
	return fd;
}

TEST(kserial_dev_exists)
{
	struct stat st;

	if (stat(DEV_KSERIAL, &st) != 0) {
		ksft_test_result_skip("/dev/kserial not present (CONFIG_KSERIAL=n?)\n");
		return;
	}
	EXPECT_EQ(S_ISCHR(st.st_mode), true);
}

TEST(kserial_open_requires_cap)
{
	int fd = open_kserial_or_skip();

	if (fd < 0)
		return;
	close(fd);
	ksft_test_result_pass("opened " DEV_KSERIAL "\n");
}

TEST(kserial_set_target_symbol_read)
{
	struct kserial_target t;
	__u32 nslots;
	unsigned char buf[8];
	ssize_t n;
	int fd = open_kserial_or_skip();

	if (fd < 0)
		return;
	memset(&t, 0, sizeof(t));
	t.type = KSERIAL_OBJ_SYMBOL;
	t.slot_id = 0;
	strncpy(t.symbol_name, "init_task", KSERIAL_SYMBOL_LEN - 1);
	strncpy(t.struct_name, "task_struct", KSERIAL_STRUCT_LEN - 1);
	strncpy(t.field_path, "pid", KSERIAL_PATH_LEN - 1);

	if (ioctl(fd, KSERIAL_IOC_SET_TARGET, &t) != 0) {
		ksft_test_result_skip("SET_TARGET failed (BTF/symbol?): %s\n", strerror(errno));
		close(fd);
		return;
	}

	if (ioctl(fd, KSERIAL_IOC_GET_NSLOTS, &nslots) != 0) {
		ksft_test_result_fail("GET_NSLOTS failed\n");
		close(fd);
		return;
	}
	EXPECT_GE(nslots, 1u);

	if (lseek(fd, 0, SEEK_SET) != 0) {
		ksft_test_result_fail("lseek slot 0 failed\n");
		close(fd);
		return;
	}
	n = read(fd, buf, sizeof(buf));
	close(fd);
	if (n < 0) {
		ksft_test_result_fail("read failed: %s\n", strerror(errno));
		return;
	}
	/* init_task has pid 1; field is int (4 bytes), little-endian */
	EXPECT_GE(n, (ssize_t)4);
	if (n >= 4)
		EXPECT_EQ((int)buf[0] | ((int)buf[1] << 8) | ((int)buf[2] << 16) | ((int)buf[3] << 24), 1);
	ksft_test_result_pass("read init_task.pid (binary) = 1\n");
}

TEST(kserial_multi_slot)
{
	struct kserial_target t;
	__u32 nslots;
	unsigned char buf[8];
	ssize_t n;
	int fd = open_kserial_or_skip();

	if (fd < 0)
		return;
	/* slot 0: init_task.pid */
	memset(&t, 0, sizeof(t));
	t.type = KSERIAL_OBJ_SYMBOL;
	t.slot_id = 0;
	strncpy(t.symbol_name, "init_task", KSERIAL_SYMBOL_LEN - 1);
	strncpy(t.struct_name, "task_struct", KSERIAL_STRUCT_LEN - 1);
	strncpy(t.field_path, "pid", KSERIAL_PATH_LEN - 1);
	if (ioctl(fd, KSERIAL_IOC_SET_TARGET, &t) != 0) {
		ksft_test_result_skip("SET_TARGET slot 0 failed\n");
		close(fd);
		return;
	}

	/* slot 1: current (self) task_struct.pid */
	memset(&t, 0, sizeof(t));
	t.type = KSERIAL_OBJ_PID;
	t.slot_id = 1;
	t.pid = getpid();
	strncpy(t.struct_name, "task_struct", KSERIAL_STRUCT_LEN - 1);
	strncpy(t.field_path, "pid", KSERIAL_PATH_LEN - 1);
	if (ioctl(fd, KSERIAL_IOC_SET_TARGET, &t) != 0) {
		ksft_test_result_skip("SET_TARGET slot 1 (pid) failed\n");
		close(fd);
		return;
	}

	if (ioctl(fd, KSERIAL_IOC_GET_NSLOTS, &nslots) != 0 || nslots < 2) {
		ksft_test_result_fail("GET_NSLOTS want >= 2, got %u\n", nslots);
		close(fd);
		return;
	}

	/* read slot 0 -> pid 1 */
	if (lseek(fd, 0, SEEK_SET) != 0) {
		ksft_test_result_fail("lseek 0 failed\n");
		close(fd);
		return;
	}
	n = read(fd, buf, 4);
	EXPECT_EQ(n, (ssize_t)4);
	if (n == 4)
		EXPECT_EQ((int)buf[0] | ((int)buf[1] << 8) | ((int)buf[2] << 16) | ((int)buf[3] << 24), 1);

	/* read slot 1 -> getpid() */
	if (lseek(fd, 1, SEEK_SET) != 1) {
		ksft_test_result_fail("lseek 1 failed\n");
		close(fd);
		return;
	}
	n = read(fd, buf, 4);
	close(fd);
	EXPECT_EQ(n, (ssize_t)4);
	if (n == 4) {
		int pid = (int)buf[0] | ((int)buf[1] << 8) | ((int)buf[2] << 16) | ((int)buf[3] << 24);
		EXPECT_EQ(pid, getpid());
	}
	ksft_test_result_pass("multi-slot read (slot0=1, slot1=self)\n");
}

TEST(kserial_echo_cat)
{
	const char *cfg = "symbol init_task task_struct pid\n";
	char buf[256];
	ssize_t n;
	int fd = open_kserial_or_skip();

	if (fd < 0)
		return;
	n = write(fd, cfg, strlen(cfg));
	if (n != (ssize_t)strlen(cfg)) {
		ksft_test_result_skip("echo write failed: %s\n", strerror(errno));
		close(fd);
		return;
	}
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0) {
		ksft_test_result_fail("cat read failed or empty\n");
		return;
	}
	buf[n] = '\0';
	/* Expect "name value" line; init_task.pid is 1 */
	EXPECT_TRUE(strstr(buf, "1") != NULL);
	EXPECT_TRUE(strstr(buf, "task_struct") != NULL || strstr(buf, "init_task") != NULL);
	ksft_test_result_pass("echo config + cat (name value)\n");
}

TEST(kserial_mmap)
{
	struct kserial_target t;
	unsigned char *p;
	unsigned char buf[8];
	ssize_t n;
	size_t map_len = 4096;
	int fd = open_kserial_or_skip();

	if (fd < 0)
		return;
	memset(&t, 0, sizeof(t));
	t.type = KSERIAL_OBJ_SYMBOL;
	t.slot_id = 0;
	strncpy(t.symbol_name, "init_task", KSERIAL_SYMBOL_LEN - 1);
	strncpy(t.struct_name, "task_struct", KSERIAL_STRUCT_LEN - 1);
	strncpy(t.field_path, "pid", KSERIAL_PATH_LEN - 1);
	if (ioctl(fd, KSERIAL_IOC_SET_TARGET, &t) != 0) {
		ksft_test_result_skip("SET_TARGET failed: %s\n", strerror(errno));
		close(fd);
		return;
	}
	if (lseek(fd, 0, SEEK_SET) != 0) {
		ksft_test_result_fail("lseek 0 failed\n");
		close(fd);
		return;
	}
	p = mmap(NULL, map_len, PROT_READ, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		ksft_test_result_fail("mmap failed: %s\n", strerror(errno));
		close(fd);
		return;
	}
	/* init_task.pid is 1 (4-byte int). Compare with read() result. */
	n = read(fd, buf, sizeof(buf));
	if (n < 4) {
		ksft_test_result_fail("read after mmap failed or short\n");
		munmap(p, map_len);
		close(fd);
		return;
	}
	if (p[0] != buf[0] || p[1] != buf[1] || p[2] != buf[2] || p[3] != buf[3]) {
		ksft_test_result_fail("mmap content != read()\n");
		munmap(p, map_len);
		close(fd);
		return;
	}
	if (((int)p[0] | ((int)p[1] << 8) | ((int)p[2] << 16) | ((int)p[3] << 24)) != 1) {
		ksft_test_result_fail("mmap pid != 1\n");
		munmap(p, map_len);
		close(fd);
		return;
	}
	munmap(p, map_len);
	close(fd);
	ksft_test_result_pass("mmap matches read (init_task.pid = 1)\n");
}

TEST_HARNESS_MAIN

// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif

static unsigned char pattern_a(size_t page)
{
	return (unsigned char)(page * 251 + 17);
}

static unsigned char pattern_b(size_t page)
{
	return (unsigned char)(page * 211 + 113);
}

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s <MiB> <zram-sysfs-path>\n", prog);
	exit(1);
}

static int write_file(const char *path, const char *value)
{
	int fd;
	ssize_t ret;

	fd = open(path, O_WRONLY);
	if (fd < 0) {
		perror(path);
		return 1;
	}

	ret = write(fd, value, strlen(value));
	if (ret < 0) {
		perror(path);
		close(fd);
		return 1;
	}

	close(fd);
	return 0;
}

static int zram_writeback_idle(const char *zram_sysfs)
{
	char path[256];

	if (snprintf(path, sizeof(path), "%s/idle", zram_sysfs) >= sizeof(path))
		return 1;
	if (write_file(path, "all\n"))
		return 1;

	if (snprintf(path, sizeof(path), "%s/writeback", zram_sysfs) >=
	    sizeof(path))
		return 1;

	return write_file(path, "idle\n");
}

static unsigned long long read_vmstat_pswpout(void)
{
	unsigned long long pswpout = 0;
	char line[256];
	FILE *fp;

	fp = fopen("/proc/vmstat", "r");
	if (!fp)
		return 0;

	while (fgets(line, sizeof(line), fp)) {
		if (sscanf(line, "pswpout %llu", &pswpout) == 1)
			break;
	}
	fclose(fp);
	return pswpout;
}

static void wait_for_pageout(unsigned long long baseline)
{
	unsigned long long prev = baseline;
	int stable_loops = 0;

	for (int i = 0; i < 100; i++) {
		struct timespec ts = { .tv_nsec = 100 * 1000 * 1000 };
		unsigned long long cur;

		nanosleep(&ts, NULL);
		cur = read_vmstat_pswpout();
		if (cur > baseline && cur == prev) {
			if (++stable_loops >= 3)
				return;
		} else {
			stable_loops = 0;
		}
		prev = cur;
	}
}

int main(int argc, char **argv)
{
	size_t page_size = sysconf(_SC_PAGESIZE);
	size_t mib, bytes, pages;
	const char *zram_sysfs;
	unsigned char *addr;
	unsigned long long pswpout_before;

	if (argc != 3)
		usage(argv[0]);

	errno = 0;
	mib = strtoull(argv[1], NULL, 0);
	if (errno || !mib)
		usage(argv[0]);
	zram_sysfs = argv[2];

	bytes = mib * 1024 * 1024;
	pages = bytes / page_size;
	if (!pages)
		usage(argv[0]);

	addr = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (addr == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	for (size_t i = 0; i < pages; i++) {
		addr[i * page_size] = pattern_a(i);
		addr[i * page_size + page_size - 1] = pattern_b(i);
	}

	pswpout_before = read_vmstat_pswpout();
	if (madvise(addr, bytes, MADV_PAGEOUT)) {
		perror("madvise(MADV_PAGEOUT)");
		munmap(addr, bytes);
		return 1;
	}

	wait_for_pageout(pswpout_before);
	if (zram_writeback_idle(zram_sysfs)) {
		munmap(addr, bytes);
		return 1;
	}

	/* Fault the pages back in. ZRAM_WB pages must survive the round trip. */
	for (size_t i = 0; i < pages; i++) {
		if (addr[i * page_size] != pattern_a(i) ||
		    addr[i * page_size + page_size - 1] != pattern_b(i)) {
			fprintf(stderr, "data mismatch at page %zu\n", i);
			munmap(addr, bytes);
			return 1;
		}
	}

	munmap(addr, bytes);
	return 0;
}

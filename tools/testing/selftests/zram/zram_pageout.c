// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif

static unsigned char pattern(size_t page)
{
	return (unsigned char)(page * 251 + 17);
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

int main(int argc, char **argv)
{
	size_t page_size = sysconf(_SC_PAGESIZE);
	size_t mib, bytes, pages;
	const char *zram_sysfs;
	unsigned char *addr;

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

	for (size_t i = 0; i < pages; i++)
		addr[i * page_size] = pattern(i);

	if (madvise(addr, bytes, MADV_PAGEOUT)) {
		perror("madvise(MADV_PAGEOUT)");
		munmap(addr, bytes);
		return 1;
	}

	sleep(1);
	if (zram_writeback_idle(zram_sysfs)) {
		munmap(addr, bytes);
		return 1;
	}
	sleep(1);

	/* Fault the pages back in. ZRAM_WB pages must survive the round trip. */
	for (size_t i = 0; i < pages; i++) {
		if (addr[i * page_size] != pattern(i)) {
			fprintf(stderr, "data mismatch at page %zu\n", i);
			munmap(addr, bytes);
			return 1;
		}
	}

	munmap(addr, bytes);
	return 0;
}

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

static uint64_t sink;

static uint64_t nsec_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a;
	uint64_t y = *(const uint64_t *)b;

	return (x > y) - (x < y);
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s --gb N [--writeback] [--zram-sys PATH]\n", prog);
	exit(1);
}

static void write_file(const char *path, const char *value)
{
	int fd;
	ssize_t ret;

	fd = open(path, O_WRONLY);
	if (fd < 0) {
		perror(path);
		exit(1);
	}

	ret = write(fd, value, strlen(value));
	if (ret < 0) {
		perror(path);
		close(fd);
		exit(1);
	}

	close(fd);
}

static void zram_writeback_idle(const char *zram_sys)
{
	char path[256];

	if (snprintf(path, sizeof(path), "%s/idle", zram_sys) >= sizeof(path))
		exit(1);
	write_file(path, "all\n");

	if (snprintf(path, sizeof(path), "%s/writeback", zram_sys) >=
	    sizeof(path))
		exit(1);
	write_file(path, "idle\n");
}

int main(int argc, char **argv)
{
	const char *zram_sys = "/sys/block/zram0";
	size_t page_size = sysconf(_SC_PAGESIZE);
	size_t gb = 0;
	size_t bytes, pages;
	uint64_t *lat;
	unsigned char *addr;
	int writeback = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--gb") && i + 1 < argc)
			gb = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--writeback"))
			writeback = 1;
		else if (!strcmp(argv[i], "--zram-sys") && i + 1 < argc)
			zram_sys = argv[++i];
		else
			usage(argv[0]);
	}

	if (!gb)
		usage(argv[0]);

	bytes = gb * 1024ULL * 1024ULL * 1024ULL;
	pages = bytes / page_size;

	printf("allocating %zu GB, %zu pages, writeback=%d\n",
	       gb, pages, writeback);

	addr = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (addr == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	lat = calloc(pages, sizeof(*lat));
	if (!lat) {
		perror("calloc");
		munmap(addr, bytes);
		return 1;
	}

	for (size_t i = 0; i < pages; i++)
		addr[i * page_size] = (unsigned char)i;

	if (madvise(addr, bytes, MADV_PAGEOUT))
		perror("madvise(MADV_PAGEOUT)");
	sleep(3);

	if (writeback) {
		zram_writeback_idle(zram_sys);
		sleep(3);
	}

	for (size_t i = 0; i < pages; i++) {
		uint64_t start = nsec_now();

		sink += addr[i * page_size];
		lat[i] = nsec_now() - start;
	}

	qsort(lat, pages, sizeof(*lat), cmp_u64);

	{
		long double sum = 0;
		size_t p50 = pages * 50 / 100;
		size_t p99 = pages * 99 / 100;
		size_t p999 = pages * 999 / 1000;

		for (size_t i = 0; i < pages; i++)
			sum += lat[i];

		printf("pages=%zu\n", pages);
		printf("avg_ns=%.0Lf\n", sum / pages);
		printf("p50_ns=%llu\n", (unsigned long long)lat[p50]);
		printf("p99_ns=%llu\n", (unsigned long long)lat[p99]);
		printf("p999_ns=%llu\n", (unsigned long long)lat[p999]);
		printf("sink=%llu\n", (unsigned long long)sink);
	}

	free(lat);
	munmap(addr, bytes);
	return 0;
}

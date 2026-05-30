// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
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
		"Usage: %s --gb N [--writeback] [--random] [--drop-caches] [--zram-sys PATH]\n",
		prog);
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

static unsigned long long read_bd_reads(const char *zram_sys)
{
	char path[256];
	char line[256];
	unsigned long long bd_count, bd_reads, bd_writes;
	FILE *fp;

	if (snprintf(path, sizeof(path), "%s/bd_stat", zram_sys) >= sizeof(path))
		exit(1);

	fp = fopen(path, "r");
	if (!fp) {
		perror(path);
		exit(1);
	}

	if (!fgets(line, sizeof(line), fp) ||
	    sscanf(line, "%llu %llu %llu", &bd_count, &bd_reads, &bd_writes) != 3) {
		fprintf(stderr, "failed to parse %s\n", path);
		fclose(fp);
		exit(1);
	}

	fclose(fp);
	return bd_reads;
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

static void shuffle_pages(size_t *order, size_t pages)
{
	unsigned int seed;

	if (getrandom(&seed, sizeof(seed), GRND_NONBLOCK) < 0)
		seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();

	for (size_t i = pages - 1; i > 0; i--) {
		size_t j = (size_t)(rand_r(&seed) % (unsigned int)(i + 1));
		size_t tmp = order[i];

		order[i] = order[j];
		order[j] = tmp;
	}
}

static void wait_for_pageout(void)
{
	unsigned long long prev = 0, cur = 0;
	char line[256];
	FILE *fp;
	int stable = 0;

	for (int i = 0; i < 100; i++) {
		struct timespec ts = { .tv_nsec = 100 * 1000 * 1000 };

		nanosleep(&ts, NULL);
		fp = fopen("/proc/vmstat", "r");
		if (!fp)
			continue;
		cur = 0;
		while (fgets(line, sizeof(line), fp)) {
			if (sscanf(line, "pswpout %llu", &cur) == 1)
				break;
		}
		fclose(fp);
		if (cur && cur == prev) {
			if (++stable >= 3)
				return;
		} else {
			stable = 0;
		}
		prev = cur;
	}
}

int main(int argc, char **argv)
{
	const char *zram_sys = "/sys/block/zram0";
	size_t page_size = sysconf(_SC_PAGESIZE);
	size_t gb = 0;
	size_t bytes, pages;
	uint64_t *lat;
	size_t *order;
	unsigned char *addr;
	unsigned long long bd_reads_before, bd_reads_after, bd_reads_delta;
	int writeback = 0;
	int random_order = 0;
	int drop_caches = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--gb") && i + 1 < argc)
			gb = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--writeback"))
			writeback = 1;
		else if (!strcmp(argv[i], "--random"))
			random_order = 1;
		else if (!strcmp(argv[i], "--drop-caches"))
			drop_caches = 1;
		else if (!strcmp(argv[i], "--zram-sys") && i + 1 < argc)
			zram_sys = argv[++i];
		else
			usage(argv[0]);
	}

	if (!gb)
		usage(argv[0]);

	bytes = gb * 1024ULL * 1024ULL * 1024ULL;
	pages = bytes / page_size;

	printf("allocating %zu GB, %zu pages, writeback=%d random=%d drop_caches=%d\n",
	       gb, pages, writeback, random_order, drop_caches);

	addr = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (addr == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	lat = calloc(pages, sizeof(*lat));
	order = malloc(pages * sizeof(*order));
	if (!lat || !order) {
		perror("alloc");
		free(lat);
		free(order);
		munmap(addr, bytes);
		return 1;
	}

	for (size_t i = 0; i < pages; i++)
		order[i] = i;

	for (size_t i = 0; i < pages; i++)
		addr[i * page_size] = (unsigned char)i;

	if (madvise(addr, bytes, MADV_PAGEOUT)) {
		perror("madvise(MADV_PAGEOUT)");
		free(lat);
		free(order);
		munmap(addr, bytes);
		return 1;
	}
	wait_for_pageout();

	if (writeback) {
		zram_writeback_idle(zram_sys);
		sleep(3);
	}

	if (drop_caches) {
		sync();
		write_file("/proc/sys/vm/drop_caches", "3\n");
	}

	if (random_order)
		shuffle_pages(order, pages);

	bd_reads_before = read_bd_reads(zram_sys);

	for (size_t n = 0; n < pages; n++) {
		size_t i = order[n];
		uint64_t start = nsec_now();

		sink += addr[i * page_size];
		lat[n] = nsec_now() - start;
	}

	bd_reads_after = read_bd_reads(zram_sys);
	bd_reads_delta = bd_reads_after - bd_reads_before;

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
		printf("bd_reads_before=%llu\n", bd_reads_before);
		printf("bd_reads_after=%llu\n", bd_reads_after);
		printf("bd_reads_delta=%llu\n", bd_reads_delta);
		if (pages)
			printf("bd_reads_per_page=%.4Lf\n",
			       (long double)bd_reads_delta / pages);
		printf("sink=%llu\n", (unsigned long long)sink);
	}

	free(order);
	free(lat);
	munmap(addr, bytes);
	return 0;
}

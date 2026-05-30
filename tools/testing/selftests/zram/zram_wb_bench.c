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

#ifndef MADV_DONTNEED
#define MADV_DONTNEED 4
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
		"Usage: %s --gb N [options]\n"
		"Options:\n"
		"  --writeback          writeback idle pages before read\n"
		"  --random             shuffle access order\n"
		"  --seed N             random shuffle seed (default: 42 if --random)\n"
		"  --stride N           touch every Nth page (default: 1)\n"
		"  --cold               --stride 64 --seed 42 --drop-caches\n"
		"  --drop-caches        sync and drop page cache before read\n"
		"  --zram-sys PATH      zram sysfs path (default /sys/block/zram0)\n",
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

static void shuffle_pages(size_t *order, size_t count, unsigned int seed)
{
	for (size_t i = count - 1; i > 0; i--) {
		size_t j = (size_t)(rand_r(&seed) % (unsigned int)(i + 1));
		size_t tmp = order[i];

		order[i] = order[j];
		order[j] = tmp;
	}
}

static int page_in_core(void *page_addr)
{
	unsigned char vec = 0;

	if (mincore(page_addr, 1, &vec))
		return -1;
	return !!(vec & 1);
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

static void print_stats(const char *prefix, uint64_t *lat, size_t count)
{
	long double sum = 0;
	size_t p50, p99, p999;

	if (!count)
		return;

	qsort(lat, count, sizeof(*lat), cmp_u64);
	p50 = count * 50 / 100;
	p99 = count * 99 / 100;
	p999 = count * 999 / 1000;

	for (size_t i = 0; i < count; i++)
		sum += lat[i];

	printf("%ssamples=%zu\n", prefix, count);
	printf("%savg_ns=%.0Lf\n", prefix, sum / count);
	printf("%sp50_ns=%llu\n", prefix, (unsigned long long)lat[p50]);
	printf("%sp99_ns=%llu\n", prefix, (unsigned long long)lat[p99]);
	printf("%sp999_ns=%llu\n", prefix, (unsigned long long)lat[p999]);
}

int main(int argc, char **argv)
{
	const char *zram_sys = "/sys/block/zram0";
	size_t page_size = sysconf(_SC_PAGESIZE);
	size_t gb = 0, stride = 1;
	size_t bytes, pages, samples;
	uint64_t *lat, *fault_lat;
	size_t *order;
	unsigned char *addr;
	unsigned long long bd_reads_before, bd_reads_after, bd_reads_delta;
	int writeback = 0;
	int random_order = 0;
	int drop_caches = 0;
	int cold_mode = 0;
	int seed_set = 0;
	unsigned int seed = 42;
	size_t fault_count = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--gb") && i + 1 < argc)
			gb = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--writeback"))
			writeback = 1;
		else if (!strcmp(argv[i], "--random"))
			random_order = 1;
		else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
			seed = (unsigned int)strtoul(argv[++i], NULL, 0);
			seed_set = 1;
		} else if (!strcmp(argv[i], "--stride") && i + 1 < argc)
			stride = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--cold"))
			cold_mode = 1;
		else if (!strcmp(argv[i], "--drop-caches"))
			drop_caches = 1;
		else if (!strcmp(argv[i], "--zram-sys") && i + 1 < argc)
			zram_sys = argv[++i];
		else
			usage(argv[0]);
	}

	if (cold_mode) {
		stride = 64;
		seed = 42;
		seed_set = 1;
		drop_caches = 1;
		random_order = 1;
	}

	if (!gb || !stride)
		usage(argv[0]);

	if (random_order && !seed_set)
		seed = 42;

	bytes = gb * 1024ULL * 1024ULL * 1024ULL;
	pages = bytes / page_size;
	samples = (pages + stride - 1) / stride;

	printf("allocating %zu GB, %zu pages, samples=%zu stride=%zu "
	       "writeback=%d random=%d seed=%u drop_caches=%d cold=%d\n",
	       gb, pages, samples, stride, writeback, random_order, seed,
	       drop_caches, cold_mode);

	addr = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (addr == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	lat = calloc(samples, sizeof(*lat));
	fault_lat = calloc(samples, sizeof(*fault_lat));
	order = malloc(samples * sizeof(*order));
	if (!lat || !fault_lat || !order) {
		perror("alloc");
		free(lat);
		free(fault_lat);
		free(order);
		munmap(addr, bytes);
		return 1;
	}

	for (size_t i = 0; i < samples; i++)
		order[i] = i * stride;

	for (size_t i = 0; i < pages; i++)
		addr[i * page_size] = (unsigned char)i;

	if (madvise(addr, bytes, MADV_PAGEOUT)) {
		perror("madvise(MADV_PAGEOUT)");
		goto out_free;
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
		shuffle_pages(order, samples, seed);

	bd_reads_before = read_bd_reads(zram_sys);

	for (size_t n = 0; n < samples; n++) {
		size_t i = order[n];
		void *page_addr = addr + i * page_size;
		int in_core_before = page_in_core(page_addr);
		uint64_t start = nsec_now();

		sink += *(unsigned char *)page_addr;
		lat[n] = nsec_now() - start;

		if (in_core_before <= 0) {
			fault_lat[fault_count++] = lat[n];
		}
	}

	bd_reads_after = read_bd_reads(zram_sys);
	bd_reads_delta = bd_reads_after - bd_reads_before;

	print_stats("", lat, samples);
	printf("fault_samples=%zu\n", fault_count);
	print_stats("fault_", fault_lat, fault_count);
	printf("bd_reads_before=%llu\n", bd_reads_before);
	printf("bd_reads_after=%llu\n", bd_reads_after);
	printf("bd_reads_delta=%llu\n", bd_reads_delta);
	if (samples)
		printf("bd_reads_per_sample=%.4Lf\n",
		       (long double)bd_reads_delta / samples);
	printf("sink=%llu\n", (unsigned long long)sink);

out_free:
	free(order);
	free(fault_lat);
	free(lat);
	munmap(addr, bytes);
	return 0;
}

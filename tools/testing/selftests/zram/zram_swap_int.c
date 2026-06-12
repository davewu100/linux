// SPDX-License-Identifier: GPL-2.0
/*
 * zram swap integrity helper for kselftests.
 * mmap anonymous memory, fill with a deterministic pattern, swap out via
 * MADV_PAGEOUT, optionally writeback idle slots, then verify after
 * drop_caches.
 */
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

static unsigned long page_size;

static void write_file(const char *path, const char *value)
{
	int fd = open(path, O_WRONLY);
	ssize_t ret;

	if (fd < 0) {
		perror(path);
		exit(2);
	}

	ret = write(fd, value, strlen(value));
	if (ret < 0) {
		perror(path);
		close(fd);
		exit(2);
	}
	close(fd);
}

static int read_bd_stat(const char *zram_sys, unsigned long long *reads,
			unsigned long long *writes)
{
	char path[256];
	char line[256];
	unsigned long long count;
	FILE *fp;

	snprintf(path, sizeof(path), "%s/bd_stat", zram_sys);
	fp = fopen(path, "r");
	if (!fp) {
		perror(path);
		exit(2);
	}
	if (!fgets(line, sizeof(line), fp) ||
	    sscanf(line, "%llu %llu %llu", &count, reads, writes) != 3) {
		fprintf(stderr, "failed to parse %s\n", path);
		fclose(fp);
		exit(2);
	}
	fclose(fp);
	return 0;
}

static unsigned long long read_notify_free(const char *zram_sys)
{
	char path[256];
	char line[256];
	unsigned long long a, b, pad, notify;
	FILE *fp;

	snprintf(path, sizeof(path), "%s/io_stat", zram_sys);
	fp = fopen(path, "r");
	if (!fp) {
		perror(path);
		exit(2);
	}
	if (!fgets(line, sizeof(line), fp) ||
	    sscanf(line, "%llu %llu %llu %llu", &a, &b, &pad, &notify) != 4) {
		fprintf(stderr, "failed to parse %s\n", path);
		fclose(fp);
		exit(2);
	}
	fclose(fp);
	return notify;
}

static unsigned long long read_mm_compr(const char *zram_sys)
{
	char path[256];
	char line[512];
	unsigned long long orig, compr;
	FILE *fp;

	snprintf(path, sizeof(path), "%s/mm_stat", zram_sys);
	fp = fopen(path, "r");
	if (!fp) {
		perror(path);
		exit(2);
	}
	if (!fgets(line, sizeof(line), fp) ||
	    sscanf(line, "%llu %llu", &orig, &compr) != 2) {
		fprintf(stderr, "failed to parse %s\n", path);
		fclose(fp);
		exit(2);
	}
	fclose(fp);
	return compr;
}

static void zram_writeback_idle(const char *zram_sys)
{
	char path[256];

	snprintf(path, sizeof(path), "%s/idle", zram_sys);
	write_file(path, "all\n");
	snprintf(path, sizeof(path), "%s/writeback", zram_sys);
	write_file(path, "idle\n");
}

static void wait_for_pageout(void)
{
	unsigned long long prev = 0, cur = 0;
	char line[256];
	FILE *fp;
	int stable = 0;

	for (int i = 0; i < 120; i++) {
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

static unsigned char page_pattern(size_t page_idx, unsigned int seed)
{
	return (unsigned char)((page_idx * 0x9e3779b9u + seed) & 0xff);
}

static void fill_pattern(unsigned char *addr, size_t pages, size_t stride,
			 unsigned int seed)
{
	for (size_t p = 0; p < pages; p += stride) {
		unsigned char *page = addr + p * page_size;
		unsigned char val = page_pattern(p, seed);

		memset(page, val, page_size);
	}
}

static int verify_pattern(unsigned char *addr, size_t pages, size_t stride,
			  unsigned int seed)
{
	for (size_t p = 0; p < pages; p += stride) {
		unsigned char *page = addr + p * page_size;
		unsigned char expect = page_pattern(p, seed);

		for (size_t i = 0; i < page_size; i++) {
			if (page[i] != expect) {
				fprintf(stderr,
					"verify fail page=%zu off=%zu got=0x%02x expect=0x%02x\n",
					p, i, page[i], expect);
				return -1;
			}
		}
	}
	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  --mb N           allocation size in megabytes (default 64)\n"
		"  --stride N       touch every Nth page (default 1)\n"
		"  --seed N         pattern seed (default 42)\n"
		"  --writeback      idle writeback before verify\n"
		"  --drop-caches    sync+drop caches before verify (default on)\n"
		"  --no-drop-caches skip drop_caches\n"
		"  --zram-sys PATH  zram sysfs path (default /sys/block/zram0)\n"
		"  --expect-bd-reads expect backing reads delta > 0\n"
		"  --expect-bd-writes expect backing writes delta > 0\n"
		"  --expect-no-bd   expect backing reads delta == 0\n",
		prog);
	exit(2);
}

int main(int argc, char **argv)
{
	size_t mb = 64;
	size_t stride = 1;
	unsigned int seed = 42;
	int writeback = 0;
	int drop_caches = 1;
	int expect_bd_reads = 0;
	int expect_bd_writes = 0;
	int expect_no_bd = 0;
	const char *zram_sys = "/sys/block/zram0";
	unsigned char *addr;
	size_t bytes, pages;
	unsigned long long bd_reads_before, bd_reads_after;
	unsigned long long bd_writes_before, bd_writes_after;

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0) {
		perror("sysconf");
		return 2;
	}

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--mb") && i + 1 < argc)
			mb = (size_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--stride") && i + 1 < argc)
			stride = (size_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--seed") && i + 1 < argc)
			seed = (unsigned int)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--writeback"))
			writeback = 1;
		else if (!strcmp(argv[i], "--drop-caches"))
			drop_caches = 1;
		else if (!strcmp(argv[i], "--no-drop-caches"))
			drop_caches = 0;
		else if (!strcmp(argv[i], "--zram-sys") && i + 1 < argc)
			zram_sys = argv[++i];
		else if (!strcmp(argv[i], "--expect-bd-reads"))
			expect_bd_reads = 1;
		else if (!strcmp(argv[i], "--expect-bd-writes"))
			expect_bd_writes = 1;
		else if (!strcmp(argv[i], "--expect-no-bd"))
			expect_no_bd = 1;
		else
			usage(argv[0]);
	}

	if (!mb || !stride)
		usage(argv[0]);

	bytes = mb * 1024ULL * 1024ULL;
	pages = bytes / page_size;

	printf("zram_swap_int: %zu MB, %zu pages, stride=%zu seed=%u writeback=%d\n",
	       mb, pages, stride, seed, writeback);

	addr = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (addr == MAP_FAILED) {
		perror("mmap");
		return 2;
	}

	read_bd_stat(zram_sys, &bd_reads_before, &bd_writes_before);

	fill_pattern(addr, pages, stride, seed);

	if (madvise(addr, bytes, MADV_PAGEOUT)) {
		perror("madvise(MADV_PAGEOUT)");
		munmap(addr, bytes);
		return 2;
	}
	wait_for_pageout();

	if (writeback) {
		unsigned long long r_wb, w_wb;

		zram_writeback_idle(zram_sys);
		sleep(2);
		read_bd_stat(zram_sys, &r_wb, &w_wb);
		if (expect_bd_writes && w_wb <= bd_writes_before) {
			fprintf(stderr, "expected backing writes after writeback "
				"(before=%llu after_wb=%llu)\n",
				bd_writes_before, w_wb);
			munmap(addr, bytes);
			return 1;
		}
	}

	if (drop_caches) {
		sync();
		write_file("/proc/sys/vm/drop_caches", "3\n");
	}

	if (verify_pattern(addr, pages, stride, seed) != 0) {
		munmap(addr, bytes);
		return 1;
	}

	read_bd_stat(zram_sys, &bd_reads_after, &bd_writes_after);
	printf("bd_reads_before=%llu bd_reads_after=%llu reads_delta=%llu\n",
	       bd_reads_before, bd_reads_after,
	       bd_reads_after - bd_reads_before);
	printf("bd_writes_before=%llu bd_writes_after=%llu writes_delta=%llu\n",
	       bd_writes_before, bd_writes_after,
	       bd_writes_after - bd_writes_before);
	printf("notify_free=%llu mm_compr=%llu\n",
	       read_notify_free(zram_sys), read_mm_compr(zram_sys));

	if (expect_no_bd && bd_reads_after != bd_reads_before) {
		fprintf(stderr, "unexpected backing reads delta %llu\n",
			bd_reads_after - bd_reads_before);
		munmap(addr, bytes);
		return 1;
	}
	if (expect_bd_reads && bd_reads_after <= bd_reads_before) {
		fprintf(stderr, "expected backing reads but delta is 0\n");
		munmap(addr, bytes);
		return 1;
	}
	if (expect_bd_writes && bd_writes_after <= bd_writes_before) {
		fprintf(stderr, "expected backing writes but delta is 0\n");
		munmap(addr, bytes);
		return 1;
	}

	munmap(addr, bytes);
	return 0;
}

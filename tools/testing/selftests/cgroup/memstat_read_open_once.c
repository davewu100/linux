/*
 * Open a file once, then loop: lseek(0) + read().
 * This mirrors the "open-once" benchmark style used in mcg_test.
 *
 * Usage: memstat_read_open_once <path> [loops] [warmup]
 *   If warmup is given: do warmup reads (discard), then time "loops" reads and
 *   print average μs per read (for the timed loops only) to stdout. Used so
 *   the first read-after-open (cold) is not included in the average.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_LOOPS 100
#define BUF_SIZE 4096

static void do_reads(int fd, char *buf, size_t buf_size, long n)
{
	long i;
	for (i = 0; i < n; i++) {
		if (lseek(fd, 0, SEEK_SET) < 0) {
			perror("lseek");
			break;
		}
		(void)read(fd, buf, buf_size);
	}
}

int main(int argc, char **argv)
{
	const char *path;
	long loops = DEFAULT_LOOPS;
	long warmup = 0;
	char buf[BUF_SIZE];
	int fd;
	char *endp;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s <path> [loops] [warmup]\n", argv[0]);
		return 1;
	}

	path = argv[1];
	if (argc >= 3) {
		errno = 0;
		loops = strtol(argv[2], &endp, 10);
		if (errno || !endp || *endp != '\0' || loops <= 0) {
			fprintf(stderr, "Invalid loops: %s\n", argv[2]);
			return 1;
		}
	}
	if (argc >= 4) {
		errno = 0;
		warmup = strtol(argv[3], &endp, 10);
		if (errno || !endp || *endp != '\0' || warmup < 0) {
			fprintf(stderr, "Invalid warmup: %s\n", argv[3]);
			return 1;
		}
	}

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror(path);
		return 1;
	}

	if (warmup > 0) {
		do_reads(fd, buf, sizeof(buf), warmup);
		{
			struct timespec t0, t1;
			clock_gettime(CLOCK_MONOTONIC, &t0);
			do_reads(fd, buf, sizeof(buf), loops);
			clock_gettime(CLOCK_MONOTONIC, &t1);
			unsigned long long ns = (unsigned long long)(t1.tv_sec - t0.tv_sec) * 1000000000ULL
				+ (unsigned long long)(t1.tv_nsec - t0.tv_nsec);
			/* round to nearest μs: (ns/1000 + loops/2) / loops */
			unsigned long us = (unsigned long)((ns / 1000 + loops / 2) / loops);
			printf("%lu\n", us);
		}
	} else {
		do_reads(fd, buf, sizeof(buf), loops);
	}

	close(fd);
	return 0;
}

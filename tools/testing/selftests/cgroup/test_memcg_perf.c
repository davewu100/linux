// SPDX-License-Identifier: GPL-2.0
/*
 * Memory cgroup performance testing
 *
 * This test measures the performance of reading cgroup memory statistics
 * files, helping to evaluate different implementation approaches.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "cgroup_util.h"

#define TEST_ITERATIONS 100000
#define BUFFER_SIZE (16 * 1024)

static char *cgroup_path = NULL;
static char memory_stat_path[256];
static char memory_stat_atomic_path[256];
static char memory_numa_stat_path[256];
static char memory_numa_stat_atomic_path[256];

/* Helper: Get current time in nanoseconds */
static inline uint64_t get_time_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Test: open once, read N times (FD reuse), report latency */
static int test_read_perf(const char *path, const char *name, int iterations)
{
	uint64_t start, end, duration;
	char buf[BUFFER_SIZE];
	int fd, i;

	printf("Testing %s (%d iterations)...\n", name, iterations);
	
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror("open");
		return -1;
	}
	
	start = get_time_ns();
	for (i = 0; i < iterations; i++) {
		lseek(fd, 0, SEEK_SET);
		if (read(fd, buf, sizeof(buf) - 1) < 0) {
			perror("read");
			close(fd);
			return -1;
		}
	}
	end = get_time_ns();
	close(fd);
	
	duration = end - start;
	printf("  Total: %.3f ms\n", duration / 1000000.0);
	printf("  Average: %.3f us per read\n", duration / (double)iterations / 1000.0);
	printf("\n");
	
	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [options]\n", prog);
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "  --cgpath <path>   Cgroup path (default: /sys/fs/cgroup)\n");
	fprintf(stderr, "  --iterations <n>  Number of iterations (default: 100000)\n");
	fprintf(stderr, "  --help           Show this help\n");
}

int main(int argc, char *argv[])
{
	int iterations = TEST_ITERATIONS;
	int i;

	/* Parse arguments */
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--cgpath") == 0 && i + 1 < argc) {
			cgroup_path = argv[++i];
		} else if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
			iterations = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			return 0;
		} else {
			fprintf(stderr, "Unknown option: %s\n", argv[i]);
			usage(argv[0]);
			return 1;
		}
	}

	if (!cgroup_path)
		cgroup_path = "/sys/fs/cgroup";

	snprintf(memory_stat_path, sizeof(memory_stat_path),
		 "%s/memory.stat", cgroup_path);
	snprintf(memory_stat_atomic_path, sizeof(memory_stat_atomic_path),
		 "%s/memory.stat.atomic", cgroup_path);
	snprintf(memory_numa_stat_path, sizeof(memory_numa_stat_path),
		 "%s/memory.numa_stat", cgroup_path);
	snprintf(memory_numa_stat_atomic_path, sizeof(memory_numa_stat_atomic_path),
		 "%s/memory.numa_stat.atomic", cgroup_path);

	printf("=== Memory Cgroup Performance Test ===\n");
	printf("Cgroup: %s\n", cgroup_path);
	printf("Iterations: %d\n\n", iterations);

	/* Test memory.stat (rstat) */
	if (access(memory_stat_path, R_OK) == 0) {
		test_read_perf(memory_stat_path, "memory.stat (rstat)", iterations);
	} else {
		printf("memory.stat not available\n\n");
	}

	/* Test memory.stat.atomic (atomic counter) */
	if (access(memory_stat_atomic_path, R_OK) == 0) {
		test_read_perf(memory_stat_atomic_path, "memory.stat.atomic", iterations);
	} else {
		printf("memory.stat.atomic not available\n\n");
	}

	/* Test memory.numa_stat (rstat) */
	if (access(memory_numa_stat_path, R_OK) == 0) {
		test_read_perf(memory_numa_stat_path, "memory.numa_stat (rstat)", iterations);
	} else {
		printf("memory.numa_stat not available\n\n");
	}

	/* Test memory.numa_stat.atomic (atomic counter) */
	if (access(memory_numa_stat_atomic_path, R_OK) == 0) {
		test_read_perf(memory_numa_stat_atomic_path, "memory.numa_stat.atomic", iterations);
	} else {
		printf("memory.numa_stat.atomic not available\n\n");
	}

	printf("=== Test Complete ===\n");
	return 0;
}

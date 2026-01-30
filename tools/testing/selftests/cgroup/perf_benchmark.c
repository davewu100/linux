// SPDX-License-Identifier: GPL-2.0
/*
 * Generic function benchmark tool using func_profiler
 * 
 * Provides a framework for benchmarking any C function
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "func_profiler.h"

#define ITERATIONS 1000
#define BUFFER_SIZE 8192

/* Example benchmark functions */
static int bench_read_file(const char *path, char *buffer, size_t size)
{
	int fd, ret;
	
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	
	ret = read(fd, buffer, size - 1);
	if (ret > 0)
		buffer[ret] = '\0';
	
	close(fd);
	return ret;
}

static void bench_memory_stat_vs_ks(void)
{
	struct profile_stats stats_traditional, stats_kserial;
	char buffer[BUFFER_SIZE];
	int i, ret;
	
	printf("=== Benchmarking: memory.stat vs memory.stat.ks ===\n\n");
	
	/* Initialize */
	profile_init(&stats_traditional, "memory.stat");
	profile_init(&stats_kserial, "memory.stat.ks");
	
	/* Warmup */
	printf("Warming up...\n");
	for (i = 0; i < 10; i++) {
		bench_read_file("/sys/fs/cgroup/memory.stat", buffer, BUFFER_SIZE);
		bench_read_file("/sys/fs/cgroup/memory.stat.ks", buffer, BUFFER_SIZE);
	}
	
	printf("Running %d iterations...\n\n", ITERATIONS);
	
	/* Benchmark traditional */
	for (i = 0; i < ITERATIONS; i++) {
		PROFILE_START(traditional);
		ret = bench_read_file("/sys/fs/cgroup/memory.stat", buffer, BUFFER_SIZE);
		PROFILE_END(stats_traditional, traditional);
		
		if (ret < 0) {
			fprintf(stderr, "Error reading memory.stat\n");
			return;
		}
	}
	
	/* Benchmark kserial */
	for (i = 0; i < ITERATIONS; i++) {
		PROFILE_START(kserial);
		ret = bench_read_file("/sys/fs/cgroup/memory.stat.ks", buffer, BUFFER_SIZE);
		PROFILE_END(stats_kserial, kserial);
		
		if (ret < 0) {
			fprintf(stderr, "Error reading memory.stat.ks\n");
			return;
		}
	}
	
	/* Results */
	printf("=== Results ===\n\n");
	profile_print(&stats_traditional);
	profile_print(&stats_kserial);
	profile_compare(&stats_traditional, &stats_kserial, "memory.stat vs memory.stat.ks");
}

static void bench_multiple_files(const char **files, int nfiles, int iterations)
{
	struct profile_stats *stats;
	char buffer[BUFFER_SIZE];
	int i, j;
	
	printf("=== Benchmarking Multiple Files ===\n\n");
	printf("Files: %d\n", nfiles);
	printf("Iterations: %d\n\n", iterations);
	
	/* Allocate stats */
	stats = calloc(nfiles, sizeof(struct profile_stats));
	if (!stats) {
		fprintf(stderr, "Failed to allocate memory\n");
		return;
	}
	
	/* Initialize */
	for (i = 0; i < nfiles; i++) {
		profile_init(&stats[i], files[i]);
	}
	
	/* Warmup */
	printf("Warming up...\n");
	for (i = 0; i < nfiles; i++) {
		for (j = 0; j < 10; j++) {
			bench_read_file(files[i], buffer, BUFFER_SIZE);
		}
	}
	
	printf("Running benchmark...\n\n");
	
	/* Benchmark each file */
	for (i = 0; i < nfiles; i++) {
		for (j = 0; j < iterations; j++) {
			uint64_t start = get_timestamp_ns();
			bench_read_file(files[i], buffer, BUFFER_SIZE);
			uint64_t end = get_timestamp_ns();
			profile_record(&stats[i], start, end);
		}
	}
	
	/* Results */
	printf("=== Results ===\n\n");
	for (i = 0; i < nfiles; i++) {
		profile_print(&stats[i]);
	}
	
	/* Compare first two if available */
	if (nfiles >= 2) {
		printf("\n");
		profile_compare(&stats[0], &stats[1], "File Comparison");
	}
	
	free(stats);
}

static void print_usage(const char *prog)
{
	printf("Usage: %s [options]\n", prog);
	printf("\n");
	printf("Options:\n");
	printf("  -m, --memstat           Benchmark memory.stat vs memory.stat.ks\n");
	printf("  -f, --files <f1> <f2>   Benchmark multiple files\n");
	printf("  -n, --iterations <n>    Number of iterations (default: 1000)\n");
	printf("  -h, --help              Show this help\n");
	printf("\n");
	printf("Examples:\n");
	printf("  %s -m\n", prog);
	printf("  %s -f /proc/meminfo /proc/cpuinfo\n", prog);
	printf("  %s -m -n 10000\n", prog);
	printf("\n");
}

int main(int argc, char **argv)
{
	int iterations = ITERATIONS;
	int mode = 0; /* 0=memstat, 1=files */
	const char *files[10];
	int nfiles = 0;
	int i;
	
	if (argc < 2) {
		print_usage(argv[0]);
		return 1;
	}
	
	/* Parse arguments */
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--memstat") == 0) {
			mode = 0;
		} else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--files") == 0) {
			mode = 1;
			/* Collect files */
			i++;
			while (i < argc && argv[i][0] != '-') {
				if (nfiles < 10) {
					files[nfiles++] = argv[i];
				}
				i++;
			}
			i--;
		} else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--iterations") == 0) {
			if (i + 1 < argc) {
				iterations = atoi(argv[++i]);
			}
		} else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			print_usage(argv[0]);
			return 0;
		}
	}
	
	/* Run benchmark */
	if (mode == 0) {
		bench_memory_stat_vs_ks();
	} else if (mode == 1 && nfiles >= 2) {
		bench_multiple_files(files, nfiles, iterations);
	} else {
		fprintf(stderr, "Error: Invalid arguments\n");
		print_usage(argv[0]);
		return 1;
	}
	
	return 0;
}

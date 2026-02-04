// SPDX-License-Identifier: GPL-2.0
/*
 * Generic performance comparison tool using func_profiler
 * 
 * Usage: perf_compare <iterations> <cmd1> <cmd2>
 * Example: perf_compare 1000 "cat /sys/fs/cgroup/memory.stat" "cat /sys/fs/cgroup/memory.stat.ks"
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "func_profiler.h"

#define MAX_CMD_LEN 1024
#define BUFFER_SIZE 4096

static int run_command(const char *cmd, char *output, size_t output_size)
{
	FILE *fp;
	size_t len = 0;
	
	fp = popen(cmd, "r");
	if (!fp)
		return -1;
	
	if (output && output_size > 0) {
		len = fread(output, 1, output_size - 1, fp);
		if (len > 0)
			output[len] = '\0';
	}
	
	return pclose(fp);
}

static void print_usage(const char *prog)
{
	printf("Usage: %s <iterations> <cmd1> <cmd2>\n", prog);
	printf("\n");
	printf("Generic performance comparison tool\n");
	printf("\n");
	printf("Arguments:\n");
	printf("  iterations  Number of iterations to run\n");
	printf("  cmd1        Baseline command\n");
	printf("  cmd2        Optimized command to compare\n");
	printf("\n");
	printf("Examples:\n");
	printf("  %s 1000 \"cat /sys/fs/cgroup/memory.stat\" \"cat /sys/fs/cgroup/memory.stat.ks\"\n", prog);
	printf("  %s 100 \"ls -l /tmp\" \"ls -l /tmp | wc -l\"\n", prog);
	printf("\n");
}

int main(int argc, char **argv)
{
	struct profile_stats stats_cmd1, stats_cmd2;
	char *cmd1, *cmd2;
	char output[BUFFER_SIZE];
	int iterations, i, ret;
	
	if (argc != 4) {
		print_usage(argv[0]);
		return 1;
	}
	
	iterations = atoi(argv[1]);
	cmd1 = argv[2];
	cmd2 = argv[3];
	
	if (iterations <= 0 || iterations > 1000000) {
		fprintf(stderr, "Error: iterations must be between 1 and 1000000\n");
		return 1;
	}
	
	printf("=== Generic Performance Comparison ===\n\n");
	printf("Iterations: %d\n", iterations);
	printf("Command 1 (baseline): %s\n", cmd1);
	printf("Command 2 (optimized): %s\n\n", cmd2);
	
	/* Initialize profilers */
	profile_init(&stats_cmd1, "Command 1 (baseline)");
	profile_init(&stats_cmd2, "Command 2 (optimized)");
	
	/* Warmup */
	printf("Warming up...\n");
	for (i = 0; i < 10; i++) {
		run_command(cmd1, NULL, 0);
		run_command(cmd2, NULL, 0);
	}
	
	printf("Running benchmark...\n\n");
	
	/* Benchmark command 1 */
	for (i = 0; i < iterations; i++) {
		PROFILE_START(cmd1);
		ret = run_command(cmd1, (i == 0) ? output : NULL, BUFFER_SIZE);
		PROFILE_END(stats_cmd1, cmd1);
		
		if (ret != 0) {
			fprintf(stderr, "Warning: Command 1 returned non-zero: %d\n", ret);
		}
	}
	
	/* Benchmark command 2 */
	for (i = 0; i < iterations; i++) {
		PROFILE_START(cmd2);
		ret = run_command(cmd2, NULL, 0);
		PROFILE_END(stats_cmd2, cmd2);
		
		if (ret != 0) {
			fprintf(stderr, "Warning: Command 2 returned non-zero: %d\n", ret);
		}
	}
	
	/* Display results */
	printf("=== Benchmark Results ===\n\n");
	
	profile_print(&stats_cmd1);
	profile_print(&stats_cmd2);
	
	profile_compare(&stats_cmd1, &stats_cmd2, "Performance Comparison");
	
	printf("\n=== Sample Output ===\n\n");
	printf("Command 1 output (first 10 lines):\n");
	
	char *line = strtok(output, "\n");
	int lines = 0;
	while (line && lines < 10) {
		printf("  %s\n", line);
		line = strtok(NULL, "\n");
		lines++;
	}
	
	printf("\n=== Summary ===\n\n");
	printf("✅ Performance comparison completed successfully!\n");
	
	return 0;
}

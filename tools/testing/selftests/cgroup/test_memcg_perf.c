// SPDX-License-Identifier: GPL-2.0
/*
 * Memory cgroup performance testing
 *
 * This test measures the performance of reading cgroup memory statistics
 * files using different approaches, helping to evaluate the performance
 * impact of atomic counter vs rstat implementations.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "func_profiler.h"
#include "lib/include/cgroup_util.h"

#define TEST_ITERATIONS 1000
#define BUFFER_SIZE (16 * 1024)

static char *cgroup_path = NULL;
static char memory_stat_path[256];
static char memory_numa_stat_path[256];

/* Test functions with profiling */

static void read_memory_stat_once(void)
{
	PROFILE_FUNC();
	
	int fd = open(memory_stat_path, O_RDONLY);
	if (fd < 0) {
		perror("open memory.stat");
		return;
	}
	
	char buf[BUFFER_SIZE];
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	if (n < 0)
		perror("read memory.stat");
	
	close(fd);
}

static void read_memory_numa_stat_once(void)
{
	PROFILE_FUNC();
	
	int fd = open(memory_numa_stat_path, O_RDONLY);
	if (fd < 0) {
		// NUMA stat might not exist on all systems
		return;
	}
	
	char buf[BUFFER_SIZE];
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	if (n < 0)
		perror("read memory.numa_stat");
	
	close(fd);
}

static void open_close_only(void)
{
	PROFILE_FUNC_NAMED("open_close_only");
	
	int fd = open(memory_stat_path, O_RDONLY);
	if (fd >= 0)
		close(fd);
}

static void read_small_chunk(void)
{
	PROFILE_FUNC_NAMED("read_32_bytes");
	
	int fd = open(memory_stat_path, O_RDONLY);
	if (fd < 0)
		return;
	
	char buf[32];
	read(fd, buf, sizeof(buf));
	close(fd);
}

static void read_full_file(void)
{
	PROFILE_FUNC_NAMED("read_full_file");
	
	int fd = open(memory_stat_path, O_RDONLY);
	if (fd < 0)
		return;
	
	char buf[BUFFER_SIZE];
	ssize_t total = 0, n;
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		total += n;
	
	close(fd);
}

static void reuse_fd_multiple_reads(void)
{
	PROFILE_FUNC_NAMED("reuse_fd_100_reads");
	
	int fd = open(memory_stat_path, O_RDONLY);
	if (fd < 0)
		return;
	
	char buf[BUFFER_SIZE];
	for (int i = 0; i < 100; i++) {
		lseek(fd, 0, SEEK_SET);
		read(fd, buf, sizeof(buf));
	}
	
	close(fd);
}

/* Test scenarios */

static int test_sequential_reads(void)
{
	printf("Test 1: Sequential reads (%d iterations)...\n", TEST_ITERATIONS);
	
	PROFILE_ENABLE();
	PROFILE_RESET();
	
	for (int i = 0; i < TEST_ITERATIONS; i++)
		read_memory_stat_once();
	
	PROFILE_PRINT_STATS();
	return 0;
}

static int test_numa_stat_reads(void)
{
	printf("Test 2: NUMA stat reads (%d iterations)...\n", TEST_ITERATIONS);
	
	// Check if NUMA stat exists
	if (access(memory_numa_stat_path, R_OK) != 0) {
		printf("NUMA stat not available, skipping test\n");
		return 0;
	}
	
	PROFILE_RESET();
	
	for (int i = 0; i < TEST_ITERATIONS; i++)
		read_memory_numa_stat_once();
	
	PROFILE_PRINT_STATS();
	return 0;
}

static int test_mixed_workload(void)
{
	printf("Test 3: Mixed memory.stat and memory.numa_stat reads...\n");
	
	bool has_numa = (access(memory_numa_stat_path, R_OK) == 0);
	
	PROFILE_RESET();
	
	for (int i = 0; i < TEST_ITERATIONS; i++) {
		if (i % 2 == 0)
			read_memory_stat_once();
		else if (has_numa)
			read_memory_numa_stat_once();
	}
	
	PROFILE_PRINT_STATS();
	return 0;
}

static int test_granular_operations(void)
{
	printf("Test 4: Granular operation breakdown...\n");
	
	PROFILE_RESET();
	
	// Test open/close overhead
	for (int i = 0; i < 1000; i++)
		open_close_only();
	
	// Test small reads
	for (int i = 0; i < 1000; i++)
		read_small_chunk();
	
	// Test full file reads
	for (int i = 0; i < 1000; i++)
		read_full_file();
	
	PROFILE_PRINT_STATS();
	return 0;
}

static int test_fd_reuse(void)
{
	printf("Test 5: FD reuse vs repeated open/close...\n");
	
	PROFILE_RESET();
	
	// Test with FD reuse (100 reads with 1 open)
	for (int i = 0; i < 10; i++)
		reuse_fd_multiple_reads();
	
	// Test with repeated open/close (100 reads with 100 opens)
	PROFILE_FUNC_NAMED("open_each_time_100_reads");
	for (int j = 0; j < 100; j++)
		read_memory_stat_once();
	
	PROFILE_PRINT_STATS();
	return 0;
}

static int test_concurrent_reads(void)
{
	printf("Test 6: Concurrent reads (2 processes)...\n");
	
	PROFILE_RESET();
	
	pid_t pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	
	if (pid == 0) {
		// Child process
		PROFILE_FUNC_NAMED("child_process_reads");
		for (int i = 0; i < TEST_ITERATIONS / 2; i++)
			read_memory_stat_once();
		exit(0);
	} else {
		// Parent process
		PROFILE_FUNC_NAMED("parent_process_reads");
		for (int i = 0; i < TEST_ITERATIONS / 2; i++)
			read_memory_stat_once();
		
		int status;
		wait(&status);
	}
	
	PROFILE_PRINT_STATS();
	return 0;
}

static int test_burst_reads(void)
{
	printf("Test 7: Burst reads (10x100 iterations)...\n");
	
	PROFILE_RESET();
	
	for (int burst = 0; burst < 10; burst++) {
		for (int i = 0; i < 100; i++)
			read_memory_stat_once();
		
		// Small delay between bursts
		usleep(1000);
	}
	
	PROFILE_PRINT_STATS();
	return 0;
}

/* Setup and teardown */

static int setup_cgroup(void)
{
	cgroup_path = cg_name("", "memcg_perf_test");
	if (!cgroup_path) {
		fprintf(stderr, "Failed to generate cgroup name\n");
		return -1;
	}
	
	if (cg_create(cgroup_path)) {
		fprintf(stderr, "Failed to create cgroup: %s\n", cgroup_path);
		return -1;
	}
	
	snprintf(memory_stat_path, sizeof(memory_stat_path),
	         "%s/memory.stat", cgroup_path);
	snprintf(memory_numa_stat_path, sizeof(memory_numa_stat_path),
	         "%s/memory.numa_stat", cgroup_path);
	
	// Verify memory.stat exists
	if (access(memory_stat_path, R_OK) != 0) {
		fprintf(stderr, "Cannot access %s: %s\n",
		        memory_stat_path, strerror(errno));
		cg_destroy(cgroup_path);
		return -1;
	}
	
	printf("Created cgroup: %s\n", cgroup_path);
	printf("Testing file: %s\n", memory_stat_path);
	
	return 0;
}

static void cleanup_cgroup(void)
{
	if (cgroup_path) {
		cg_destroy(cgroup_path);
		free(cgroup_path);
	}
}

static void print_test_info(void)
{
	printf("\n");
	printf("======================================\n");
	printf(" Memory Cgroup Performance Test\n");
	printf("======================================\n");
	printf("\n");
	printf("This test measures the performance of reading cgroup memory\n");
	printf("statistics files (memory.stat, memory.numa_stat).\n");
	printf("\n");
	printf("Test iterations: %d\n", TEST_ITERATIONS);
	printf("\n");
	
	// Detect CPU frequency
	FILE* cpuinfo = fopen("/proc/cpuinfo", "r");
	if (cpuinfo) {
		char line[256];
		while (fgets(line, sizeof(line), cpuinfo)) {
			if (strstr(line, "model name")) {
				printf("CPU: %s", strchr(line, ':') + 2);
				break;
			}
		}
		fclose(cpuinfo);
	}
	
	// Check kernel config
	FILE* config = fopen("/proc/config.gz", "r");
	if (!config)
		config = fopen("/boot/config-" , "r");
	
	printf("\nKernel configuration:\n");
	
	// Read from sysfs instead
	FILE* memcg_rstat = fopen("/sys/module/memcontrol/parameters/memcg_rstat_counter", "r");
	if (memcg_rstat) {
		char value[8];
		if (fgets(value, sizeof(value), memcg_rstat)) {
			printf("  MEMCG_RSTAT_COUNTER: %s", value);
		}
		fclose(memcg_rstat);
	}
	
	FILE* memcg_atomic = fopen("/sys/module/memcontrol/parameters/memcg_atomic_counter", "r");
	if (memcg_atomic) {
		char value[8];
		if (fgets(value, sizeof(value), memcg_atomic)) {
			printf("  MEMCG_ATOMIC_COUNTER: %s", value);
		}
		fclose(memcg_atomic);
	}
	
	printf("\n======================================\n\n");
}

int main(int argc, char **argv)
{
	int ret = 0;
	
	print_test_info();
	
	// Initialize profiler
	func_profiler_init();
	
	// Setup test cgroup
	if (setup_cgroup() != 0) {
		fprintf(stderr, "Failed to setup test environment\n");
		return 1;
	}
	
	// Run all tests
	ret |= test_sequential_reads();
	ret |= test_numa_stat_reads();
	ret |= test_mixed_workload();
	ret |= test_granular_operations();
	ret |= test_fd_reuse();
	ret |= test_concurrent_reads();
	ret |= test_burst_reads();
	
	// Export results to CSV
	func_profiler_export_csv("memcg_perf_results.csv");
	
	// Cleanup
	cleanup_cgroup();
	func_profiler_cleanup();
	
	printf("\n");
	if (ret == 0)
		printf("All tests completed successfully!\n");
	else
		printf("Some tests failed (ret=%d)\n", ret);
	
	return ret;
}

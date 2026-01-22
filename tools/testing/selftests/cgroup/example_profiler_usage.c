// SPDX-License-Identifier: GPL-2.0
/*
 * Simple example of using the API profiler
 *
 * This demonstrates the basic usage of the profiler library
 * for measuring cgroup file access performance.
 */

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include "func_profiler.h"

// Example 1: Auto-profiled function with automatic naming
void read_cgroup_stat(const char *path)
{
	PROFILE_FUNC();  // Automatically names this "read_cgroup_stat"

	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror("open");
		return;
	}

	char buf[4096];
	read(fd, buf, sizeof(buf));
	close(fd);
}

// Example 2: Custom profiling name
void internal_helper_function(const char *path)
{
	PROFILE_FUNC_NAMED("cgroup_stat_helper");  // Custom name

	int fd = open(path, O_RDONLY);
	if (fd >= 0) {
		char buf[1024];
		read(fd, buf, sizeof(buf));
		close(fd);
	}
}

// Example 3: Fixed ID for cross-run comparison
void critical_operation(void)
{
	PROFILE_FUNC_ID(100);  // Fixed ID = 100

	// Some operation you want to consistently measure
	usleep(100);
}

int main(void)
{
	printf("Function Profiler Usage Example\n");
	printf("===========================\n\n");

	// Step 1: Initialize the profiler
	func_profiler_init();

	// Step 2: Enable profiling
	PROFILE_ENABLE();

	// Step 3: Run your code
	printf("Running 1000 iterations...\n");
	for (int i = 0; i < 1000; i++) {
		read_cgroup_stat("/sys/fs/cgroup/memory.stat");

		if (i % 2 == 0)
			internal_helper_function("/sys/fs/cgroup/memory.stat");

		if (i % 10 == 0)
			critical_operation();
	}

	// Step 4: Print statistics
	PROFILE_PRINT_STATS();

	// Step 5: Optional - export to CSV
	func_profiler_export_csv("example_results.csv");

	// Step 6: Optional - reset and run again
	printf("\nResetting and running 500 more iterations...\n");
	PROFILE_RESET();

	for (int i = 0; i < 500; i++) {
		read_cgroup_stat("/sys/fs/cgroup/memory.stat");
	}

	PROFILE_PRINT_STATS();

	// Step 7: Cleanup
	func_profiler_cleanup();

	printf("\nExample completed!\n");
	return 0;
}

// SPDX-License-Identifier: GPL-2.0
/*
 * Function profiler - ported from atomic_counter_impl_v5
 * 
 * High-precision profiling for performance comparison
 */

#ifndef _FUNC_PROFILER_H
#define _FUNC_PROFILER_H

#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Profile statistics */
struct profile_stats {
	const char *name;
	uint64_t count;
	uint64_t total_ns;
	uint64_t min_ns;
	uint64_t max_ns;
};

/* Get timestamp in nanoseconds */
static inline uint64_t get_timestamp_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Initialize profile stats */
static inline void profile_init(struct profile_stats *stats, const char *name)
{
	memset(stats, 0, sizeof(*stats));
	stats->name = name;
	stats->min_ns = UINT64_MAX;
}

/* Record one sample */
static inline void profile_record(struct profile_stats *stats, uint64_t start_ns, uint64_t end_ns)
{
	uint64_t elapsed = end_ns - start_ns;
	
	stats->count++;
	stats->total_ns += elapsed;
	
	if (elapsed < stats->min_ns)
		stats->min_ns = elapsed;
	
	if (elapsed > stats->max_ns)
		stats->max_ns = elapsed;
}

/* Print profile results */
static inline void profile_print(const struct profile_stats *stats)
{
	if (stats->count == 0) {
		printf("%-30s: No samples\n", stats->name);
		return;
	}
	
	uint64_t avg_ns = stats->total_ns / stats->count;
	
	printf("%-30s: count=%lu, avg=%.2f μs, min=%.2f μs, max=%.2f μs, total=%.2f ms\n",
	       stats->name,
	       stats->count,
	       avg_ns / 1000.0,
	       stats->min_ns / 1000.0,
	       stats->max_ns / 1000.0,
	       stats->total_ns / 1000000.0);
}

/* Compare two profile stats (for speedup calculation) */
static inline void profile_compare(const struct profile_stats *baseline,
				   const struct profile_stats *optimized,
				   const char *comparison_name)
{
	if (baseline->count == 0 || optimized->count == 0) {
		printf("%-30s: Cannot compare (no samples)\n", comparison_name);
		return;
	}
	
	uint64_t baseline_avg = baseline->total_ns / baseline->count;
	uint64_t optimized_avg = optimized->total_ns / optimized->count;
	
	double speedup = (double)baseline_avg / optimized_avg;
	int64_t improvement_ns = baseline_avg - optimized_avg;
	
	printf("\n=== %s ===\n", comparison_name);
	printf("Baseline (%s):\n", baseline->name);
	printf("  Average: %.2f μs\n", baseline_avg / 1000.0);
	
	printf("Optimized (%s):\n", optimized->name);
	printf("  Average: %.2f μs\n", optimized_avg / 1000.0);
	
	printf("Improvement:\n");
	printf("  Speedup: %.2fx faster\n", speedup);
	printf("  Time saved: %.2f μs (%.1f%% reduction)\n",
	       improvement_ns / 1000.0,
	       (improvement_ns * 100.0) / baseline_avg);
	
	if (speedup > 10.0) {
		printf("  🚀 Excellent! More than 10x faster!\n");
	} else if (speedup > 5.0) {
		printf("  ✨ Great! More than 5x faster!\n");
	} else if (speedup > 2.0) {
		printf("  ✅ Good! More than 2x faster!\n");
	} else if (speedup > 1.0) {
		printf("  ⚡ Faster\n");
	} else {
		printf("  ⚠️  Slower or same\n");
	}
}

/* Macro for easy profiling */
#define PROFILE_START(name) \
	uint64_t _profile_start_##name = get_timestamp_ns()

#define PROFILE_END(stats, name) \
	profile_record(&stats, _profile_start_##name, get_timestamp_ns())

#endif /* _FUNC_PROFILER_H */

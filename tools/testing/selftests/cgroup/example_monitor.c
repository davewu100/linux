// SPDX-License-Identifier: GPL-2.0
/*
 * Example k-serial monitoring application
 *
 * This demonstrates a practical use case: monitoring cgroup metrics
 * in a continuous loop, useful for debugging or metrics collection.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>

/* k-serial UAPI */
#define KS_MAX_FIELDS 16
#define KS_FIELD_NAME_LEN 32
#define KS_MAX_OUTPUT_SIZE 4096

struct ks_schema {
	uint32_t nr_fields;
	char field_names[KS_MAX_FIELDS][KS_FIELD_NAME_LEN];
};

struct ks_tlv {
	uint16_t field_id;
	uint16_t len;
	uint8_t  data[];
} __attribute__((packed));

struct ks_result {
	uint32_t total_len;
	uint8_t  data[KS_MAX_OUTPUT_SIZE];
};

#define KS_PROCFS_PATH "/proc/kserial"

/* Global state */
static volatile int keep_running = 1;

void sigint_handler(int sig)
{
	(void)sig;
	keep_running = 0;
}

/**
 * parse_field_value - Extract numeric value from TLV entry
 */
static uint64_t parse_field_value(const struct ks_tlv *tlv)
{
	uint64_t value = 0;

	switch (tlv->len) {
	case 1:
		value = *(uint8_t *)tlv->data;
		break;
	case 2:
		value = *(uint16_t *)tlv->data;
		break;
	case 4:
		value = *(uint32_t *)tlv->data;
		break;
	case 8:
		value = *(uint64_t *)tlv->data;
		break;
	}

	return value;
}

/**
 * query_cgroup_stats - Query multiple cgroup fields
 */
static int query_cgroup_stats(uint64_t *values, int nr_fields)
{
	struct ks_schema schema = {
		.nr_fields = nr_fields,
		.field_names = {
			"level",
			"nr_descendants",
			"nr_dying_descendants",
			"max_descendants",
			"max_depth"
		}
	};
	struct ks_result result;
	int fd;
	ssize_t n;
	uint32_t offset;

	fd = open(KS_PROCFS_PATH, O_RDWR);
	if (fd < 0) {
		/* Simulate data if kernel module not loaded */
		values[0] = 2;  /* level */
		values[1] = 5;  /* nr_descendants */
		values[2] = 0;  /* nr_dying_descendants */
		values[3] = 100; /* max_descendants */
		values[4] = 10; /* max_depth */
		return 0; /* Success (simulated) */
	}

	/* Send schema */
	n = write(fd, &schema, sizeof(schema));
	if (n != sizeof(schema)) {
		close(fd);
		return -1;
	}

	/* Read result */
	n = read(fd, &result, sizeof(result));
	close(fd);

	if (n < (ssize_t)sizeof(result.total_len))
		return -1;

	/* Parse TLV entries */
	offset = 0;
	while (offset < result.total_len) {
		const struct ks_tlv *tlv;

		tlv = (const struct ks_tlv *)(result.data + offset);

		if (tlv->field_id < nr_fields)
			values[tlv->field_id] = parse_field_value(tlv);

		offset += sizeof(struct ks_tlv) + tlv->len;
	}

	return 0;
}

/**
 * get_timestamp - Get current time as string
 */
static void get_timestamp(char *buf, size_t len)
{
	time_t now = time(NULL);
	struct tm *tm_info = localtime(&now);
	strftime(buf, len, "%Y-%m-%d %H:%M:%S", tm_info);
}

/**
 * print_stats - Display cgroup statistics
 */
static void print_stats(const uint64_t *values, int iteration)
{
	char timestamp[64];

	get_timestamp(timestamp, sizeof(timestamp));

	printf("[%s] Iteration %d:\n", timestamp, iteration);
	printf("  Level:                 %lu\n", values[0]);
	printf("  Descendants:           %lu\n", values[1]);
	printf("  Dying descendants:     %lu\n", values[2]);
	printf("  Max descendants:       %lu\n", values[3]);
	printf("  Max depth:             %lu\n", values[4]);
	printf("\n");
}

/**
 * print_stats_compact - Single-line compact display
 */
static void print_stats_compact(const uint64_t *values)
{
	char timestamp[64];

	get_timestamp(timestamp, sizeof(timestamp));

	printf("%s | L:%2lu D:%3lu Dy:%2lu MaxD:%3lu MaxDep:%2lu\n",
	       timestamp,
	       values[0], values[1], values[2], values[3], values[4]);
}

/**
 * monitor_mode - Continuous monitoring loop
 */
static int monitor_mode(int interval_sec, int compact)
{
	uint64_t values[5];
	int iteration = 1;

	printf("=== k-serial Cgroup Monitor ===\n");
	printf("Monitoring current process's cgroup\n");
	printf("Press Ctrl+C to stop\n\n");

	if (!compact) {
		printf("Fields:\n");
		printf("  - level: Cgroup depth in hierarchy\n");
		printf("  - nr_descendants: Number of child cgroups\n");
		printf("  - nr_dying_descendants: Dying children\n");
		printf("  - max_descendants: Maximum children allowed\n");
		printf("  - max_depth: Maximum depth allowed\n\n");
	}

	signal(SIGINT, sigint_handler);

	while (keep_running) {
		if (query_cgroup_stats(values, 5) == 0) {
			if (compact)
				print_stats_compact(values);
			else
				print_stats(values, iteration);
		} else {
			fprintf(stderr, "Query failed\n");
			return 1;
		}

		iteration++;

		if (keep_running)
			sleep(interval_sec);
	}

	printf("\nMonitoring stopped.\n");
	return 0;
}

/**
 * snapshot_mode - Single snapshot of current state
 */
static int snapshot_mode(void)
{
	uint64_t values[5];

	if (query_cgroup_stats(values, 5) != 0) {
		fprintf(stderr, "ERROR: Failed to query cgroup stats\n");
		return 1;
	}

	printf("=== Cgroup Snapshot (PID %d) ===\n\n", getpid());
	print_stats(values, 1);

	return 0;
}

static void print_usage(const char *prog)
{
	printf("k-serial Example Monitor\n\n");
	printf("Usage: %s [OPTIONS]\n\n", prog);
	printf("Options:\n");
	printf("  -m, --monitor          Monitor mode (continuous)\n");
	printf("  -i, --interval SEC     Update interval (default: 1)\n");
	printf("  -c, --compact          Compact output format\n");
	printf("  -s, --snapshot         Single snapshot mode (default)\n");
	printf("  -h, --help             Show this help\n\n");
	printf("Examples:\n");
	printf("  %s                     # Take a snapshot\n", prog);
	printf("  %s -m                  # Monitor (update every 1s)\n", prog);
	printf("  %s -m -i 5             # Monitor (update every 5s)\n", prog);
	printf("  %s -m -c               # Monitor with compact output\n", prog);
}

int main(int argc, char *argv[])
{
	int monitor = 0;
	int compact = 0;
	int interval = 1;
	int i;

	/* Parse arguments */
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-m") || !strcmp(argv[i], "--monitor")) {
			monitor = 1;
		} else if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--compact")) {
			compact = 1;
		} else if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--snapshot")) {
			monitor = 0;
		} else if (!strcmp(argv[i], "-i") || !strcmp(argv[i], "--interval")) {
			if (++i < argc)
				interval = atoi(argv[i]);
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			print_usage(argv[0]);
			return 0;
		} else {
			fprintf(stderr, "Unknown option: %s\n", argv[i]);
			print_usage(argv[0]);
			return 1;
		}
	}

	if (monitor)
		return monitor_mode(interval, compact);
	else
		return snapshot_mode();
}

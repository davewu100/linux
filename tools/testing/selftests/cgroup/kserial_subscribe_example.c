// SPDX-License-Identifier: GPL-2.0
/*
 * kserial Subscribe-Publish Mode Example
 *
 * Demonstrates stateful high-performance queries using subscribe/read pattern.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <time.h>

/* Kernel API definitions */
#define KS_IOCTL_MAGIC 'k'
#define KS_IOCTL_SUBSCRIBE _IOW(KS_IOCTL_MAGIC, 1, struct ks_subscribe)
#define KS_IOCTL_UNSUBSCRIBE _IO(KS_IOCTL_MAGIC, 2)

struct ks_subscribe {
	char struct_name[64];
	char fields[32][128];
	uint32_t nr_fields;
	uint32_t pid;
	uint32_t flags;
	uint8_t include_descriptor;
	uint8_t reserved[3];
};

/* Measure time in microseconds */
static uint64_t get_time_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* Example 1: Subscribe + First Read (with descriptor) + Subsequent Reads */
void example_subscribe_with_descriptor(void)
{
	int fd, ret, i;
	struct ks_subscribe sub = {0};
	char buf[4096];
	uint64_t start, end;

	printf("=== Example 1: Subscribe with Descriptor ===\n\n");

	/* Open kserial device */
	fd = open("/proc/kserial", O_RDWR);
	if (fd < 0) {
		perror("open");
		return;
	}

	/* Setup subscription */
	strncpy(sub.struct_name, "mem_cgroup", sizeof(sub.struct_name) - 1);
	strncpy(sub.fields[0], "vmstats", sizeof(sub.fields[0]) - 1);
	strncpy(sub.fields[1], "vmstats", sizeof(sub.fields[1]) - 1);
	sub.nr_fields = 2;
	sub.pid = 0;  /* Current process */
	sub.include_descriptor = 1;  /* Include descriptor in first read */

	/* Subscribe (ioctl) */
	start = get_time_us();
	ret = ioctl(fd, KS_IOCTL_SUBSCRIBE, &sub);
	end = get_time_us();

	if (ret < 0) {
		perror("ioctl(KS_IOCTL_SUBSCRIBE)");
		close(fd);
		return;
	}

	printf("Subscribe: %lu μs\n", end - start);
	printf("  struct: %s\n", sub.struct_name);
	printf("  fields: %u\n", sub.nr_fields);
	printf("\n");

	/* First read (includes descriptor) */
	start = get_time_us();
	ret = read(fd, buf, sizeof(buf));
	end = get_time_us();

	if (ret < 0) {
		perror("read (first)");
		close(fd);
		return;
	}

	printf("First read (with descriptor): %lu μs\n", end - start);
	printf("  Received: %d bytes\n", ret);

	/* Parse descriptor */
	if (ret >= 8) {
		uint32_t *desc_hdr = (uint32_t *)buf;
		if (desc_hdr[0] == 0xDE5C0000) {  /* DESC magic */
			printf("  Descriptor magic: 0x%X\n", desc_hdr[0]);
			printf("  Number of fields: %u\n", desc_hdr[1]);

			/* Field descriptors */
			for (i = 0; i < desc_hdr[1] && i < 2; i++) {
				uint32_t *field_desc = (uint32_t *)(buf + 8 + i * 12);
				printf("  Field %d: offset=%u, size=%u, type_id=%u\n",
				       i, field_desc[0], field_desc[1], field_desc[2]);
			}
		}
	}
	printf("\n");

	/* Disable descriptor for subsequent reads */
	sub.include_descriptor = 0;
	ret = ioctl(fd, KS_IOCTL_SUBSCRIBE, &sub);
	if (ret < 0) {
		perror("ioctl(KS_IOCTL_SUBSCRIBE) - disable descriptor");
		close(fd);
		return;
	}

	/* Subsequent reads (raw payload only) */
	printf("Subsequent reads (raw payload):\n");
	uint64_t total_time = 0;
	int num_reads = 10;

	for (i = 0; i < num_reads; i++) {
		start = get_time_us();
		ret = read(fd, buf, sizeof(buf));
		end = get_time_us();

		if (ret < 0) {
			perror("read");
			break;
		}

		total_time += (end - start);

		if (i == 0) {
			printf("  Read %d: %lu μs, %d bytes\n", i, end - start, ret);
		}
	}

	printf("  ...\n");
	printf("  Average: %lu μs over %d reads\n", total_time / num_reads, num_reads);
	printf("\n");

	close(fd);
}

/* Example 2: High-frequency monitoring (1000 reads) */
void example_high_frequency_monitoring(void)
{
	int fd, ret, i;
	struct ks_subscribe sub = {0};
	char buf[4096];
	uint64_t start, end, total;
	int num_reads = 1000;

	printf("=== Example 2: High-Frequency Monitoring ===\n\n");

	fd = open("/proc/kserial", O_RDWR);
	if (fd < 0) {
		perror("open");
		return;
	}

	/* Subscribe to 2 fields */
	strncpy(sub.struct_name, "mem_cgroup", sizeof(sub.struct_name) - 1);
	strncpy(sub.fields[0], "vmstats", sizeof(sub.fields[0]) - 1);
	strncpy(sub.fields[1], "vmstats", sizeof(sub.fields[1]) - 1);
	sub.nr_fields = 2;
	sub.pid = 0;
	sub.include_descriptor = 0;  /* Skip descriptor */

	start = get_time_us();
	ret = ioctl(fd, KS_IOCTL_SUBSCRIBE, &sub);
	if (ret < 0) {
		perror("ioctl(KS_IOCTL_SUBSCRIBE)");
		close(fd);
		return;
	}
	end = get_time_us();

	printf("Setup (subscribe): %lu μs\n", end - start);

	/* Perform 1000 reads */
	start = get_time_us();
	for (i = 0; i < num_reads; i++) {
		ret = read(fd, buf, sizeof(buf));
		if (ret < 0) {
			perror("read");
			break;
		}
	}
	end = get_time_us();
	total = end - start;

	printf("Total time for %d reads: %lu μs (%.2f ms)\n",
	       num_reads, total, total / 1000.0);
	printf("Average per read: %.2f μs\n", (double)total / num_reads);
	printf("Throughput: %.0f reads/sec\n", num_reads * 1000000.0 / total);
	printf("\n");

	close(fd);
}

/* Example 3: Compare with legacy mode */
void example_compare_modes(void)
{
	printf("=== Example 3: Performance Comparison ===\n\n");

	/* TODO: Implement legacy mode comparison */
	printf("Subscribe mode demonstrated above.\n");
	printf("Legacy mode (write+read) would require:\n");
	printf("  - open() for each query\n");
	printf("  - write() schema\n");
	printf("  - read() result\n");
	printf("  - close()\n");
	printf("  = 4 syscalls vs 1 syscall in subscribe mode\n");
	printf("\n");
}

int main(int argc, char *argv[])
{
	printf("kserial Subscribe-Publish Mode Examples\n");
	printf("========================================\n\n");

	example_subscribe_with_descriptor();
	example_high_frequency_monitoring();
	example_compare_modes();

	printf("Done!\n");
	return 0;
}

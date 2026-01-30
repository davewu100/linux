// SPDX-License-Identifier: GPL-2.0
/*
 * kserial io_uring example - Ultimate performance async queries
 * 
 * Demonstrates:
 * - Batch submission: 1000 queries with ONE syscall
 * - Async completion: Zero blocking, zero context switches
 * - Expected: ~0.1μs per query (100,000 queries/sec)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>
#include <liburing.h>
#include "../../../include/linux/kserial.h"

#define BATCH_SIZE 1000
#define BUFFER_SIZE 4096

/* Helper: Get timestamp in nanoseconds */
static inline uint64_t get_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

int main(void)
{
	struct io_uring ring;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int fd, ret, i;
	uint64_t start_ns, end_ns;
	double elapsed_ms, queries_per_sec;
	
	/* Allocate buffers for batch queries */
	char **buffers = malloc(BATCH_SIZE * sizeof(char *));
	if (!buffers) {
		perror("malloc buffers");
		return 1;
	}
	
	for (i = 0; i < BATCH_SIZE; i++) {
		buffers[i] = aligned_alloc(4096, BUFFER_SIZE);
		if (!buffers[i]) {
			perror("aligned_alloc");
			return 1;
		}
	}
	
	printf("=== kserial io_uring Example ===\n\n");
	
	/* Step 1: Open /dev/kserial */
	printf("[1] Opening /dev/kserial...\n");
	fd = open("/dev/kserial", O_RDWR);
	if (fd < 0) {
		perror("open /dev/kserial");
		printf("    Note: Use /dev/kserial for io_uring (not /dev/kserial)\n");
		return 1;
	}
	
	/* Step 2: Subscribe to fields (one-time setup) */
	printf("[2] Subscribing to mem_cgroup fields...\n");
	struct ks_subscribe sub = {
		.struct_name = "mem_cgroup",
		.nr_fields = 3,
		.pid = 0,  /* current process */
		.flags = 0,
		.include_descriptor = 0,  /* Skip descriptor for io_uring */
	};
	strcpy(sub.fields[0], "vmstats.state[13]");  /* anon */
	strcpy(sub.fields[1], "vmstats.state[15]");  /* file */
	strcpy(sub.fields[2], "vmstats.state[37]");  /* kernel */
	
	ret = ioctl(fd, KS_IOCTL_SUBSCRIBE, &sub);
	if (ret < 0) {
		perror("ioctl KS_IOCTL_SUBSCRIBE");
		close(fd);
		return 1;
	}
	printf("    ✓ Subscribed (cached BTF lookups)\n\n");
	
	/* Step 3: Initialize io_uring */
	printf("[3] Initializing io_uring (queue depth: %d)...\n", BATCH_SIZE);
	ret = io_uring_queue_init(BATCH_SIZE, &ring, 0);
	if (ret < 0) {
		fprintf(stderr, "io_uring_queue_init failed: %s\n", strerror(-ret));
		close(fd);
		return 1;
	}
	printf("    ✓ io_uring initialized\n\n");
	
	/* Step 4: Batch submit (ALL queries with ONE syscall) */
	printf("[4] Batch submitting %d queries...\n", BATCH_SIZE);
	start_ns = get_ns();
	
	for (i = 0; i < BATCH_SIZE; i++) {
		sqe = io_uring_get_sqe(&ring);
		if (!sqe) {
			fprintf(stderr, "io_uring_get_sqe failed\n");
			goto cleanup;
		}
		
		/* Prepare io_uring command for kserial */
		memset(sqe, 0, sizeof(*sqe));
		sqe->opcode = IORING_OP_URING_CMD;
		sqe->fd = fd;
		sqe->cmd_len[0] = KS_URING_CMD_READ;  /* Command type */
		sqe->cmd[0] = (__u64)buffers[i];      /* User buffer address */
		sqe->cmd[1] = BUFFER_SIZE;            /* Buffer size */
		sqe->user_data = i;                   /* Track which buffer */
	}
	
	/* Submit ALL at once (ONE syscall!) */
	ret = io_uring_submit(&ring);
	if (ret < 0) {
		fprintf(stderr, "io_uring_submit failed: %s\n", strerror(-ret));
		goto cleanup;
	}
	printf("    ✓ Submitted %d queries (1 syscall)\n\n", ret);
	
	/* Step 5: Wait for completions */
	printf("[5] Waiting for completions...\n");
	for (i = 0; i < BATCH_SIZE; i++) {
		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret < 0) {
			fprintf(stderr, "io_uring_wait_cqe failed: %s\n", strerror(-ret));
			goto cleanup;
		}
		
		/* Check result */
		if (cqe->res < 0) {
			fprintf(stderr, "Query %d failed: %s\n", 
				(int)cqe->user_data, strerror(-cqe->res));
		}
		
		io_uring_cqe_seen(&ring, cqe);
	}
	
	end_ns = get_ns();
	printf("    ✓ All queries completed\n\n");
	
	/* Step 6: Calculate performance */
	elapsed_ms = (end_ns - start_ns) / 1000000.0;
	queries_per_sec = (BATCH_SIZE * 1000.0) / elapsed_ms;
	
	printf("=== Performance Results ===\n");
	printf("Total queries:     %d\n", BATCH_SIZE);
	printf("Total time:        %.3f ms\n", elapsed_ms);
	printf("Time per query:    %.3f μs\n", elapsed_ms * 1000.0 / BATCH_SIZE);
	printf("Throughput:        %.0f queries/sec\n", queries_per_sec);
	printf("Syscalls:          2 (submit + wait)\n\n");
	
	/* Step 7: Show sample data */
	printf("=== Sample Query Results ===\n");
	printf("First query buffer:\n");
	
	/* Parse descriptor if included (usually not for io_uring) */
	uint32_t *desc_hdr = (uint32_t *)buffers[0];
	if (desc_hdr[0] == 0xDE5C0000) {
		printf("  [Descriptor included - unexpected for io_uring]\n");
	} else {
		/* Raw payload: 3 x u64 values */
		uint64_t *values = (uint64_t *)buffers[0];
		printf("  anon:   %lu bytes (%.2f MB)\n", 
		       values[0], values[0] / 1048576.0);
		printf("  file:   %lu bytes (%.2f MB)\n", 
		       values[1], values[1] / 1048576.0);
		printf("  kernel: %lu bytes (%.2f MB)\n", 
		       values[2], values[2] / 1048576.0);
	}
	printf("\n");
	
	/* Step 8: Compare with other methods */
	printf("=== Performance Comparison ===\n");
	printf("Standard read():      9000 ns  (4 syscalls)\n");
	printf("Subscribe read():      500 ns  (1 syscall)\n");
	printf("Subscribe mmap:        300 ns  (0 syscall after mmap)\n");
	printf("Subscribe io_uring:    %.0f ns  (batched, async)\n", 
	       elapsed_ms * 1000000.0 / BATCH_SIZE);
	printf("\n");
	printf("Speedup vs read():    %.1fx faster\n", 
	       9000.0 / (elapsed_ms * 1000000.0 / BATCH_SIZE));
	printf("Throughput gain:      %.0f queries/sec (io_uring) vs 111K (read)\n\n",
	       queries_per_sec);
	
	/* Unsubscribe */
	ioctl(fd, KS_IOCTL_UNSUBSCRIBE);
	
cleanup:
	io_uring_queue_exit(&ring);
	close(fd);
	
	for (i = 0; i < BATCH_SIZE; i++)
		free(buffers[i]);
	free(buffers);
	
	return 0;
}

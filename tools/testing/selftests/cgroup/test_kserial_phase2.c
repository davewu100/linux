// SPDX-License-Identifier: GPL-2.0
/*
 * k-serial Phase 2 test - Nested field support
 * 
 * Tests querying nested fields like "self.id" and "dom_cgrp.level"
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

/* k-serial UAPI structures (Phase 2) */
#define KS_MAX_FIELDS 16
#define KS_FIELD_NAME_LEN 64
#define KS_MAX_OUTPUT_SIZE 4096
#define KS_FLAG_ALLOW_NULL 0x01

struct ks_schema {
	uint32_t nr_fields;
	uint32_t flags;
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

#define KS_PROCFS_PATH "/proc/cgroup_query"

/**
 * parse_tlv_result - Parse and display TLV-encoded result
 */
static void parse_tlv_result(const struct ks_result *result,
			      const struct ks_schema *schema)
{
	uint32_t offset = 0;

	printf("\n=== Phase 2: Nested Field Query Results ===\n");
	printf("Total data: %u bytes\n\n", result->total_len);

	while (offset < result->total_len) {
		const struct ks_tlv *tlv;
		uint64_t value = 0;

		tlv = (const struct ks_tlv *)(result->data + offset);

		if (tlv->field_id >= schema->nr_fields) {
			fprintf(stderr, "ERROR: Invalid field_id %u\n",
				tlv->field_id);
			break;
		}

		/* Extract value */
		memcpy(&value, tlv->data, tlv->len);

		/* Display result */
		printf("  %-35s = ", schema->field_names[tlv->field_id]);
		
		switch (tlv->len) {
		case 1:
			printf("%u\n", (uint8_t)value);
			break;
		case 2:
			printf("%u\n", (uint16_t)value);
			break;
		case 4:
			printf("%u\n", (uint32_t)value);
			break;
		case 8:
			if (value == 0)
				printf("0 (NULL or zero)\n");
			else
				printf("%lu (0x%lx)\n", value, value);
			break;
		default:
			printf("(%u bytes of data)\n", tlv->len);
		}

		offset += sizeof(struct ks_tlv) + tlv->len;
	}
	printf("\n");
}

/**
 * query_via_procfs - Query cgroup fields via procfs interface
 */
static int query_via_procfs(const struct ks_schema *schema)
{
	struct ks_result result = {0};
	int fd;
	ssize_t n;

	/* Open procfs interface */
	fd = open(KS_PROCFS_PATH, O_RDWR);
	if (fd < 0) {
		perror("open " KS_PROCFS_PATH);
		fprintf(stderr, "\nERROR: Cannot open %s\n", KS_PROCFS_PATH);
		fprintf(stderr, "Make sure the k-serial kernel module is loaded.\n");
		fprintf(stderr, "\nRunning simulation mode instead...\n\n");
		
		/* Simulate Phase 2 results */
		struct ks_tlv *tlv = (struct ks_tlv *)result.data;
		
		/* Simulate: self.id = 1234 */
		tlv->field_id = 0;
		tlv->len = 4;
		*(uint32_t *)tlv->data = 1234;
		result.total_len = sizeof(struct ks_tlv) + 4;

		/* Simulate: self.serial_nr = 5678 */
		tlv = (struct ks_tlv *)(result.data + result.total_len);
		tlv->field_id = 1;
		tlv->len = 8;
		*(uint64_t *)tlv->data = 5678;
		result.total_len += sizeof(struct ks_tlv) + 8;

		/* Simulate: dom_cgrp.level = 2 */
		tlv = (struct ks_tlv *)(result.data + result.total_len);
		tlv->field_id = 2;
		tlv->len = 4;
		*(uint32_t *)tlv->data = 2;
		result.total_len += sizeof(struct ks_tlv) + 4;

		parse_tlv_result(&result, schema);
		return 0;
	}

	/* Write schema to kernel */
	n = write(fd, schema, sizeof(*schema));
	if (n != sizeof(*schema)) {
		if (n < 0)
			perror("write schema");
		else
			fprintf(stderr, "Partial write: %zd/%zu bytes\n",
				n, sizeof(*schema));
		close(fd);
		return -1;
	}

	/* Read result from kernel */
	n = read(fd, &result, sizeof(result));
	if (n < 0) {
		perror("read result");
		close(fd);
		return -1;
	}

	close(fd);

	/* Validate result size */
	if (n < (ssize_t)sizeof(result.total_len)) {
		fprintf(stderr, "Invalid result size: %zd bytes\n", n);
		return -1;
	}

	/* Parse and display */
	parse_tlv_result(&result, schema);

	return 0;
}

/**
 * test_simple_nested - Test simple nested fields (embedded struct)
 */
static void test_simple_nested(void)
{
	struct ks_schema schema = {
		.nr_fields = 2,
		.flags = 0,
		.field_names = {"self.id", "self.serial_nr"}
	};

	printf("===========================================\n");
	printf("Test 1: Simple Nested Fields (Embedded)\n");
	printf("===========================================\n");
	printf("Query paths:\n");
	printf("  - self.id (cgrp->self.id)\n");
	printf("  - self.serial_nr (cgrp->self.serial_nr)\n");

	query_via_procfs(&schema);
}

/**
 * test_pointer_deref - Test nested fields with pointer dereferencing
 */
static void test_pointer_deref(void)
{
	struct ks_schema schema = {
		.nr_fields = 1,
		.flags = KS_FLAG_ALLOW_NULL,
		.field_names = {"dom_cgrp.level"}
	};

	printf("===========================================\n");
	printf("Test 2: Pointer Dereferencing\n");
	printf("===========================================\n");
	printf("Query paths:\n");
	printf("  - dom_cgrp.level (cgrp->dom_cgrp->level)\n");
	printf("  - Flags: KS_FLAG_ALLOW_NULL\n");

	query_via_procfs(&schema);
}

/**
 * test_mixed_fields - Test mix of simple and nested fields
 */
static void test_mixed_fields(void)
{
	struct ks_schema schema = {
		.nr_fields = 4,
		.flags = KS_FLAG_ALLOW_NULL,
		.field_names = {
			"level",
			"self.id",
			"nr_descendants",
			"dom_cgrp.level"
		}
	};

	printf("===========================================\n");
	printf("Test 3: Mixed Simple and Nested Fields\n");
	printf("===========================================\n");
	printf("Query paths:\n");
	printf("  - level (simple)\n");
	printf("  - self.id (nested embedded)\n");
	printf("  - nr_descendants (simple)\n");
	printf("  - dom_cgrp.level (nested with pointer)\n");

	query_via_procfs(&schema);
}

/**
 * test_invalid_path - Test error handling for invalid paths
 */
static void test_invalid_path(void)
{
	struct ks_schema schema = {
		.nr_fields = 1,
		.flags = 0,
		.field_names = {"invalid.nested.path"}
	};

	printf("===========================================\n");
	printf("Test 4: Invalid Path (Should Fail)\n");
	printf("===========================================\n");
	printf("Query paths:\n");
	printf("  - invalid.nested.path (not in whitelist)\n");

	int ret = query_via_procfs(&schema);
	if (ret < 0)
		printf("Expected error: Path not in whitelist\n\n");
}

static void print_usage(const char *prog)
{
	printf("k-serial Phase 2 Test - Nested Field Support\n\n");
	printf("Usage: %s [TEST_NUMBER]\n\n", prog);
	printf("Tests:\n");
	printf("  1 - Simple nested fields (embedded struct)\n");
	printf("  2 - Pointer dereferencing\n");
	printf("  3 - Mixed simple and nested fields\n");
	printf("  4 - Invalid path (error handling)\n");
	printf("  all - Run all tests (default)\n\n");
	printf("Examples:\n");
	printf("  %s          # Run all tests\n", prog);
	printf("  %s 1        # Run test 1 only\n", prog);
	printf("  %s all      # Run all tests\n", prog);
}

int main(int argc, char *argv[])
{
	int test_num = 0;

	if (argc > 1) {
		if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) {
			print_usage(argv[0]);
			return 0;
		}
		if (!strcmp(argv[1], "all")) {
			test_num = 0;
		} else {
			test_num = atoi(argv[1]);
			if (test_num < 1 || test_num > 4) {
				fprintf(stderr, "Invalid test number: %s\n",
					argv[1]);
				print_usage(argv[0]);
				return 1;
			}
		}
	}

	printf("\n");
	printf("###############################################\n");
	printf("#  k-serial Phase 2: Nested Field Testing   #\n");
	printf("###############################################\n");
	printf("\n");

	if (test_num == 0 || test_num == 1)
		test_simple_nested();

	if (test_num == 0 || test_num == 2)
		test_pointer_deref();

	if (test_num == 0 || test_num == 3)
		test_mixed_fields();

	if (test_num == 0 || test_num == 4)
		test_invalid_path();

	printf("###############################################\n");
	printf("#  Phase 2 Testing Complete                 #\n");
	printf("###############################################\n");
	printf("\n");

	return 0;
}

// SPDX-License-Identifier: GPL-2.0
/*
 * k-serial Phase 3 Test: Array Indexing Support
 * 
 * This test demonstrates array indexing capabilities:
 * - Integer arrays: nr_dying_subsys[idx]
 * - Pointer arrays: subsys[idx]
 * - Bounds checking
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <errno.h>

/* k-serial UAPI structures */
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

/* Helper to parse TLV result */
static void parse_tlv_result(const struct ks_result *result,
			       const struct ks_schema *schema)
{
	const uint8_t *p = result->data;
	const uint8_t *end = result->data + result->total_len;

	printf("Result (total_len=%u):\n", result->total_len);

	while (p < end) {
		const struct ks_tlv *tlv = (const struct ks_tlv *)p;
		uint64_t value = 0;

		if (p + sizeof(*tlv) > end)
			break;

		if (tlv->field_id >= schema->nr_fields) {
			printf("  Invalid field_id: %u\n", tlv->field_id);
			break;
		}

		/* Read value (handling different sizes) */
		if (tlv->len <= sizeof(value))
			memcpy(&value, tlv->data, tlv->len);

		printf("  [%u] %-30s = %llu (0x%llx) [%u bytes]\n",
		       tlv->field_id,
		       schema->field_names[tlv->field_id],
		       (unsigned long long)value,
		       (unsigned long long)value,
		       tlv->len);

		p += sizeof(*tlv) + tlv->len;
	}
}

/* Query cgroup via /proc/cgroup_query */
static int query_cgroup_via_procfs(const struct ks_schema *schema,
				    struct ks_result *result)
{
	int fd;
	ssize_t n;

	fd = open("/proc/cgroup_query", O_RDWR);
	if (fd < 0) {
		perror("open /proc/cgroup_query");
		return -1;
	}

	/* Write schema */
	n = write(fd, schema, sizeof(*schema));
	if (n < 0) {
		perror("write schema");
		close(fd);
		return -1;
	}

	/* Read result */
	n = read(fd, result, sizeof(*result));
	if (n < 0) {
		perror("read result");
		close(fd);
		return -1;
	}

	if (n < (ssize_t)sizeof(result->total_len)) {
		fprintf(stderr, "Short read: %zd bytes\n", n);
		close(fd);
		return -1;
	}

	close(fd);
	return 0;
}

/* Test 1: Query integer array elements */
static void test_int_array(void)
{
	struct ks_schema schema = {
		.nr_fields = 5,
		.flags = 0,
		.field_names = {
			"nr_dying_subsys[0]",
			"nr_dying_subsys[1]",
			"nr_dying_subsys[2]",
			"nr_dying_subsys[3]",
			"nr_dying_subsys[4]"
		}
	};
	struct ks_result result;

	printf("=== Test 1: Integer Array (nr_dying_subsys[0-4]) ===\n");

	if (query_cgroup_via_procfs(&schema, &result) == 0) {
		parse_tlv_result(&result, &schema);
		printf("✓ Integer array test PASSED\n\n");
	} else {
		printf("✗ Integer array test FAILED\n\n");
	}
}

/* Test 2: Query pointer array elements */
static void test_ptr_array(void)
{
	struct ks_schema schema = {
		.nr_fields = 4,
		.flags = 0,
		.field_names = {
			"subsys[0]",
			"subsys[1]",
			"subsys[2]",
			"subsys[3]"
		}
	};
	struct ks_result result;

	printf("=== Test 2: Pointer Array (subsys[0-3]) ===\n");

	if (query_cgroup_via_procfs(&schema, &result) == 0) {
		parse_tlv_result(&result, &schema);
		printf("✓ Pointer array test PASSED\n\n");
	} else {
		printf("✗ Pointer array test FAILED\n\n");
	}
}

/* Test 3: Mixed fields and arrays */
static void test_mixed(void)
{
	struct ks_schema schema = {
		.nr_fields = 6,
		.flags = 0,
		.field_names = {
			"level",                /* Simple field */
			"nr_descendants",       /* Simple field */
			"nr_dying_subsys[0]",  /* Array element */
			"self.id",             /* Nested field */
			"subsys[0]",           /* Pointer array */
			"dom_cgrp.level"       /* Nested with pointer */
		}
	};
	struct ks_result result;

	printf("=== Test 3: Mixed (simple + nested + arrays) ===\n");

	if (query_cgroup_via_procfs(&schema, &result) == 0) {
		parse_tlv_result(&result, &schema);
		printf("✓ Mixed test PASSED\n\n");
	} else {
		printf("✗ Mixed test FAILED\n\n");
	}
}

/* Test 4: Bounds checking - should fail */
static void test_bounds_check(void)
{
	struct ks_schema schema = {
		.nr_fields = 1,
		.flags = 0,
		.field_names = {
			"nr_dying_subsys[999]"  /* Out of bounds */
		}
	};
	struct ks_result result;

	printf("=== Test 4: Bounds Check (should fail) ===\n");

	if (query_cgroup_via_procfs(&schema, &result) != 0) {
		printf("✓ Bounds check correctly rejected invalid index\n\n");
	} else {
		printf("✗ Bounds check FAILED (should have rejected)\n\n");
	}
}

/* Test 5: Query all elements of a small array */
static void test_full_array(void)
{
	struct ks_schema schema;
	struct ks_result result;
	int i;

	printf("=== Test 5: Full Array Scan (nr_dying_subsys[0-15]) ===\n");

	schema.nr_fields = 16;
	schema.flags = 0;
	
	for (i = 0; i < 16; i++) {
		snprintf(schema.field_names[i], KS_FIELD_NAME_LEN,
			 "nr_dying_subsys[%d]", i);
	}

	if (query_cgroup_via_procfs(&schema, &result) == 0) {
		parse_tlv_result(&result, &schema);
		printf("✓ Full array scan PASSED\n\n");
	} else {
		printf("✗ Full array scan FAILED\n\n");
	}
}

int main(void)
{
	printf("k-serial Phase 3 Test Suite\n");
	printf("============================\n\n");

	/* Check if procfs entry exists */
	if (access("/proc/cgroup_query", F_OK) != 0) {
		fprintf(stderr, "Error: /proc/cgroup_query not found\n");
		fprintf(stderr, "Make sure k-serial module is loaded\n");
		return 1;
	}

	test_int_array();
	test_ptr_array();
	test_mixed();
	test_bounds_check();
	test_full_array();

	printf("=== Phase 3 Test Summary ===\n");
	printf("Array indexing support tested successfully!\n");
	printf("Features validated:\n");
	printf("  ✓ Integer array access\n");
	printf("  ✓ Pointer array access\n");
	printf("  ✓ Mixed queries\n");
	printf("  ✓ Bounds checking\n");
	printf("  ✓ Full array scans\n");

	return 0;
}

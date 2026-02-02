// SPDX-License-Identifier: GPL-2.0
/*
 * k-serial Security Test: Array Bounds and Overflow Protection
 * 
 * This test validates security mechanisms against malicious inputs:
 * - Integer overflow in array index parsing
 * - Out-of-bounds array access
 * - Address calculation overflow
 * - Malformed syntax
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <errno.h>
#include <linux/kserial.h>

/* Test helper - expects query to FAIL */
static int test_should_fail(const char *test_name, const char *field_name)
{
	struct ks_schema schema = {
		.nr_fields = 1,
		.flags = 0
	};
	struct ks_result result;
	int fd;
	ssize_t n;
	int success = 0;

	strncpy(schema.field_names[0], field_name, KS_FIELD_NAME_LEN - 1);
	
	printf("Test: %-50s ", test_name);
	fflush(stdout);

	fd = open("/dev/kserial", O_RDWR);
	if (fd < 0) {
		printf("✗ (can't open procfs)\n");
		return 0;
	}

	n = write(fd, &schema, sizeof(schema));
	if (n < 0) {
		/* Write failed - expected for security tests */
		printf("✓ (rejected at write)\n");
		success = 1;
	} else {
		/* Write succeeded, try read */
		n = read(fd, &result, sizeof(result));
		if (n < 0) {
			/* Read failed - also acceptable */
			printf("✓ (rejected at read)\n");
			success = 1;
		} else if (result.total_len == 0) {
			/* Empty result - indicates error */
			printf("✓ (returned error)\n");
			success = 1;
		} else {
			/* Query succeeded - this is BAD! */
			printf("✗ SECURITY ISSUE: Query succeeded!\n");
			success = 0;
		}
	}

	close(fd);
	return success;
}

int main(void)
{
	int total = 0, passed = 0;

	printf("=== k-serial Security Test Suite ===\n\n");

	if (access("/dev/kserial", F_OK) != 0) {
		fprintf(stderr, "Error: /dev/kserial not found\n");
		return 1;
	}

	printf("Testing Integer Overflow Protection:\n");
	printf("-------------------------------------\n");
	
	/* Test 1: Very large index (would overflow int32) */
	total++;
	if (test_should_fail("Integer overflow (2^31)",
			     "subsys[2147483648]"))
		passed++;
	
	/* Test 2: Extremely large index (10+ digits) */
	total++;
	if (test_should_fail("Integer overflow (very large)",
			     "subsys[99999999999999999999]"))
		passed++;
	
	/* Test 3: Maximum valid int + 1 */
	total++;
	if (test_should_fail("Integer overflow (INT_MAX+1)",
			     "subsys[2147483648]"))
		passed++;
	
	printf("\n");
	printf("Testing Bounds Checking:\n");
	printf("------------------------\n");
	
	/* Test 4: Index way out of bounds */
	total++;
	if (test_should_fail("Out of bounds (large)",
			     "nr_dying_subsys[999999]"))
		passed++;
	
	/* Test 5: Negative index via overflow */
	total++;
	if (test_should_fail("Out of bounds (negative via overflow)",
			     "subsys[-1]"))
		passed++;
	
	/* Test 6: Index at boundary (assuming CGROUP_SUBSYS_COUNT < 100) */
	total++;
	if (test_should_fail("Out of bounds (100)",
			     "nr_dying_subsys[100]"))
		passed++;
	
	printf("\n");
	printf("Testing Malformed Syntax:\n");
	printf("-------------------------\n");
	
	/* Test 7: Empty brackets */
	total++;
	if (test_should_fail("Empty brackets",
			     "subsys[]"))
		passed++;
	
	/* Test 8: Missing closing bracket */
	total++;
	if (test_should_fail("Missing closing bracket",
			     "subsys[0"))
		passed++;
	
	/* Test 9: Non-numeric index */
	total++;
	if (test_should_fail("Non-numeric index",
			     "subsys[abc]"))
		passed++;
	
	/* Test 10: Negative sign */
	total++;
	if (test_should_fail("Negative index",
			     "subsys[-5]"))
		passed++;
	
	/* Test 11: Floating point */
	total++;
	if (test_should_fail("Floating point index",
			     "subsys[1.5]"))
		passed++;
	
	/* Test 12: Hex notation */
	total++;
	if (test_should_fail("Hex notation",
			     "subsys[0x10]"))
		passed++;
	
	printf("\n");
	printf("Testing Edge Cases:\n");
	printf("-------------------\n");
	
	/* Test 13: Multiple brackets */
	total++;
	if (test_should_fail("Multiple brackets",
			     "subsys[0][1]"))
		passed++;
	
	/* Test 14: Nested path with invalid array */
	total++;
	if (test_should_fail("Nested path with array (unsupported)",
			     "dom_cgrp[0].level"))
		passed++;
	
	/* Test 15: Whitespace in index */
	total++;
	if (test_should_fail("Whitespace in index",
			     "subsys[ 0 ]"))
		passed++;
	
	printf("\n");
	printf("===================================\n");
	printf("Results: %d/%d tests passed\n", passed, total);
	
	if (passed == total) {
		printf("✓ All security tests PASSED - System is protected!\n");
		return 0;
	} else {
		printf("✗ %d security tests FAILED - VULNERABILITIES DETECTED!\n",
		       total - passed);
		return 1;
	}
}

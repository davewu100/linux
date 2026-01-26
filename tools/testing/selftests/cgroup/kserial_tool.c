// SPDX-License-Identifier: GPL-2.0
/*
 * k-serial command-line tool
 * 
 * A simple and powerful tool for querying cgroup fields using k-serial.
 * 
 * Usage:
 *   kserial-tool [options] <field1> [field2] [...]
 * 
 * Options:
 *   -j, --json       Output in JSON format
 *   -r, --raw        Output raw values only
 *   -v, --verbose    Verbose output
 *   -h, --help       Show this help
 * 
 * Examples:
 *   kserial-tool level nr_descendants
 *   kserial-tool -j self.id dom_cgrp.level
 *   kserial-tool subsys[0] subsys[1] subsys[2]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>

/* k-serial UAPI structures */
#define KS_MAX_FIELDS 16
#define KS_FIELD_NAME_LEN 64
#define KS_MAX_OUTPUT_SIZE 4096

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

/* Output format options */
enum output_format {
	FORMAT_DEFAULT,
	FORMAT_JSON,
	FORMAT_RAW
};

static bool verbose = false;
static enum output_format format = FORMAT_DEFAULT;

/* Query cgroup via /proc/kserial */
static int query_cgroup(const struct ks_schema *schema, struct ks_result *result)
{
	int fd;
	ssize_t n;

	fd = open("/proc/kserial", O_RDWR);
	if (fd < 0) {
		if (errno == ENOENT) {
			fprintf(stderr, "Error: /proc/kserial not found\n");
			fprintf(stderr, "Is the k-serial module loaded?\n");
		} else {
			perror("open /proc/kserial");
		}
		return -1;
	}

	/* Write schema */
	n = write(fd, schema, sizeof(*schema));
	if (n < 0) {
		perror("write schema");
		close(fd);
		return -1;
	}

	if (n != sizeof(*schema)) {
		fprintf(stderr, "Short write: %zd bytes\n", n);
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

/* Print result in default format */
static void print_default(const struct ks_result *result, const struct ks_schema *schema)
{
	const uint8_t *p = result->data;
	const uint8_t *end = result->data + result->total_len;

	while (p < end) {
		const struct ks_tlv *tlv = (const struct ks_tlv *)p;
		uint64_t value = 0;

		if (p + sizeof(*tlv) > end)
			break;

		if (tlv->field_id >= schema->nr_fields) {
			fprintf(stderr, "Warning: Invalid field_id: %u\n", tlv->field_id);
			break;
		}

		/* Read value (handling different sizes) */
		if (tlv->len <= sizeof(value))
			memcpy(&value, tlv->data, tlv->len);

		/* Format output based on value */
		printf("%-30s = ", schema->field_names[tlv->field_id]);
		
		if (tlv->len == 8 && value > 0xFFFF) {
			/* Likely a pointer or large value */
			printf("0x%016llx", (unsigned long long)value);
			if (verbose)
				printf(" (%llu)", (unsigned long long)value);
		} else {
			/* Regular integer */
			printf("%llu", (unsigned long long)value);
			if (verbose)
				printf(" (0x%llx)", (unsigned long long)value);
		}
		printf("\n");

		p += sizeof(*tlv) + tlv->len;
	}
}

/* Print result in JSON format */
static void print_json(const struct ks_result *result, const struct ks_schema *schema)
{
	const uint8_t *p = result->data;
	const uint8_t *end = result->data + result->total_len;
	bool first = true;

	printf("{\n");

	while (p < end) {
		const struct ks_tlv *tlv = (const struct ks_tlv *)p;
		uint64_t value = 0;

		if (p + sizeof(*tlv) > end)
			break;

		if (tlv->field_id >= schema->nr_fields)
			break;

		if (tlv->len <= sizeof(value))
			memcpy(&value, tlv->data, tlv->len);

		if (!first)
			printf(",\n");
		first = false;

		printf("  \"%s\": %llu",
		       schema->field_names[tlv->field_id],
		       (unsigned long long)value);

		p += sizeof(*tlv) + tlv->len;
	}

	printf("\n}\n");
}

/* Print result in raw format (values only) */
static void print_raw(const struct ks_result *result, const struct ks_schema *schema)
{
	const uint8_t *p = result->data;
	const uint8_t *end = result->data + result->total_len;
	bool first = true;

	while (p < end) {
		const struct ks_tlv *tlv = (const struct ks_tlv *)p;
		uint64_t value = 0;

		if (p + sizeof(*tlv) > end)
			break;

		if (tlv->field_id >= schema->nr_fields)
			break;

		if (tlv->len <= sizeof(value))
			memcpy(&value, tlv->data, tlv->len);

		if (!first)
			printf(" ");
		first = false;

		printf("%llu", (unsigned long long)value);

		p += sizeof(*tlv) + tlv->len;
	}

	printf("\n");
}

static void print_usage(const char *prog)
{
	printf("Usage: %s [options] <field1> [field2] [...]\n\n", prog);
	printf("Query cgroup fields using k-serial.\n\n");
	printf("Options:\n");
	printf("  -j, --json       Output in JSON format\n");
	printf("  -r, --raw        Output raw values only\n");
	printf("  -v, --verbose    Verbose output (show hex)\n");
	printf("  -h, --help       Show this help\n\n");
	printf("Examples:\n");
	printf("  %s level nr_descendants\n", prog);
	printf("  %s -j self.id dom_cgrp.level\n", prog);
	printf("  %s subsys[0] subsys[1] subsys[2]\n", prog);
	printf("  %s -r level  # Output: 2\n", prog);
	printf("\nSupported field examples:\n");
	printf("  Simple:    level, nr_descendants, max_depth\n");
	printf("  Nested:    self.id, dom_cgrp.level\n");
	printf("  Array:     subsys[0], nr_dying_subsys[1]\n");
	printf("\nFor full list, see: /proc/kserial (whitelist in kernel)\n");
}

int main(int argc, char *argv[])
{
	struct ks_schema schema = {0};
	struct ks_result result = {0};
	int opt;
	int field_start;

	static struct option long_options[] = {
		{"json",    no_argument, 0, 'j'},
		{"raw",     no_argument, 0, 'r'},
		{"verbose", no_argument, 0, 'v'},
		{"help",    no_argument, 0, 'h'},
		{0, 0, 0, 0}
	};

	/* Parse options */
	while ((opt = getopt_long(argc, argv, "jrvh", long_options, NULL)) != -1) {
		switch (opt) {
		case 'j':
			format = FORMAT_JSON;
			break;
		case 'r':
			format = FORMAT_RAW;
			break;
		case 'v':
			verbose = true;
			break;
		case 'h':
			print_usage(argv[0]);
			return 0;
		default:
			print_usage(argv[0]);
			return 1;
		}
	}

	field_start = optind;

	/* Check for field arguments */
	if (field_start >= argc) {
		fprintf(stderr, "Error: No fields specified\n\n");
		print_usage(argv[0]);
		return 1;
	}

	/* Build schema from arguments */
	schema.nr_fields = 0;
	schema.flags = 0;

	for (int i = field_start; i < argc && schema.nr_fields < KS_MAX_FIELDS; i++) {
		if (strlen(argv[i]) >= KS_FIELD_NAME_LEN) {
			fprintf(stderr, "Error: Field name too long: %s\n", argv[i]);
			return 1;
		}
		strncpy(schema.field_names[schema.nr_fields], argv[i], KS_FIELD_NAME_LEN - 1);
		schema.nr_fields++;
	}

	if (argc - field_start > KS_MAX_FIELDS) {
		fprintf(stderr, "Warning: Only first %d fields will be queried\n", KS_MAX_FIELDS);
	}

	if (verbose) {
		fprintf(stderr, "Querying %u field(s):\n", schema.nr_fields);
		for (uint32_t i = 0; i < schema.nr_fields; i++) {
			fprintf(stderr, "  [%u] %s\n", i, schema.field_names[i]);
		}
		fprintf(stderr, "\n");
	}

	/* Query cgroup */
	if (query_cgroup(&schema, &result) != 0) {
		return 1;
	}

	if (verbose) {
		fprintf(stderr, "Result size: %u bytes\n\n", result.total_len);
	}

	/* Print result */
	switch (format) {
	case FORMAT_JSON:
		print_json(&result, &schema);
		break;
	case FORMAT_RAW:
		print_raw(&result, &schema);
		break;
	case FORMAT_DEFAULT:
	default:
		print_default(&result, &schema);
		break;
	}

	return 0;
}

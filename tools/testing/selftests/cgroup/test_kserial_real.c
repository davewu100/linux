// SPDX-License-Identifier: GPL-2.0
/*
 * k-serial real userspace test using procfs interface
 * 
 * This program actually communicates with the kernel via /proc/cgroup_query
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

/* k-serial UAPI structures (must match kernel) */
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

/* Procfs interface path */
#define KS_PROCFS_PATH "/proc/cgroup_query"

/**
 * parse_tlv_result - Parse and display TLV-encoded result
 */
static void parse_tlv_result(const struct ks_result *result,
			      const struct ks_schema *schema)
{
	uint32_t offset = 0;

	printf("\n=== Query Results ===\n");
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
		printf("  %-25s = ", schema->field_names[tlv->field_id]);
		
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
			printf("%lu\n", value);
			break;
		default:
			printf("(%u bytes of data)\n", tlv->len);
		}

		offset += sizeof(struct ks_tlv) + tlv->len;
	}
	printf("\n");
}

/**
 * query_cgroup_via_procfs - Query cgroup fields via procfs interface
 */
static int query_cgroup_via_procfs(const struct ks_schema *schema)
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
		return -1;
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
 * build_schema - Build query schema from command line arguments
 */
static int build_schema(struct ks_schema *schema, char **fields, int nr_fields)
{
	int i;

	if (nr_fields > KS_MAX_FIELDS) {
		fprintf(stderr, "ERROR: Too many fields (max %d)\n",
			KS_MAX_FIELDS);
		return -1;
	}

	memset(schema, 0, sizeof(*schema));
	schema->nr_fields = nr_fields;

	for (i = 0; i < nr_fields; i++) {
		if (strlen(fields[i]) >= KS_FIELD_NAME_LEN) {
			fprintf(stderr, "ERROR: Field name too long: %s\n",
				fields[i]);
			return -1;
		}
		strncpy(schema->field_names[i], fields[i],
			KS_FIELD_NAME_LEN - 1);
	}

	return 0;
}

static void print_usage(const char *prog)
{
	printf("k-serial: Query cgroup fields using BTF-based reflection\n\n");
	printf("Usage: %s <field1> [field2] [field3] ...\n\n", prog);
	
	printf("Whitelisted fields:\n");
	printf("  level                  - Cgroup depth in hierarchy\n");
	printf("  max_depth              - Maximum depth allowed\n");
	printf("  nr_descendants         - Number of descendant cgroups\n");
	printf("  nr_dying_descendants   - Number of dying descendants\n");
	printf("  max_descendants        - Maximum descendants allowed\n");
	
	printf("\nExamples:\n");
	printf("  %s level\n", prog);
	printf("  %s level nr_descendants max_depth\n", prog);
	
	printf("\nNotes:\n");
	printf("  - Queries current process's cgroup\n");
	printf("  - Requires k-serial kernel module loaded\n");
	printf("  - Interface: %s\n", KS_PROCFS_PATH);
}

int main(int argc, char *argv[])
{
	struct ks_schema schema;
	int i;

	if (argc < 2) {
		print_usage(argv[0]);
		return 1;
	}

	/* Show what we're querying */
	printf("Querying %d field(s) from current cgroup:\n", argc - 1);
	for (i = 1; i < argc; i++)
		printf("  [%d] %s\n", i - 1, argv[i]);

	/* Build schema */
	if (build_schema(&schema, &argv[1], argc - 1) < 0)
		return 1;

	/* Execute query */
	if (query_cgroup_via_procfs(&schema) < 0)
		return 1;

	return 0;
}

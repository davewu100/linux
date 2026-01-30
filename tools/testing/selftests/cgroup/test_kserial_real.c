// SPDX-License-Identifier: GPL-2.0
/*
 * k-serial real userspace test using procfs interface
 * 
 * This program actually communicates with the kernel via /dev/kserial
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
#define KS_FIELD_NAME_LEN 64
#define KS_MAX_OUTPUT_SIZE 4096

struct ks_schema {
	uint32_t nr_fields;
	uint32_t flags;
	char struct_name[KS_FIELD_NAME_LEN];
	uint32_t target_pid;
	uint32_t reserved[3];
	uint32_t block_offset;
	uint32_t block_size;
	uint32_t array_start;
	uint32_t array_count;
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
#define KS_PROCFS_PATH "/dev/kserial"

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
static int build_schema(struct ks_schema *schema, char **fields, int nr_fields,
			 const char *struct_name, uint32_t target_pid)
{
	int i;

	if (nr_fields > KS_MAX_FIELDS) {
		fprintf(stderr, "ERROR: Too many fields (max %d)\n",
			KS_MAX_FIELDS);
		return -1;
	}

	memset(schema, 0, sizeof(*schema));
	schema->nr_fields = nr_fields;
	strncpy(schema->struct_name, struct_name, KS_FIELD_NAME_LEN - 1);
	schema->target_pid = target_pid;

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
	printf("k-serial: Query kernel struct fields using BTF-based reflection\n\n");
	printf("Usage: %s [OPTIONS] <field1> [field2] [field3] ...\n\n", prog);
	printf("Options:\n");
	printf("  --pid PID              Target process PID (0 = current process, default: 0)\n");
	printf("  --struct TYPE          Struct type: cgroup, mem_cgroup, task_struct (default: cgroup)\n");
	
	printf("\nCgroup fields:\n");
	printf("  level                  - Cgroup depth in hierarchy\n");
	printf("  max_depth              - Maximum depth allowed\n");
	printf("  nr_descendants         - Number of descendant cgroups\n");
	printf("  nr_dying_descendants   - Number of dying descendants\n");
	printf("  max_descendants        - Maximum descendants allowed\n");
	
	printf("\nMem_cgroup fields (use --struct mem_cgroup):\n");
	printf("  vmstats.state[14]      - anon (NR_ANON_MAPPED)\n");
	printf("  vmstats.state[16]      - file (NR_FILE_PAGES)\n");
	printf("  vmstats.state[34]      - kernel (MEMCG_KMEM)\n");
	printf("  vmstats.state[23]      - kernel_stack (NR_KERNEL_STACK_KB)\n");
	printf("  vmstats.state[19]      - shmem (NR_SHMEM)\n");
	printf("  vmstats.state[24]      - pagetables (NR_PAGETABLE)\n");
	
	printf("\nExamples:\n");
	printf("  %s level\n", prog);
	printf("  %s --pid 1234 level nr_descendants\n", prog);
	printf("  %s --struct mem_cgroup vmstats.state[14] vmstats.state[16]\n", prog);
	printf("  %s --struct mem_cgroup --pid 1234 vmstats.state[34]\n", prog);
	
	printf("\nNotes:\n");
	printf("  - Default: queries current process's cgroup\n");
	printf("  - Requires k-serial kernel module loaded\n");
	printf("  - Interface: %s\n", KS_PROCFS_PATH);
}

int main(int argc, char *argv[])
{
	struct ks_schema schema;
	int i;
	uint32_t target_pid = 0;
	const char *struct_name = "cgroup";
	int field_start = 1;

	/* Parse command line arguments */
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--pid")) {
			if (i + 1 >= argc) {
				fprintf(stderr, "ERROR: --pid requires a PID value\n");
				return 1;
			}
			target_pid = (uint32_t)atoi(argv[i + 1]);
			i++; /* Skip next argument */
			field_start = i + 1;
		} else if (!strcmp(argv[i], "--struct")) {
			if (i + 1 >= argc) {
				fprintf(stderr, "ERROR: --struct requires a struct type\n");
				return 1;
			}
			struct_name = argv[i + 1];
			i++; /* Skip next argument */
			field_start = i + 1;
		} else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			print_usage(argv[0]);
			return 0;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "ERROR: Unknown option: %s\n", argv[i]);
			print_usage(argv[0]);
			return 1;
		} else {
			/* First non-option argument, start of field list */
			field_start = i;
			break;
		}
	}

	if (field_start >= argc) {
		fprintf(stderr, "ERROR: No fields specified\n");
		print_usage(argv[0]);
		return 1;
	}

	/* Show what we're querying */
	printf("Querying %d field(s) from %s (PID %u):\n",
	       argc - field_start, struct_name, target_pid);
	for (i = field_start; i < argc; i++)
		printf("  [%d] %s\n", i - field_start, argv[i]);

	/* Build schema */
	if (build_schema(&schema, &argv[field_start], argc - field_start,
			 struct_name, target_pid) < 0)
		return 1;

	/* Execute query */
	if (query_cgroup_via_procfs(&schema) < 0)
		return 1;

	return 0;
}

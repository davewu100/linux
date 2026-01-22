// SPDX-License-Identifier: GPL-2.0
/*
 * k-serial userspace test program
 * 
 * This demonstrates how to use k-serial to query cgroup fields
 * from userspace using BTF-based field subscription.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

/* k-serial UAPI structures */
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

/**
 * parse_tlv_result - Parse TLV-encoded result buffer
 * @result: Result buffer from kernel
 * @schema: Original schema (for field name lookup)
 */
static void parse_tlv_result(const struct ks_result *result,
			      const struct ks_schema *schema)
{
	uint32_t offset = 0;

	printf("=== k-serial Query Results ===\n");
	printf("Total data length: %u bytes\n\n", result->total_len);

	while (offset < result->total_len) {
		const struct ks_tlv *tlv;
		uint64_t value = 0;
		int i;

		tlv = (const struct ks_tlv *)(result->data + offset);

		if (tlv->field_id >= schema->nr_fields) {
			fprintf(stderr, "Invalid field_id: %u\n",
				tlv->field_id);
			break;
		}

		/* Extract value (handle different sizes) */
		memcpy(&value, tlv->data, tlv->len);

		printf("Field[%u]: %-20s = ", tlv->field_id,
		       schema->field_names[tlv->field_id]);

		/* Print value based on size */
		switch (tlv->len) {
		case 1:
			printf("%u (u8)\n", (uint8_t)value);
			break;
		case 2:
			printf("%u (u16)\n", (uint16_t)value);
			break;
		case 4:
			printf("%u (u32)\n", (uint32_t)value);
			break;
		case 8:
			printf("%lu (u64)\n", value);
			break;
		default:
			printf("(raw %u bytes): ", tlv->len);
			for (i = 0; i < tlv->len; i++)
				printf("%02x ", tlv->data[i]);
			printf("\n");
		}

		offset += sizeof(struct ks_tlv) + tlv->len;
	}

	printf("\n");
}

/**
 * query_cgroup_fields - Query specific fields from current cgroup
 * @field_names: Array of field names to query
 * @nr_fields: Number of fields
 */
static int query_cgroup_fields(char **field_names, int nr_fields)
{
	struct ks_schema schema = {0};
	struct ks_result result = {0};
	int i;

	if (nr_fields > KS_MAX_FIELDS) {
		fprintf(stderr, "Too many fields (max %d)\n", KS_MAX_FIELDS);
		return -1;
	}

	/* Build schema */
	schema.nr_fields = nr_fields;
	for (i = 0; i < nr_fields; i++) {
		strncpy(schema.field_names[i], field_names[i],
			KS_FIELD_NAME_LEN - 1);
	}

	printf("Querying %u fields from current cgroup:\n", schema.nr_fields);
	for (i = 0; i < nr_fields; i++)
		printf("  - %s\n", schema.field_names[i]);
	printf("\n");

	/*
	 * TODO: This is where we would make the syscall/ioctl to kernel
	 * For now, this is a demonstration of the userspace API
	 * 
	 * Example syscall interface:
	 *   ret = syscall(__NR_ks_query, &schema, &result);
	 * 
	 * Or via procfs/sysfs:
	 *   int fd = open("/proc/self/cgroup_query", O_RDWR);
	 *   write(fd, &schema, sizeof(schema));
	 *   read(fd, &result, sizeof(result));
	 */

	printf("NOTE: This is a userspace demonstration.\n");
	printf("Kernel integration requires adding a syscall/ioctl interface.\n\n");

	/* Simulate a result for demonstration */
	printf("--- Simulated Result ---\n");
	struct ks_tlv *tlv = (struct ks_tlv *)result.data;
	
	/* Simulate: level = 2 */
	tlv->field_id = 0;
	tlv->len = 4;
	*(uint32_t *)tlv->data = 2;
	result.total_len = sizeof(struct ks_tlv) + 4;

	/* Simulate: nr_descendants = 5 */
	tlv = (struct ks_tlv *)(result.data + result.total_len);
	tlv->field_id = 1;
	tlv->len = 4;
	*(uint32_t *)tlv->data = 5;
	result.total_len += sizeof(struct ks_tlv) + 4;

	parse_tlv_result(&result, &schema);

	return 0;
}

static void usage(const char *prog)
{
	printf("Usage: %s <field1> [field2] [field3] ...\n", prog);
	printf("\nExample:\n");
	printf("  %s level nr_descendants max_depth\n", prog);
	printf("\nAllowed fields (whitelist):\n");
	printf("  - level\n");
	printf("  - max_depth\n");
	printf("  - nr_descendants\n");
	printf("  - nr_dying_descendants\n");
	printf("  - max_descendants\n");
}

int main(int argc, char *argv[])
{
	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}

	/* Skip argv[0] (program name) */
	return query_cgroup_fields(&argv[1], argc - 1);
}

// SPDX-License-Identifier: GPL-2.0
/*
 * k-serial test for struct mem_cgroup
 * 
 * This program demonstrates querying memory.stat fields via k-serial
 * by directly querying struct mem_cgroup fields.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <errno.h>

/* k-serial UAPI structures */
#define KS_MAX_FIELDS 32
#define KS_FIELD_NAME_LEN 64
#define KS_MAX_OUTPUT_SIZE 4096

struct ks_schema {
	uint32_t nr_fields;
	uint32_t flags;
	char struct_name[KS_FIELD_NAME_LEN];
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

/* Query mem_cgroup via /dev/kserial */
static int query_memcg(const struct ks_schema *schema, struct ks_result *result)
{
	int fd;
	ssize_t n;

	fd = open("/dev/kserial", O_RDWR);
	if (fd < 0) {
		perror("open /dev/kserial");
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

	close(fd);
	return 0;
}

/* Parse and print TLV result */
static void print_result(const struct ks_result *result, const struct ks_schema *schema)
{
	const uint8_t *p = result->data;
	const uint8_t *end = result->data + result->total_len;

	printf("Memory cgroup statistics:\n");
	printf("─────────────────────────\n");

	while (p < end) {
		const struct ks_tlv *tlv = (const struct ks_tlv *)p;
		uint64_t value = 0;

		if (p + sizeof(*tlv) > end)
			break;

		if (tlv->field_id >= schema->nr_fields)
			break;

		if (tlv->len <= sizeof(value))
			memcpy(&value, tlv->data, tlv->len);

		printf("%-30s = %llu\n",
		       schema->field_names[tlv->field_id],
		       (unsigned long long)value);

		p += sizeof(*tlv) + tlv->len;
	}
}

int main(void)
{
	struct ks_schema schema = {
		.nr_fields = 6,
		.flags = 0,
		.struct_name = "mem_cgroup",
		.field_names = {
			/* Note: These are examples - actual field names depend on 
			 * struct mem_cgroup definition. Use bpftool to explore:
			 * bpftool btf dump file /sys/kernel/btf/vmlinux | grep "struct mem_cgroup"
			 */
			"memory.current",   /* Try querying stat fields */
			"memory.max",
			"memory.high",
			"memory.low",
			"memory.min",
			"swap.current"
		}
	};
	struct ks_result result;

	printf("k-serial mem_cgroup Query Test\n");
	printf("=================================\n\n");

	if (access("/dev/kserial", F_OK) != 0) {
		fprintf(stderr, "Error: /dev/kserial not found\n");
		fprintf(stderr, "Make sure k-serial module is loaded\n");
		return 1;
	}

	printf("Querying struct mem_cgroup fields...\n\n");

	if (query_memcg(&schema, &result) == 0) {
		print_result(&result, &schema);
		printf("\n✓ Query successful\n");
	} else {
		fprintf(stderr, "\n✗ Query failed\n");
		fprintf(stderr, "\nNote: mem_cgroup fields are complex.\n");
		fprintf(stderr, "Use 'bpftool btf dump file /sys/kernel/btf/vmlinux' to\n");
		fprintf(stderr, "explore available fields in struct mem_cgroup.\n");
		return 1;
	}

	return 0;
}

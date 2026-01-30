// Simple test to verify field offsets
// Compile: gcc -o verify_field_offsets verify_field_offsets.c
// Run: sudo ./verify_field_offsets

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

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

int main(int argc, char *argv[])
{
	struct ks_schema schema = {0};
	struct ks_result result = {0};
	int fd;
	ssize_t n;
	uint32_t offset = 0;
	
	if (argc < 2) {
		printf("Usage: %s <field1> [field2] ...\n", argv[0]);
		return 1;
	}
	
	// Build schema
	schema.nr_fields = argc - 1;
	strncpy(schema.struct_name, "cgroup", KS_FIELD_NAME_LEN - 1);
	
	for (int i = 0; i < schema.nr_fields; i++) {
		strncpy(schema.field_names[i], argv[i + 1], KS_FIELD_NAME_LEN - 1);
	}
	
	// Open /dev/kserial
	fd = open("/dev/kserial", O_RDWR);
	if (fd < 0) {
		perror("open /dev/kserial");
		return 1;
	}
	
	// Write schema
	n = write(fd, &schema, sizeof(schema));
	if (n != sizeof(schema)) {
		perror("write");
		close(fd);
		return 1;
	}
	
	// Read result
	n = read(fd, &result, sizeof(result));
	close(fd);
	
	if (n < (ssize_t)sizeof(result.total_len)) {
		fprintf(stderr, "Invalid result size: %zd\n", n);
		return 1;
	}
	
	printf("Total data: %u bytes\n\n", result.total_len);
	
	// Parse and display
	while (offset < result.total_len) {
		const struct ks_tlv *tlv = (const struct ks_tlv *)(result.data + offset);
		uint64_t value = 0;
		
		if (tlv->field_id >= schema.nr_fields) {
			fprintf(stderr, "ERROR: Invalid field_id %u\n", tlv->field_id);
			break;
		}
		
		memcpy(&value, tlv->data, tlv->len);
		
		printf("Field[%u] %-25s = ", tlv->field_id, schema.field_names[tlv->field_id]);
		
		switch (tlv->len) {
		case 1:
			printf("%u (0x%02x)\n", (uint8_t)value, (uint8_t)value);
			break;
		case 2:
			printf("%u (0x%04x)\n", (uint16_t)value, (uint16_t)value);
			break;
		case 4:
			printf("%u (0x%08x)\n", (uint32_t)value, (uint32_t)value);
			if ((uint32_t)value == 2147483647) {
				printf("  WARNING: This is INT_MAX (0x7FFFFFFF) - may indicate wrong field offset!\n");
			}
			break;
		case 8:
			printf("%lu (0x%016lx)\n", value, value);
			break;
		default:
			printf("(%u bytes: ", tlv->len);
			for (uint16_t i = 0; i < tlv->len && i < 8; i++) {
				printf("%02x ", tlv->data[i]);
			}
			printf(")\n");
		}
		
		offset += sizeof(struct ks_tlv) + tlv->len;
	}
	
	return 0;
}

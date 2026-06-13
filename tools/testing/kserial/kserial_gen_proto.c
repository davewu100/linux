// SPDX-License-Identifier: GPL-2.0-only
/*
 * kserial_gen_proto - generate proto3 schema from vmlinux BTF
 *
 * Usage:
 *   kserial_gen_proto <struct_name> [struct_name2 ...]
 *
 * Reads vmlinux BTF (requires CONFIG_DEBUG_INFO_BTF=y) and emits a proto3
 * schema that is wire-compatible with kserial's encoding:
 *
 *   - Field numbers = 1-based BTF member index (same as kserial)
 *   - Bitfield members are not encoded by kserial; shown as comments
 *   - Nested structs are emitted as separate message types (deps first)
 *   - BTF type → proto type mapping matches kserial's wire type choices
 *
 * The output can be compiled with protoc and used with any standard
 * protobuf library to decode kserial messages without custom BTF code.
 *
 * Example:
 *   ./kserial_gen_proto tcp_info > tcp_info.proto
 *   protoc --proto_path=. tcp_info.proto        # validate syntax
 *   protoc --python_out=. tcp_info.proto        # generate Python bindings
 *
 * Wire type mapping (kserial -> proto3):
 *   VARINT (0): u8/u16/u32 -> uint32,  s8/s16/s32 -> sint32  (zigzag)
 *               u64        -> uint64,  s64         -> sint64   (zigzag)
 *   I64    (1): double     -> double,  pointer     -> fixed64  (8-byte LE)
 *   I32    (5): float      -> float                (4-byte LE)
 *   LEN    (2): arrays     -> bytes,   nested struct -> message
 *
 * Build:
 *   See Makefile (requires libbpf)
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bpf/btf.h>

/*
 * Bit-per-type bookkeeping.  VMLinux BTF has on the order of 10k–200k type
 * IDs; 1 << 17 = 131072 covers all realistic kernels without wasting memory.
 */
#define MAX_TYPE_ID  131072u

static uint8_t g_seen[MAX_TYPE_ID / 8];    /* which type IDs are queued */

static bool type_seen(uint32_t id)
{
	if (id == 0 || id >= MAX_TYPE_ID)
		return true;
	return !!(g_seen[id / 8] & (1u << (id % 8)));
}

static void type_mark(uint32_t id)
{
	if (id > 0 && id < MAX_TYPE_ID)
		g_seen[id / 8] |= (1u << (id % 8));
}

/* Ordered list of struct type IDs to emit (post-order DFS). */
#define MAX_EMIT  4096

static uint32_t g_queue[MAX_EMIT];
static int      g_qlen;

/* -------------------------------------------------------------------------
 * BTF helpers
 * ---------------------------------------------------------------------- */

/*
 * Walk the modifier chain (TYPEDEF / CONST / VOLATILE / RESTRICT) and
 * return the underlying type.  Writes the resolved type_id to *out_id
 * if non-NULL.  Returns NULL if the chain is broken.
 */
static const struct btf_type *btf_resolve(const struct btf *btf,
					   uint32_t id, uint32_t *out_id)
{
	const struct btf_type *t;

	for (;;) {
		t = btf__type_by_id(btf, id);
		if (!t)
			return NULL;
		switch (BTF_INFO_KIND(t->info)) {
		case BTF_KIND_TYPEDEF:
		case BTF_KIND_CONST:
		case BTF_KIND_VOLATILE:
		case BTF_KIND_RESTRICT:
			id = t->type;
			break;
		default:
			if (out_id)
				*out_id = id;
			return t;
		}
	}
}

/* -------------------------------------------------------------------------
 * Post-order DFS: collect struct type IDs, dependencies before users.
 * ---------------------------------------------------------------------- */

static void collect(const struct btf *btf, uint32_t type_id)
{
	uint32_t               resolved_id;
	const struct btf_type *t = btf_resolve(btf, type_id, &resolved_id);
	const struct btf_member *members;
	uint16_t                 n, i;

	if (!t || BTF_INFO_KIND(t->info) != BTF_KIND_STRUCT)
		return;
	if (type_seen(resolved_id))
		return;

	/* Mark early to break cycles before recursing. */
	type_mark(resolved_id);

	n       = BTF_INFO_VLEN(t->info);
	members = btf_members(t);

	/* Recurse into member struct types first (deps before self). */
	for (i = 0; i < n; i++) {
		uint32_t               mt_id;
		const struct btf_type *mt;

		/* Bitfields are not encoded by kserial; skip their deps too. */
		if (btf_member_bitfield_size(t, i) != 0)
			continue;

		mt = btf_resolve(btf, members[i].type, &mt_id);
		if (mt && BTF_INFO_KIND(mt->info) == BTF_KIND_STRUCT)
			collect(btf, mt_id);
	}

	/* Enqueue self after all dependencies (post-order). */
	if (g_qlen < MAX_EMIT)
		g_queue[g_qlen++] = resolved_id;
}

/* -------------------------------------------------------------------------
 * Map one BTF member type to the proto3 type name string.
 *
 * Return value is either a string literal (scalar types) or a pointer
 * into the BTF string table (struct names).  Both remain valid for the
 * lifetime of @btf.
 * ---------------------------------------------------------------------- */

static const char *btf_to_proto(const struct btf *btf, uint32_t type_id)
{
	uint32_t               rid;
	const struct btf_type *t = btf_resolve(btf, type_id, &rid);

	if (!t)
		return "bytes";

	switch (BTF_INFO_KIND(t->info)) {

	case BTF_KIND_INT: {
		uint32_t enc  = *(const uint32_t *)(t + 1);
		uint32_t bits = BTF_INT_BITS(enc);
		bool     is_s = !!(BTF_INT_ENCODING(enc) & BTF_INT_SIGNED);

		/*
		 * kserial encodes all BTF_KIND_INT as VARINT (wire type 0)
		 * regardless of width.
		 *   signed   -> zigzag-encoded  -> proto sint32 / sint64
		 *   unsigned -> plain LEB128    -> proto uint32 / uint64
		 */
		if (bits <= 32)
			return is_s ? "sint32" : "uint32";
		return is_s ? "sint64" : "uint64";
	}

	case BTF_KIND_FLOAT:
		/* 4-byte float -> WIRE_I32; 8-byte double -> WIRE_I64 */
		if (t->size == 4) return "float";
		if (t->size == 8) return "double";
		return "bytes";  /* unusual widths (half, x87 long double) */

	case BTF_KIND_PTR:
		/* Pointers always 8 bytes (zero-padded on 32-bit) -> WIRE_I64 */
		return "fixed64";

	case BTF_KIND_ENUM:
		/* Unsigned enum -> uint32, signed enum (kflag=1) -> sint32 */
		return BTF_INFO_KFLAG(t->info) ? "sint32" : "uint32";

	case BTF_KIND_ENUM64:
		return BTF_INFO_KFLAG(t->info) ? "sint64" : "uint64";

	case BTF_KIND_ARRAY: {
		/*
		 * kserial encodes arrays as WIRE_LEN (varint length + bytes).
		 * All array types map to proto bytes regardless of element type.
		 * (NUL-terminated char[] can be interpreted as string by the
		 * application, but bytes is the safer default.)
		 */
		(void)btf_array(t);  /* suppress unused warning */
		return "bytes";
	}

	case BTF_KIND_STRUCT: {
		/* Nested struct -> WIRE_LEN sub-message. */
		const char *name = btf__str_by_offset(btf, t->name_off);

		if (name && *name)
			return name;  /* pointer into BTF string table */
		return "bytes";   /* anonymous struct: fall back to raw bytes */
	}

	default:
		return "bytes";
	}
}

/* -------------------------------------------------------------------------
 * Emit one proto3 message block for the struct at @type_id.
 * ---------------------------------------------------------------------- */

static void emit_message(const struct btf *btf, uint32_t type_id)
{
	const struct btf_type   *t = btf__type_by_id(btf, type_id);
	const struct btf_member *members;
	const char              *sname;
	uint16_t                 n, i;

	if (!t)
		return;

	sname = btf__str_by_offset(btf, t->name_off);
	if (!sname || !*sname)
		return;  /* skip anonymous structs */

	n       = BTF_INFO_VLEN(t->info);
	members = btf_members(t);

	printf("message %s {\n", sname);

	for (i = 0; i < n; i++) {
		const struct btf_member *m       = &members[i];
		const char              *fname   = btf__str_by_offset(btf, m->name_off);
		uint32_t                 fieldno = i + 1u;
		uint32_t                 bfsz;

		if (!fname)
			fname = "";

		/*
		 * Bitfields are skipped by kserial's encoder; they still occupy
		 * their slot in the BTF member array, so their field numbers are
		 * consumed and must not be reused.  Show them as comments so the
		 * field numbering stays self-documenting.
		 */
		bfsz = btf_member_bitfield_size(t, i);
		if (bfsz != 0) {
			printf("  // field %u: %s (bitfield:%u, not encoded by kserial)\n",
			       fieldno, fname, bfsz);
			continue;
		}

		printf("  %-12s %-32s = %u;\n",
		       btf_to_proto(btf, m->type), fname, fieldno);
	}

	printf("}\n\n");
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
	struct btf *btf;
	int         i, ret = 0;

	if (argc < 2) {
		fprintf(stderr,
			"usage: %s <struct_name> [struct_name2 ...]\n"
			"\n"
			"Generate a proto3 schema from vmlinux BTF that is wire-compatible\n"
			"with kserial's encoding.  Requires CONFIG_DEBUG_INFO_BTF=y.\n",
			argv[0]);
		return 1;
	}

	btf = btf__load_vmlinux_btf();
	if (!btf) {
		fprintf(stderr,
			"btf__load_vmlinux_btf() failed "
			"(need CONFIG_DEBUG_INFO_BTF=y)\n");
		return 1;
	}

	/* Resolve each requested struct and collect its dependencies. */
	for (i = 1; i < argc; i++) {
		int32_t tid = btf__find_by_name_kind(btf, argv[i],
						     BTF_KIND_STRUCT);
		if (tid < 0) {
			fprintf(stderr, "struct %s: not found in vmlinux BTF\n",
				argv[i]);
			ret = 1;
			continue;
		}
		collect(btf, (uint32_t)tid);
	}

	if (g_qlen == 0)
		goto out;

	/* Emit proto3 file header. */
	printf("// SPDX-License-Identifier: GPL-2.0-only\n");
	printf("// Generated by kserial_gen_proto from vmlinux BTF.\n");
	printf("//\n");
	printf("// Field numbers equal the 1-based BTF member index, matching\n");
	printf("// kserial's wire encoding.  Bitfields are not encoded by kserial\n");
	printf("// and are shown as comments with their field numbers noted.\n");
	printf("//\n");
	printf("// Wire type mapping (kserial <-> proto3):\n");
	printf("//   uint32, sint32    <-> VARINT  (LEB128; sint uses zigzag)\n");
	printf("//   uint64, sint64    <-> VARINT  (LEB128; sint uses zigzag)\n");
	printf("//   double            <-> I64     (8-byte little-endian)\n");
	printf("//   fixed64           <-> I64     (8-byte little-endian, pointer)\n");
	printf("//   float             <-> I32     (4-byte little-endian)\n");
	printf("//   bytes             <-> LEN     (varint length + data)\n");
	printf("//   nested message    <-> LEN     (varint length + sub-message)\n");
	printf("\n");
	printf("syntax = \"proto3\";\n\n");

	/* Emit all collected message types in dependency order. */
	for (i = 0; i < g_qlen; i++)
		emit_message(btf, g_queue[i]);

out:
	btf__free(btf);
	return ret;
}

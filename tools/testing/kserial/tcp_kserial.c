// SPDX-License-Identifier: GPL-2.0-only
/*
 * tcp_kserial - encode live TCP socket stats via /dev/kserial
 *
 * Usage:
 *   tcp_kserial [-a] <host> <port>
 *
 *   -a   print all fields, including zero-valued ones
 *
 * Connects to <host>:<port>, samples struct tcp_info via getsockopt(TCP_INFO),
 * writes the raw bytes to /dev/kserial for BTF-based encoding, then reads back
 * the protobuf-compatible message and decodes field names from vmlinux BTF.
 *
 * Useful comparison:  ss -ti dst <host>:<port>
 *
 * What ss -ti cannot show but this tool can:
 *   tcpi_probes, tcpi_backoff, tcpi_options, tcpi_fackets, tcpi_bytes_retrans,
 *   tcpi_dsack_dups, tcpi_rcv_ooopack, tcpi_snd_wnd, tcpi_rcv_wnd, tcpi_rehash,
 *   tcpi_total_rto, tcpi_total_rto_recoveries, tcpi_total_rto_time,
 *   tcpi_received_ce, tcpi_delivered_e{0,1,ce}_bytes
 *
 * What ss -ti shows but this tool cannot (bitfields, skipped by kserial):
 *   tcpi_snd_wscale, tcpi_rcv_wscale, tcpi_delivery_rate_app_limited
 *
 * Build:
 *   gcc -O2 -Wall -I../../../include/uapi -lbpf -o tcp_kserial tcp_kserial.c
 *
 * Notes:
 *   - Requires /dev/kserial (CONFIG_KSERIAL=y) and CONFIG_DEBUG_INFO_BTF=y.
 *   - Bitfield members in tcp_info (snd/rcv_wscale, delivery_rate_app_limited,
 *     fastopen_client_fail) are skipped by kserial; the BTF member indices for
 *     those fields are still counted in the field numbering, so the first u32
 *     field (tcpi_rto) appears as field 11 in the encoded stream.
 *   - By default, zero-valued fields are hidden; use -a to show all fields.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>   /* struct tcp_info, TCP_INFO, SOL_TCP */
#include <bpf/btf.h>       /* btf__load_vmlinux_btf, btf__find_by_name_kind */

/*
 * kserial UAPI definitions.
 * When building inside the kernel tree with -I../../../include/uapi the header
 * is available directly; fall back to inline constants for standalone builds.
 */
#if __has_include(<linux/kserial.h>)
#  include <linux/kserial.h>
#else
#define KSERIAL_MAGIC    0x4B534552U
#define KSERIAL_VERSION  1

#define KSERIAL_WIRE_VARINT  0
#define KSERIAL_WIRE_I64     1
#define KSERIAL_WIRE_LEN     2
#define KSERIAL_WIRE_I32     5

struct kserial_req {
	uint32_t magic;
	uint8_t  version;
	uint8_t  flags;
	uint16_t name_len;
	uint32_t data_len;
	uint32_t reserved;
};

struct kserial_msg_hdr {
	uint32_t magic;
	uint8_t  version;
	uint8_t  flags;
	uint16_t name_len;
	uint32_t msg_len;
	uint32_t reserved;
};
#endif /* linux/kserial.h */

/* -------------------------------------------------------------------------
 * LEB128 / varint helpers (same encoding as protobuf)
 * ---------------------------------------------------------------------- */

static int varint_decode(const uint8_t *buf, size_t size,
			 uint64_t *out_val, size_t *out_len)
{
	uint64_t result = 0;
	int      shift  = 0;
	size_t   i;

	for (i = 0; i < size && i < 10; i++) {
		result |= (uint64_t)(buf[i] & 0x7f) << shift;
		shift  += 7;
		if (!(buf[i] & 0x80)) {
			*out_val = result;
			*out_len = i + 1;
			return 0;
		}
	}
	return -1; /* truncated */
}

/*
 * Reverse the zigzag transform applied by kserial to SIGNED integer fields.
 * kserial only zigzag-encodes signed fields; unsigned fields are raw varints.
 */
static int64_t zigzag_decode(uint64_t v)
{
	return (int64_t)((v >> 1) ^ -(int64_t)(v & 1));
}

/*
 * Return true if the BTF member @m resolves to a signed integer type.
 * Walks TYPEDEF/CONST/VOLATILE/RESTRICT modifiers until reaching the
 * underlying INT, ENUM, or ENUM64.  ENUMs are signed iff their BTF kflag is 1.
 */
static bool btf_member_is_signed(const struct btf *btf,
				  const struct btf_member *m)
{
	const struct btf_type *t = btf__type_by_id(btf, m->type);

	while (t) {
		switch (BTF_INFO_KIND(t->info)) {
		case BTF_KIND_INT: {
			uint32_t enc = *(const uint32_t *)(t + 1);

			return !!(BTF_INT_ENCODING(enc) & BTF_INT_SIGNED);
		}
		case BTF_KIND_ENUM:
		case BTF_KIND_ENUM64:
			return !!BTF_INFO_KFLAG(t->info);
		case BTF_KIND_TYPEDEF:
		case BTF_KIND_CONST:
		case BTF_KIND_VOLATILE:
		case BTF_KIND_RESTRICT:
			t = btf__type_by_id(btf, t->type);
			break;
		default:
			return false;
		}
	}
	return false;
}

/* -------------------------------------------------------------------------
 * kserial write helper
 * ---------------------------------------------------------------------- */

static int kserial_send(int fd, const char *type_name,
			const void *data, size_t data_len)
{
	struct kserial_req  req;
	uint16_t            name_len = (uint16_t)(strlen(type_name) + 1);
	size_t              total    = sizeof(req) + name_len + data_len;
	uint8_t            *buf;
	ssize_t             n;

	buf = malloc(total);
	if (!buf)
		return -1;

	req.magic    = KSERIAL_MAGIC;
	req.version  = KSERIAL_VERSION;
	req.flags    = 0;
	req.name_len = name_len;
	req.data_len = (uint32_t)data_len;
	req.reserved = 0;

	memcpy(buf,                       &req,      sizeof(req));
	memcpy(buf + sizeof(req),         type_name, name_len);
	memcpy(buf + sizeof(req) + name_len, data,   data_len);

	n = write(fd, buf, total);
	free(buf);

	if (n < 0) {
		perror("write /dev/kserial");
		return -1;
	}
	return 0;
}

/* -------------------------------------------------------------------------
 * BTF-aware message decoder
 *
 * For each field in the encoded stream, looks up the corresponding BTF member
 * name so the output shows  .tcpi_rtt = 1234  instead of  field_24 = 1234.
 *
 * When show_zero is false, zero-valued fields are skipped (default behaviour).
 * Pass show_zero = true (via -a flag) to see every field including zeros.
 * ---------------------------------------------------------------------- */

static void decode_with_btf(const uint8_t *msg, size_t msg_size,
			     struct btf *btf, int show_zero)
{
	const struct kserial_msg_hdr *hdr;
	const char    *type_name;
	const uint8_t *p, *end;
	int32_t        type_id;
	const struct btf_type   *bt       = NULL;
	const struct btf_member *members  = NULL;
	uint16_t                 n_members = 0;

	if (msg_size < sizeof(*hdr)) {
		fprintf(stderr, "message too short (%zu bytes)\n", msg_size);
		return;
	}

	hdr = (const struct kserial_msg_hdr *)msg;
	if (hdr->msg_len > msg_size) {
		fprintf(stderr, "message length %u exceeds buffer size %zu\n",
			hdr->msg_len, msg_size);
		return;
	}
	type_name = (const char *)(hdr + 1);
	p         = (const uint8_t *)(hdr + 1) + hdr->name_len;
	end       = msg + hdr->msg_len;

	/* Look up the struct in vmlinux BTF to get member names. */
	type_id = btf__find_by_name_kind(btf, type_name, BTF_KIND_STRUCT);
	if (type_id > 0) {
		bt        = btf__type_by_id(btf, (uint32_t)type_id);
		members   = btf_members(bt);
		n_members = (uint16_t)BTF_INFO_VLEN(bt->info);
	} else {
		fprintf(stderr,
			"BTF: struct %s not found; printing raw field numbers\n",
			type_name);
	}

	printf("struct %s (kserial-encoded, %u bytes on wire):\n",
	       type_name, hdr->msg_len);

	while (p < end) {
		uint64_t tag;
		size_t   tag_len;
		uint32_t field_num, wire_type;

		if (varint_decode(p, (size_t)(end - p), &tag, &tag_len) < 0)
			break;

		p         += tag_len;
		field_num  = (uint32_t)(tag >> 3);
		wire_type  = (uint32_t)(tag & 0x7);

		/*
		 * Map field_num (1-based BTF member index) to the member name.
		 * Bitfield members are skipped by kserial but still occupy their
		 * slot in the BTF member array, so the index arithmetic is exact.
		 */
		const char *name = NULL;

		if (members && field_num >= 1 && field_num <= n_members)
			name = btf__str_by_offset(btf,
					members[field_num - 1].name_off);

		switch (wire_type) {
		case KSERIAL_WIRE_VARINT: {
			uint64_t raw;
			size_t   vlen;

			if (varint_decode(p, (size_t)(end - p), &raw, &vlen) < 0)
				goto done;
			p += vlen;

			/*
			 * kserial applies zigzag encoding only to SIGNED integer
			 * fields.  Unsigned fields are emitted as plain varints.
			 * Check the BTF type to select the right decode path so
			 * that unsigned values (e.g. all tcp_info counters) are
			 * not misinterpreted: zigzag_decode(1000) == 500, and
			 * zigzag_decode(1) == -1.
			 */
			bool is_signed = (members &&
					  field_num >= 1 &&
					  field_num <= n_members)
				? btf_member_is_signed(btf, &members[field_num - 1])
				: false;

			if (is_signed) {
				int64_t val = zigzag_decode(raw);

				if (!show_zero && val == 0)
					continue;
				if (name && *name)
					printf("  .%-30s = %" PRId64 "\n",
					       name, val);
				else
					printf("  .field_%-24u = %" PRId64 "\n",
					       field_num, val);
			} else {
				if (!show_zero && raw == 0)
					continue;
				if (name && *name)
					printf("  .%-30s = %" PRIu64 "\n",
					       name, raw);
				else
					printf("  .field_%-24u = %" PRIu64 "\n",
					       field_num, raw);
			}
			break;
		}

		case KSERIAL_WIRE_I64: {
			uint64_t val = 0;

			if ((size_t)(end - p) < 8)
				goto done;
			memcpy(&val, p, 8);
			p += 8;

			if (!show_zero && val == 0)
				continue;

			if (name && *name)
				printf("  .%-30s = %" PRIu64 "\n", name, val);
			else
				printf("  .field_%-24u = %" PRIu64 "\n",
				       field_num, val);
			break;
		}

		case KSERIAL_WIRE_I32: {
			/* No float fields in tcp_info; skip silently. */
			if ((size_t)(end - p) < 4)
				goto done;
			p += 4;
			break;
		}

		case KSERIAL_WIRE_LEN: {
			uint64_t slen;
			size_t   llen;

			if (varint_decode(p, (size_t)(end - p), &slen, &llen) < 0)
				goto done;
			p += llen + (size_t)slen;
			break;
		}

		default:
			goto done;
		}
	}
done:
	printf("}\n");
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
	struct addrinfo  hints, *res;
	struct tcp_info  ti;
	socklen_t        tilen;
	struct btf      *btf;
	uint8_t          rbuf[8192];
	ssize_t          n;
	int              sockfd = -1, kfd = -1, ret = 0;

	int show_zero = 0;
	int argoff    = 1;

	if (argc >= 2 && strcmp(argv[1], "-a") == 0) {
		show_zero = 1;
		argoff    = 2;
	}

	if (argc - argoff != 2) {
		fprintf(stderr, "usage: %s [-a] <host> <port>\n", argv[0]);
		return 1;
	}

	const char *host = argv[argoff];
	const char *port = argv[argoff + 1];

	/* --- Connect a TCP socket to the target host:port ----------------- */
	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;

	if (getaddrinfo(host, port, &hints, &res) != 0) {
		perror("getaddrinfo");
		return 1;
	}

	sockfd = socket(res->ai_family, SOCK_STREAM, 0);
	if (sockfd < 0) {
		perror("socket");
		freeaddrinfo(res);
		return 1;
	}

	if (connect(sockfd, res->ai_addr, res->ai_addrlen) < 0) {
		perror("connect");
		freeaddrinfo(res);
		ret = 1;
		goto out;
	}
	freeaddrinfo(res);

	printf("Connected to %s:%s\n\n", host, port);

	/* --- Sample struct tcp_info directly via getsockopt --------------- */
	tilen = sizeof(ti);
	if (getsockopt(sockfd, SOL_TCP, TCP_INFO, &ti, &tilen) < 0) {
		perror("getsockopt TCP_INFO");
		ret = 1;
		goto out;
	}

	/*
	 * Print a few key fields from the raw getsockopt result for comparison
	 * with the kserial-decoded output below and with  ss -ti.
	 */
	printf("Raw getsockopt(TCP_INFO) — key fields:\n");
	printf("  tcpi_state      = %u\n",     ti.tcpi_state);
	printf("  tcpi_rtt        = %u us\n",  ti.tcpi_rtt);
	printf("  tcpi_rttvar     = %u us\n",  ti.tcpi_rttvar);
	printf("  tcpi_snd_cwnd   = %u\n",     ti.tcpi_snd_cwnd);
	printf("  tcpi_snd_mss    = %u\n",     ti.tcpi_snd_mss);
	printf("  tcpi_retrans    = %u\n",     ti.tcpi_retrans);
	printf("\nCompare with:  ss -ti dst %s:%s\n\n", host, port);

	/* --- Load vmlinux BTF for member name resolution ------------------ */
	btf = btf__load_vmlinux_btf();
	if (!btf) {
		fprintf(stderr,
			"btf__load_vmlinux_btf() failed "
			"(need CONFIG_DEBUG_INFO_BTF=y)\n");
		ret = 1;
		goto out;
	}

	/* --- Encode via /dev/kserial -------------------------------------- */
	kfd = open("/dev/kserial", O_RDWR);
	if (kfd < 0) {
		perror("open /dev/kserial");
		ret = 1;
		goto out_btf;
	}

	/*
	 * Pass the BTF type name "tcp_info" and the raw getsockopt() bytes.
	 * The kernel walks the BTF members and emits a protobuf-compatible
	 * tag+value record for every non-bitfield field.
	 */
	if (kserial_send(kfd, "tcp_info", &ti, (size_t)tilen) < 0) {
		ret = 1;
		goto out_btf;
	}

	/* --- Read back and decode ----------------------------------------- */
	n = read(kfd, rbuf, sizeof(rbuf));
	if (n <= 0) {
		perror("read /dev/kserial");
		ret = 1;
		goto out_btf;
	}

	decode_with_btf(rbuf, (size_t)n, btf, show_zero);

out_btf:
	btf__free(btf);
out:
	if (kfd >= 0)
		close(kfd);
	if (sockfd >= 0)
		close(sockfd);
	return ret;
}

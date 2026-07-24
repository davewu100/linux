// SPDX-License-Identifier: GPL-2.0
/*
 * zswap_backend_bench - compare the crypto acomp and lib/zcomp backends for
 * the *same* compression algorithm, the way zswap uses each of them.
 *
 * This isolates the effect of the zswap "route software compressors through
 * lib/zcomp" change: for one algorithm (e.g. lzo) it runs
 *
 *   - the crypto acomp path exactly as zswap_compress()/zswap_decompress()
 *     drive it (scatterlist + crypto_acomp_{compress,decompress}), and
 *   - the lib/zcomp path exactly as the zcomp variants drive it
 *     (zcomp_stream_get + zcomp_{compress,decompress}),
 *
 * over the same batch of representative, mixed-compressibility pages, and
 * reports compress/decompress throughput (MB/s), per-op latency and the
 * achieved compression ratio for each backend.  Round-trip output is verified
 * against the source so a "faster" backend that corrupts data is caught.
 *
 * Load with e.g.:
 *   insmod zswap_backend_bench.ko alg=lzo iters=200000 npages=256
 * Results are printed to dmesg.  The module unloads itself after printing.
 *
 * IMPORTANT: pick an algorithm that both backends can service in *software*.
 * The point is to compare crypto-software vs zcomp for the same work; if an
 * offload driver provides the crypto side you are timing hardware instead.
 * Check /proc/crypto for the chosen name and confirm the crypto driver is the
 * generic software one (driver name ending in -generic / -scomp).
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/ktime.h>
#include <linux/scatterlist.h>
#include <linux/prandom.h>
#include <crypto/acompress.h>

#include "../../../lib/zcomp/zcomp.h"

static char *alg = "lzo";
module_param(alg, charp, 0444);
MODULE_PARM_DESC(alg, "compression algorithm shared by both backends (e.g. lzo, lz4, lz4hc, zstd, deflate, 842)");

static unsigned long iters = 200000UL;
module_param(iters, ulong, 0444);
MODULE_PARM_DESC(iters, "number of (de)compress iterations per backend (spread over the page set)");

static unsigned long warmup = 4096UL;
module_param(warmup, ulong, 0444);
MODULE_PARM_DESC(warmup, "warmup iterations (not timed) per backend");

static unsigned int npages = 256;
module_param(npages, uint, 0444);
MODULE_PARM_DESC(npages, "size of the representative page set");

static unsigned int seed = 0x5a5a5a5a;
module_param(seed, uint, 0444);
MODULE_PARM_DESC(seed, "PRNG seed for the page set (fixed for reproducibility)");

/*
 * Build one page of representative, mixed-compressibility content.  Real
 * anonymous memory is neither all-zero nor random; it is a mix of text-like,
 * structured-binary and near-random regions.  @kind selects the flavour so the
 * page set as a whole spans low/medium/high compression ratios.
 */
enum page_kind {
	PK_TEXT,	/* high ratio: word-like tokens with structure */
	PK_STRUCT,	/* medium ratio: pointer/counter-like binary records */
	PK_MIXED,	/* medium-low: half structured, half noisy */
	PK_NOISE,	/* low ratio: near-incompressible */
	PK__NR
};

static const char *const words[] = {
	"the ", "kernel ", "zswap ", "page ", "compress ", "memory ",
	"swap ", "cache ", "backend ", "crypto ", "data ", "store ",
	"and ", "of ", "a ", "to ", "in ", "is ", "for ", "with ",
};

static void fill_text(struct rnd_state *r, u8 *p)
{
	unsigned int i = 0;

	while (i < PAGE_SIZE) {
		const char *w = words[prandom_u32_state(r) % ARRAY_SIZE(words)];
		unsigned int n = strlen(w);

		if (i + n > PAGE_SIZE)
			n = PAGE_SIZE - i;
		memcpy(p + i, w, n);
		i += n;
	}
}

static void fill_struct(struct rnd_state *r, u8 *p)
{
	u32 *w = (u32 *)p;
	u32 base = prandom_u32_state(r);
	unsigned int i;

	/* counter/pointer-like records: mostly slowly-changing values */
	for (i = 0; i < PAGE_SIZE / sizeof(u32); i++) {
		if ((i & 0xf) == 0)
			base = prandom_u32_state(r) & 0xffff0000;
		w[i] = base + i;
	}
}

static void fill_noise(struct rnd_state *r, u8 *p, unsigned int len)
{
	u32 *w = (u32 *)p;
	unsigned int i;

	for (i = 0; i < len / sizeof(u32); i++)
		w[i] = prandom_u32_state(r);
}

static void fill_page(struct rnd_state *r, u8 *p, enum page_kind kind)
{
	switch (kind) {
	case PK_TEXT:
		fill_text(r, p);
		break;
	case PK_STRUCT:
		fill_struct(r, p);
		break;
	case PK_MIXED:
		/* structured first half, noisy second half (each PAGE_SIZE/2) */
		fill_struct(r, p);
		fill_noise(r, p + PAGE_SIZE / 2, PAGE_SIZE / 2);
		break;
	case PK_NOISE:
	default:
		fill_noise(r, p, PAGE_SIZE);
		break;
	}
}

/* Per-page source content plus a scratch slot for a pre-compressed copy. */
struct bench_page {
	u8 *src;		/* source bytes (also the crypto sglist input) */
	u8 *comp;		/* pre-compressed bytes (filled before decompress) */
	unsigned int clen;	/* length of comp; PAGE_SIZE means "stored as-is" */
};

static struct bench_page *pages;

static void free_pages_set(unsigned int n)
{
	unsigned int i;

	if (!pages)
		return;
	for (i = 0; i < n; i++) {
		free_page((unsigned long)pages[i].src);
		free_pages((unsigned long)pages[i].comp, 1);
	}
	kvfree(pages);
	pages = NULL;
}

static int alloc_pages_set(void)
{
	struct rnd_state rnd;
	unsigned int i;

	prandom_seed_state(&rnd, seed);

	pages = kvcalloc(npages, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	for (i = 0; i < npages; i++) {
		/*
		 * Both src and comp are handed to the crypto acomp path via
		 * sg_init_one(), which requires a linear-mapped, physically
		 * contiguous buffer (it does virt_to_page()).  kvmalloc() may
		 * fall back to vmalloc, whose pages are not contiguous and give
		 * a bogus scatterlist.  kmalloc() is linear but does not
		 * guarantee page alignment, so a PAGE_SIZE buffer can straddle a
		 * page boundary and the software (de)compressors walk off the
		 * end into an unmapped page (general protection fault).  Use
		 * page-aligned, order-0/1 allocations to keep them sg-safe.
		 */
		pages[i].src = (u8 *)__get_free_page(GFP_KERNEL);
		pages[i].comp = (u8 *)__get_free_pages(GFP_KERNEL, 1);
		if (!pages[i].src || !pages[i].comp) {
			free_pages_set(i + 1);
			return -ENOMEM;
		}
		/* cycle through the flavours to span the ratio range */
		fill_page(&rnd, pages[i].src, i % PK__NR);
	}
	return 0;
}

/*
 * Crypto acomp path, driven like zswap_compress()/zswap_decompress().
 * Accumulates compressed bytes (for the ratio) and verifies the round trip.
 */
static int run_crypto(u64 *comp_ns, u64 *decomp_ns, u64 *comp_bytes,
		      unsigned int *fail)
{
	struct crypto_acomp *tfm;
	struct acomp_req *req;
	struct crypto_wait wait;
	struct scatterlist si, so;
	u8 *cbuf, *dbuf;
	unsigned long it;
	ktime_t t0, t1;
	int ret = 0;

	tfm = crypto_alloc_acomp(alg, 0, 0);
	if (IS_ERR(tfm)) {
		pr_err("zswap_backend_bench: crypto alloc '%s' failed: %pe\n",
		       alg, tfm);
		return PTR_ERR(tfm);
	}
	pr_info("zswap_backend_bench: crypto driver=%s async=%d\n",
		crypto_acomp_driver_name(tfm), acomp_is_async(tfm));

	req = acomp_request_alloc(tfm);
	cbuf = kmalloc(2 * PAGE_SIZE, GFP_KERNEL);
	dbuf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!req || !cbuf || !dbuf) {
		ret = -ENOMEM;
		goto out;
	}
	crypto_init_wait(&wait);
	acomp_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				   crypto_req_done, &wait);

	/* warmup (result intentionally discarded) */
	for (it = 0; it < warmup; it++) {
		struct bench_page *bp = &pages[it % npages];

		sg_init_one(&si, bp->src, PAGE_SIZE);
		sg_init_one(&so, cbuf, 2 * PAGE_SIZE);
		acomp_request_set_params(req, &si, &so, PAGE_SIZE, 2 * PAGE_SIZE);
		crypto_wait_req(crypto_acomp_compress(req), &wait);
	}

	/* timed compress */
	*comp_bytes = 0;
	t0 = ktime_get();
	for (it = 0; it < iters; it++) {
		struct bench_page *bp = &pages[it % npages];

		sg_init_one(&si, bp->src, PAGE_SIZE);
		sg_init_one(&so, cbuf, 2 * PAGE_SIZE);
		acomp_request_set_params(req, &si, &so, PAGE_SIZE, 2 * PAGE_SIZE);
		ret = crypto_wait_req(crypto_acomp_compress(req), &wait);
		if (ret)
			break;
		*comp_bytes += req->dlen;
	}
	t1 = ktime_get();
	*comp_ns = ktime_to_ns(ktime_sub(t1, t0));
	if (ret) {
		pr_err("zswap_backend_bench: crypto compress failed: %d\n", ret);
		goto out;
	}

	/* pre-compress every page once (not timed) so decompress is isolated */
	for (it = 0; it < npages; it++) {
		struct bench_page *bp = &pages[it];

		sg_init_one(&si, bp->src, PAGE_SIZE);
		sg_init_one(&so, cbuf, 2 * PAGE_SIZE);
		acomp_request_set_params(req, &si, &so, PAGE_SIZE, 2 * PAGE_SIZE);
		ret = crypto_wait_req(crypto_acomp_compress(req), &wait);
		if (ret)
			goto out;
		/*
		 * Mirror what zswap does: a page that does not compress below
		 * PAGE_SIZE is stored as-is rather than kept in a compressed
		 * form that would not round-trip.  The zcomp side does the same,
		 * so both backends are verified on equal terms.
		 */
		if (req->dlen < PAGE_SIZE) {
			bp->clen = req->dlen;
			memcpy(bp->comp, cbuf, bp->clen);
		} else {
			bp->clen = PAGE_SIZE;
			memcpy(bp->comp, bp->src, PAGE_SIZE);
		}
	}

	/* timed decompress + verification */
	t0 = ktime_get();
	for (it = 0; it < iters; it++) {
		struct bench_page *bp = &pages[it % npages];

		if (bp->clen < PAGE_SIZE) {
			sg_init_one(&si, bp->comp, bp->clen);
			sg_init_one(&so, dbuf, PAGE_SIZE);
			acomp_request_set_params(req, &si, &so, bp->clen,
						 PAGE_SIZE);
			ret = crypto_wait_req(crypto_acomp_decompress(req),
					      &wait);
			if (ret)
				break;
			if (req->dlen != PAGE_SIZE ||
			    memcmp(dbuf, bp->src, PAGE_SIZE))
				(*fail)++;
		} else {
			/* stored as-is: zswap just copies it back */
			memcpy(dbuf, bp->comp, PAGE_SIZE);
			if (memcmp(dbuf, bp->src, PAGE_SIZE))
				(*fail)++;
		}
	}
	t1 = ktime_get();
	*decomp_ns = ktime_to_ns(ktime_sub(t1, t0));

out:
	kfree(dbuf);
	kfree(cbuf);
	if (req)
		acomp_request_free(req);
	crypto_free_acomp(tfm);
	return ret;
}

/*
 * lib/zcomp path, driven like the zcomp variants in zswap.c.
 */
static int run_zcomp(u64 *comp_ns, u64 *decomp_ns, u64 *comp_bytes,
		     unsigned int *fail)
{
	struct zcomp *comp;
	struct zcomp_strm *zstrm;
	u8 *cbuf, *dbuf;
	unsigned long it;
	ktime_t t0, t1;
	int ret = 0;

	if (!zcomp_lookup_backend_name(alg)) {
		pr_info("zswap_backend_bench: zcomp has no backend '%s', skipping zcomp side\n",
			alg);
		return -ENOENT;
	}

	comp = zcomp_create(alg, &(struct zcomp_params){
		.level = ZCOMP_PARAM_NOT_SET,
		.deflate.winbits = ZCOMP_PARAM_NOT_SET,
	});
	if (IS_ERR(comp)) {
		pr_err("zswap_backend_bench: zcomp_create('%s') failed: %pe\n",
		       alg, comp);
		return PTR_ERR(comp);
	}

	cbuf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	dbuf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!cbuf || !dbuf) {
		ret = -ENOMEM;
		goto out;
	}

	/* warmup */
	for (it = 0; it < warmup; it++) {
		struct bench_page *bp = &pages[it % npages];
		unsigned int clen;

		zstrm = zcomp_stream_get(comp);
		if (!zcomp_compress(comp, zstrm, bp->src, &clen))
			memcpy(cbuf, zstrm->buffer, min_t(unsigned int, clen, PAGE_SIZE));
		zcomp_stream_put(zstrm);
	}

	/* timed compress */
	*comp_bytes = 0;
	t0 = ktime_get();
	for (it = 0; it < iters; it++) {
		struct bench_page *bp = &pages[it % npages];
		unsigned int clen;

		zstrm = zcomp_stream_get(comp);
		ret = zcomp_compress(comp, zstrm, bp->src, &clen);
		if (!ret)
			*comp_bytes += clen;
		zcomp_stream_put(zstrm);
		if (ret)
			break;
	}
	t1 = ktime_get();
	*comp_ns = ktime_to_ns(ktime_sub(t1, t0));
	if (ret) {
		pr_err("zswap_backend_bench: zcomp compress failed: %d\n", ret);
		goto out;
	}

	/* pre-compress every page once (not timed) so decompress is isolated */
	for (it = 0; it < npages; it++) {
		struct bench_page *bp = &pages[it];
		unsigned int clen;

		zstrm = zcomp_stream_get(comp);
		ret = zcomp_compress(comp, zstrm, bp->src, &clen);
		if (!ret) {
			/* zswap stores an incompressible page (>=PAGE_SIZE) as-is */
			bp->clen = min_t(unsigned int, clen, PAGE_SIZE);
			if (clen < PAGE_SIZE)
				memcpy(bp->comp, zstrm->buffer, clen);
			else
				memcpy(bp->comp, bp->src, PAGE_SIZE);
		}
		zcomp_stream_put(zstrm);
		if (ret)
			goto out;
	}

	/* timed decompress + verification */
	t0 = ktime_get();
	for (it = 0; it < iters; it++) {
		struct bench_page *bp = &pages[it % npages];

		zstrm = zcomp_stream_get(comp);
		if (bp->clen < PAGE_SIZE) {
			ret = zcomp_decompress(comp, zstrm, bp->comp, bp->clen, dbuf);
			if (!ret && memcmp(dbuf, bp->src, PAGE_SIZE))
				(*fail)++;
		} else {
			/* stored as-is: zswap just copies it back */
			memcpy(dbuf, bp->comp, PAGE_SIZE);
			if (memcmp(dbuf, bp->src, PAGE_SIZE))
				(*fail)++;
		}
		zcomp_stream_put(zstrm);
		if (ret)
			break;
	}
	t1 = ktime_get();
	*decomp_ns = ktime_to_ns(ktime_sub(t1, t0));

out:
	kfree(dbuf);
	kfree(cbuf);
	zcomp_destroy(comp);
	return ret;
}

static void report(const char *name, u64 comp_ns, u64 decomp_ns,
		   u64 comp_bytes)
{
	u64 total_in = (u64)iters * PAGE_SIZE;
	u64 c_mbps = 0, d_mbps = 0;
	u64 ratio_x1000 = 0;

	/* MB/s = bytes / ns * 1e9 / 1e6 = bytes * 1000 / ns */
	if (comp_ns)
		c_mbps = div64_u64(total_in * 1000, comp_ns);
	if (decomp_ns)
		d_mbps = div64_u64(total_in * 1000, decomp_ns);
	if (comp_bytes)
		ratio_x1000 = div64_u64(total_in * 1000, comp_bytes);

	pr_info("zswap_backend_bench: %-6s  compress %6llu MB/s (%llu ns/op)  decompress %6llu MB/s (%llu ns/op)  ratio %llu.%03llu:1\n",
		name, c_mbps, div64_u64(comp_ns, iters),
		d_mbps, div64_u64(decomp_ns, iters),
		ratio_x1000 / 1000, ratio_x1000 % 1000);
}

static int __init bench_init(void)
{
	u64 cc_ns = 0, cd_ns = 0, cbytes = 0;
	u64 zc_ns = 0, zd_ns = 0, zbytes = 0;
	unsigned int cfail = 0, zfail = 0;
	int cret, zret;

	if (!npages || !iters) {
		pr_err("zswap_backend_bench: npages and iters must be > 0\n");
		return -EINVAL;
	}

	pr_info("zswap_backend_bench: alg=%s iters=%lu npages=%u seed=0x%x\n",
		alg, iters, npages, seed);

	if (alloc_pages_set())
		return -ENOMEM;

	cret = run_crypto(&cc_ns, &cd_ns, &cbytes, &cfail);
	zret = run_zcomp(&zc_ns, &zd_ns, &zbytes, &zfail);

	pr_info("zswap_backend_bench: ==== results for '%s' (%u pages, %lu iters) ====\n",
		alg, npages, iters);
	if (!cret)
		report("crypto", cc_ns, cd_ns, cbytes);
	else
		pr_info("zswap_backend_bench: crypto side unavailable (%d)\n", cret);
	if (!zret)
		report("zcomp", zc_ns, zd_ns, zbytes);
	else
		pr_info("zswap_backend_bench: zcomp side unavailable (%d)\n", zret);

	if (!cret && !zret) {
		if (cc_ns && zc_ns)
			pr_info("zswap_backend_bench: zcomp compress is %llu.%02llu%% of crypto time (lower is faster)\n",
				div64_u64(zc_ns * 100, cc_ns),
				div64_u64(zc_ns * 10000, cc_ns) % 100);
		if (cd_ns && zd_ns)
			pr_info("zswap_backend_bench: zcomp decompress is %llu.%02llu%% of crypto time (lower is faster)\n",
				div64_u64(zd_ns * 100, cd_ns),
				div64_u64(zd_ns * 10000, cd_ns) % 100);
	}
	if (cfail || zfail)
		pr_err("zswap_backend_bench: ROUND-TRIP MISMATCH crypto=%u zcomp=%u (results invalid)\n",
		       cfail, zfail);

	free_pages_set(npages);

	/* Return an error so the module unloads itself after printing. */
	return -ECANCELED;
}

static void __exit bench_exit(void)
{
}

module_init(bench_init);
module_exit(bench_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("zswap crypto-acomp vs lib/zcomp backend micro-benchmark");

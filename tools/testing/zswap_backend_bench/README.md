# zswap backend micro-benchmark: crypto acomp vs lib/zcomp

Compares the **crypto acomp** path and the **lib/zcomp** path for the *same*
compression algorithm, driven exactly the way `mm/zswap.c` drives each backend.
This measures the effect of the "route software compressors through lib/zcomp"
change: for a shared algorithm (e.g. `lzo`) it reports compress/decompress
throughput, per-op latency and the achieved compression ratio for both
backends, over the same batch of representative mixed-compressibility pages.

It is a self-unloading kernel module; all output goes to `dmesg`.

## What it does

For the algorithm given by `alg=`:

1. Builds a fixed, reproducible set of `npages` pages spanning the realistic
   compressibility range (text-like, structured-binary, mixed, near-random) —
   *not* all-zero pages, which would flatter every compressor equally.
2. Runs the **crypto** side like `zswap_compress()/zswap_decompress()`
   (scatterlist + `crypto_acomp_{compress,decompress}`).
3. Runs the **zcomp** side like the zcomp variants in `zswap.c`
   (`zcomp_stream_get` + `zcomp_{compress,decompress}`).
4. Verifies every round trip (`decompress(compress(x)) == x`); a mismatch
   marks the results invalid.
5. Prints MB/s, ns/op and ratio for each backend and the zcomp-vs-crypto ratio.

## Requirements

The **target** kernel (the one you boot and load the module into) must be
built from this tree with, for the algorithm under test, both backends present:

```
CONFIG_ZSWAP=y            # implies CONFIG_ZCOMP=y
CONFIG_MODULES=y
# for alg=lzo:
CONFIG_ZCOMP_BACKEND_LZO=y
CONFIG_CRYPTO_LZO=y
# for alg=lz4:   CONFIG_ZCOMP_BACKEND_LZ4=y  + CONFIG_CRYPTO_LZ4=y
# for alg=lz4hc: CONFIG_ZCOMP_BACKEND_LZ4HC=y + CONFIG_CRYPTO_LZ4HC=y
# for alg=zstd:  CONFIG_ZCOMP_BACKEND_ZSTD=y + CONFIG_CRYPTO_ZSTD=y
# for alg=deflate: CONFIG_ZCOMP_BACKEND_DEFLATE=y + CONFIG_CRYPTO_DEFLATE=y
# for alg=842:   CONFIG_ZCOMP_BACKEND_842=y  + CONFIG_CRYPTO_842=y
```

Only the affected (shared) algorithms are meaningful here: **lzo, lzorle
("lzo-rle"), lz4, lz4hc, zstd, deflate, 842**.

> IMPORTANT: to compare crypto-*software* vs zcomp, make sure no offload driver
> provides the crypto side, otherwise the crypto numbers are hardware. Check
> `/proc/crypto` for the chosen name; the crypto driver name should end in
> `-generic` or `-scomp`.

## Build

```sh
make -C tools/testing/zswap_backend_bench KDIR=$PWD
```

`KDIR` must point at the built kernel tree that produced the *running* kernel
(so `Module.symvers` exports the `zcomp_*` symbols). Building against a stale
`Module.symvers` gives `modpost: "zcomp_create" ... undefined`.

## Run (on the target running that kernel, as root)

```sh
./run.sh                 # sweeps the shared algorithms, default sizes
# or a single run:
insmod zswap_backend_bench.ko alg=lzo iters=200000 npages=256
dmesg | tail -n 20
```

Parameters:

| param    | default      | meaning                                        |
|----------|--------------|------------------------------------------------|
| `alg`    | `lzo`        | algorithm shared by both backends              |
| `iters`  | `200000`     | timed (de)compress iterations per backend      |
| `warmup` | `4096`       | untimed warmup iterations                      |
| `npages` | `256`        | size of the representative page set            |
| `seed`   | `0x5a5a5a5a` | PRNG seed (fixed → reproducible page set)      |

## Reading the output

```
zswap_backend_bench: ==== results for 'lzo' (256 pages, 200000 iters) ====
zswap_backend_bench: crypto  compress  1234 MB/s (3311 ns/op)  decompress  4567 MB/s (896 ns/op)  ratio 2.731:1
zswap_backend_bench: zcomp   compress  1301 MB/s (3141 ns/op)  decompress  4712 MB/s (868 ns/op)  ratio 2.731:1
zswap_backend_bench: zcomp compress is 94.87% of crypto time (lower is faster)
zswap_backend_bench: zcomp decompress is 96.80% of crypto time (lower is faster)
```

- The **ratio** should be identical for both backends (same algorithm, same
  data) — a good sanity check that they really do the same compression.
- Compare compress/decompress MB/s and the `% of crypto time` lines to see
  whether routing through zcomp is faster, slower or neutral for this
  algorithm on this CPU.

## Comparing "before vs after" the zswap change

Because both backends are exercised in a single run, there is no need to
rebuild "before" and "after" kernels: the **crypto** numbers are the
pre-change path (zswap used crypto for everything) and the **zcomp** numbers
are the post-change path for the same algorithm. The per-op and MB/s deltas
are the performance effect of the change.

---

# zram_comp_bench.sh — portable, cross-environment compression benchmark

`zswap_backend_bench.ko` needs a kernel built from this tree and is the precise
tool: it isolates crypto-acomp vs lib/zcomp for the same algorithm in one run.
When you cannot load a custom module (e.g. a stock/appliance kernel, or when
you just want a quick cross-environment comparison), use the pure-shell
`zram_comp_bench.sh` instead.

It drives a private `zram` device, which uses the same software compressors
(lib/zcomp on a kernel that has it), and reports the achieved compression ratio
and end-to-end write/read throughput for each algorithm the kernel supports,
over the same reproducible mixed-compressibility payload (text-like plus
near-random, not all-zero).

```sh
sudo ./zram_comp_bench.sh [SIZE_MB] [ALG ...]
# examples:
sudo ./zram_comp_bench.sh              # 128 MB, all supported algorithms
sudo ./zram_comp_bench.sh 256 zstd deflate
```

It only needs `dd`, `awk`, `sync` and the `zram` module, reads the algorithm
list from the kernel (so it adapts to whatever the target supports), and
resets/unloads zram when done. Because it touches neither swap nor real memory
pressure, it is safe to run on production-ish boxes.

### How the two tools relate

| Tool | Measures | Needs | Use when |
|------|----------|-------|----------|
| `zswap_backend_bench.ko` | crypto acomp vs lib/zcomp, isolated | kernel built from this tree | proving the zswap change on a dev kernel |
| `zram_comp_bench.sh` | per-algorithm ratio + throughput via zram | any kernel with zram | quick, portable, cross-environment comparison |

To compare "crypto software" against "zcomp": run `zram_comp_bench.sh` on a
kernel whose zram uses the old crypto-based compression and again on a kernel
whose zram uses lib/zcomp; the ratio should match per algorithm (same
algorithm) while the throughput delta reflects the backend. Keep the CPU
constant (ideally the same box, different kernels) since throughput is
CPU-bound.

---

## Sample results

Two complementary measurements from a QEMU/KVM run (`-cpu host`, 4 vCPU /
4 GiB) on a kernel built from this tree with, for each algorithm, both the
zcomp backend and the matching crypto software algorithm enabled.  Only the
five shared algorithms available in that build are shown (lzo, lzo-rle, lz4,
zstd, deflate).

### 1. zswap_backend_bench.ko — crypto acomp vs lib/zcomp (compression only)

Per-op compression/decompression of a single page, isolating the backend from
the rest of the swap path.  Each algorithm was run twice; the table uses the
second (better warmed-up) run.

| alg     | ratio | crypto compress   | zcomp compress    | compress delta      | crypto decompress | zcomp decompress | decompress delta |
|---------|-------|-------------------|-------------------|---------------------|-------------------|------------------|------------------|
| lzo     | 1.177 | 955 MB/s (4287ns) | 1095 MB/s (3738ns)| zcomp ~13% faster   | 3028 MB/s         | 3017 MB/s        | ~even            |
| lzo-rle | 1.176 | 1064 MB/s (3849ns)| 1073 MB/s (3814ns)| ~even (zcomp 99.1%) | 2955 MB/s         | 2944 MB/s        | ~even            |
| lz4     | 1.140 | 978 MB/s (4187ns) | 992 MB/s (4127ns) | ~even (zcomp 98.6%) | 4190 MB/s         | 4194 MB/s        | ~even            |
| zstd    | 1.291 | 72 MB/s (56188ns) | 210 MB/s (19478ns)| zcomp ~2.9x faster  | 598 MB/s          | 602 MB/s         | ~even            |
| deflate | 1.674 | 31 MB/s (130399ns)| 31 MB/s (131480ns)| ~even (zcomp 100.8%)| 274 MB/s          | 267 MB/s         | ~3% slower       |

Observations:

- **Compression ratio is identical** on both backends for every algorithm,
  confirming that routing through zcomp does not change the produced data
  (the module also verifies every round trip).
- **Decompression is within ~3%** on both paths for all algorithms.
- **Compression through zcomp is faster than or on par with crypto acomp**, and
  the gain scales with how much per-request setup the algorithm needs: zstd sees
  the largest speedup (~2.9x), lzo gains ~13%, while lz4/lzo-rle/deflate are
  ~even.
- **The zstd outlier is a real, code-level difference in compression-context
  (cctx) reuse, not a benchmark artefact.** The crypto zstd driver
  (`crypto/zstd.c`) re-initialises the cctx on *every* request
  (`zstd_init_cctx()`/`zstd_init_cstream()` in `zstd_compress_one()`), even
  though its workspace memory is reused; zstd's cctx init resets a fairly large
  internal state, so it is expensive per op.  The lib/zcomp zstd backend
  (`lib/zcomp/backend_zstd.c`) initialises the cctx once, when the per-CPU
  stream is created, and the hot path just calls `zstd_compress_cctx()` on the
  already-initialised context.  Light, near-stateless algorithms (lzo, lz4)
  have a trivial init, so they show little or no difference; only zstd's heavy
  init makes the gap large.  This is not specific to the benchmark: real zswap
  reuses the crypto *tfm* per CPU, but the zstd driver underneath still
  re-inits the cctx per request, so the same difference applies in production.

### 2. zram_comp_bench.sh — end-to-end via zram (lib/zcomp backend)

Whole write(compress)/read(decompress) path of a private zram device, 256 MiB
of mixed data (180 MiB text-like + 76 MiB near-random) pushed with `dd`.  One
untimed warmup run first, then each algorithm run twice using the second run;
timed with `/proc/uptime` (centisecond resolution).  These numbers include the
full block-I/O and `dd` overhead, so they are lower than the per-op figures
above and are meant for comparing algorithms (and backends across kernels), not
as absolute compressor speed.

| alg     | ratio | write (compress) | read (decompress) | write MB/s | read MB/s |
|---------|-------|------------------|-------------------|------------|-----------|
| lzo     | 2.848 | 0.45 s           | 0.20 s            | 568.9      | 1280.0    |
| lzo-rle | 2.845 | 0.40 s           | 0.23 s            | 640.0      | 1113.0    |
| lz4     | 2.872 | 0.50 s           | 0.26 s            | 512.0      | 984.6     |
| zstd    | 3.002 | 0.85 s           | 0.31 s            | 301.2      | 825.8     |
| deflate | 3.016 | 5.61 s           | 0.41 s            | 45.6       | 624.4     |

Observations:

- The classic speed/ratio trade-off is visible end-to-end: **lzo/lzo-rle/lz4
  compress fastest** (~0.4-0.5 s) at a lower ratio (~2.85), **zstd** is a good
  balance (ratio 3.00 at 0.85 s), and **deflate is by far the slowest to
  compress** (5.61 s, an order of magnitude) for a ratio (3.02) barely above
  zstd.
- Decompression (read) is fast and close for all algorithms (0.20-0.41 s).
- The compression ratios differ from table 1 only because the payload differs
  (256 MiB mixed file here vs the per-page set in the module); within each table
  the ratio is what matters.

Caveats:

- These are **virtual-machine** numbers. `-cpu host` passes the physical CPU
  through, so the *relative* comparisons from a single run are meaningful, but
  the **absolute MB/s do not represent bare-metal performance**.
- The zram table reflects the lib/zcomp backend only (that is what this kernel's
  zram uses); to compare against a crypto-based zram, run `zram_comp_bench.sh`
  on a kernel whose zram still uses crypto, keeping the CPU constant.
- lz4hc and 842 are not shown because that particular build did not enable those
  backends; enable the matching `CONFIG_ZCOMP_BACKEND_*` and `CONFIG_CRYPTO_*`
  to measure them.

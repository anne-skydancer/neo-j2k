<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026, Anne Skydancer (neo-j2k) -->

# neo-j2k

> **[Download neo-j2k 0.1.2 for Windows x64 (ZIP)](https://github.com/anne-skydancer/neo-j2k/releases/download/v0.1.2/neoj2k-viewer-0.1.2-win-x64.zip)**
>
> Prerelease for 64-bit Windows viewers that load a separate `openjp2.dll`. Requires AVX2. Not for the official Firestorm
> releases, which link Kakadu statically and have no `openjp2.dll`: it works in Firestorm builds compiled without Kakadu.
> [Installation instructions](modules/openjp2/INSTALL.md) · [Release notes and checksums](https://github.com/anne-skydancer/neo-j2k/releases/tag/v0.1.2)

A JPEG 2000 Part 1 codec (decode and encode, lossless 5/3 and lossy 9/7) in C++20, built as a static library or
as a shared library (`neoj2k.dll`, `libneoj2k.so`). Its arithmetic is OpenJPEG 2.5.4's, so its decodes are
bit-identical to OpenJPEG 2.5.4 on x86-64 through OpenJPEG's SSE 9/7 path (`opj_includes.h` defines `__SSE__` on
x86-64; its scalar path, which AArch64 takes, may be fused into FMA by GCC and Clang), both built and compared
without floating-point contraction (`-ffp-contract=off`; MSVC `/fp:precise` without `/fp:contract`), lossy 9/7
included. Everything around the arithmetic is rebuilt for small images, defined partial decodes and bounded
resources: the decoder limits bound the work and the memory a codestream can ask for, checked from its headers
before anything is allocated. A decoder has none unless it is given some: without them it decodes any size the
format allows. The profiles (`doc/api.md`) are Second Life textures (1,024 tiles, 8 components, 64 layers, 2,048
px a side at the decoded resolution, 256 MiB of working memory) and, as a build module, OpenSim textures (the
same with 16,384 px a side). Where OpenJPEG departs from the standard, neo-j2k follows the standard and lists the
departure with its test (`doc/departures.md`). The [native Rust crate](https://github.com/anne-skydancer/neoj2k-rust) implements the same core
codec and compares its output against the C++ library.

Licence: BSD-2-Clause (`LICENSE`). Code ported from OpenJPEG keeps its own BSD-2-Clause notices (`NOTICE.md`,
`LICENSE.OpenJPEG`).

## State

Stage 0, the skeleton: the build, the draft API (OpenJPEG's API under `nj2k_` names, with a compile-time `opj_*`
compatibility header), CI, the test oracle, the corpus and its reference tables, the differential, cut-point,
layer and conformance harness, fuzz targets and the bench.

Stage 1b, the header and the limits: streams, the main and tile-part header model and the JP2/JPH box reader,
the decoder limits (the Second Life profile built in, an OpenSim module), statuses, the allocator hook, the
reduce policy and the decoded dimensions.

Stage 1c, the decoder: whole and partial codestreams decode bit-identical to the oracle at every reduce level and
every cut point (the stage 1a semantics: every complete codeword segment kept), through OpenJPEG's API or from
memory into the caller's buffers (`nj2k_read_header_memory`, `nj2k_decode_memory`: one decoder per thread, the
main header parsed once, the caller's bytes read in place and never written, no heap call once warmed up for
images up to 256 x 256). Large images decode too, on one thread as fast as OpenJPEG or faster (`doc/bench.md`).
Decoding by tile and decode areas come later.

Stages 2-4, speed on one thread with the same output: the colour transform, DC shift and copy into the
caller's buffers fused into one pass (the viewer's 8-bit image in registers); the inverse wavelet row-wise on
large levels, its vertical steps in one sweep; the MQ decoder's renormalisation by count-leading-zeros; no
heap call once warmed up, from the first repeat of a request on (`doc/bench.md`).

Stage 5, threads in one image: the library starts no thread; a host lends it an executor
(`nj2k_decoder_set_executor` or `nj2k_encoder_set_executor`), and a tile whose largest component has 2^18 samples or more is split across it,
with the same output at any thread count. The host lends only when it has idle cores (stage 5b,
`doc/api.md`).

Stage 6d, the MQ decoder (MQ-NG), with the same coefficients: a context's state as one 32-bit word, the decision by
selects with the renormalisation always run, and the cleanup pass's runs of empty columns taken at once. One-thread
Tier-1 time changes (minus is faster) by -29.0 % to -28.0 % lossless and -22.0 % to -15.0 % lossy with MSVC (-23.0 % and -19.0 % to -16.0 % with clang-cl),
the whole decode -29.0 % to -27.0 % lossless and -20.0 % to -12.0 % lossy (MSVC AVX2; MSVC SCALAR -28.0 % to -26.0 % and -18.0 % to -12.0 %,
clang-cl -24.0 % to -23.0 % and -19.0 % to -14.0 %; `doc/bench.md`, "Stage 6d").

The encoder takes the settings the Second Life viewer encodes with (RLCP, 64 x 64 code-blocks, 5/3 lossless or
9/7 with rate layers, the colour transform, 8-bit components of up to 4,096 samples a side), through OpenJPEG's
encoder API, with codestreams byte for byte OpenJPEG's (`encode_unit`, the encode fuzz target); the rest of
OpenJPEG's encoder gives `NJ2K_E_UNSUPPORTED` (the j2k-extra module). From 256 x 256 it uses parallel component
wavelets and Tier-1 code-block batches when the host lends an executor; smaller uploads remain serial. Codestream
bytes are identical at every concurrency (`doc/api.md`, `doc/bench.md`).

The openjp2 module: OpenJPEG's API as the library a viewer already loads (`openjp2.dll`, `libopenjp2.so.7`), neo-j2k
inside, so a viewer that talks to OpenJPEG takes neo-j2k with nothing recompiled or relinked: the 55 functions of
OpenJPEG 2.5.4's header, the threads `opj_codec_set_threads` starts, and `opj_version()` saying "nj2k" (the five characters a viewer's texture cache header has room for) (`doc/api.md`, build option `NJ2K_MODULE_OPENJP2`). The library goes into a viewer's folder under the name `openjp2.dll`, which is only the name the viewer asks for:
it is neo-j2k, not OpenJPEG, and its file properties say so. It is packaged as a zip
(`neoj2k-viewer-<version>-win-x64.zip`, named for neo-j2k) with its
own install instructions (`modules/openjp2/INSTALL.md`, `tools/package_openjp2.py`); an installer is planned.

Version: 0.1.0 is the first release; 0.1.1 adds the floating-point hotfix (the output no longer depends on the host's
floating-point environment) and the openjp2 module; 0.1.2 fixes the module's `opj_version()`, whose 0.1.1 string was too long
for Firestorm's texture cache header and stopped the viewer at start. The Rust crate uses the version of the C++
library it was tested against.

## How fast

All numbers are measured, against OpenJPEG 2.5.4 at the oracle pin (`doc/oracle.md`), built with the same flags.
Every decode and encode in these tables gives the oracle's output. The machine is a Ryzen 9 9950X3D (8 cores, 8
threads used), Windows, MSVC 14.51 RelWithDebInfo, AVX2. It was not quiet: other jobs shared it, so the ratios
are more reliable than the milliseconds. Each section names its load. The full tables, with every run and how
to reproduce them, are in `doc/bench.md`.

### OpenJPEG 2.5.4 versus neo-j2k C++

This is the short comparison for readers looking for “OpenJPEG2000 versus neo-j2k.” OpenJPEG is the upstream
JPEG 2000 implementation; neo-j2k C++ is measured against the pinned OpenJPEG 2.5.4 oracle with identical
outputs. Times are medians from the detailed tables below; lower is faster. The change is neo-j2k C++'s time against OpenJPEG's (minus is faster).

| Operation | Threads | OpenJPEG | neo-j2k C++ | Change vs OpenJPEG | Result |
|---|---:|---:|---:|---:|---|
| Decode 64² whole | 1 | 373 µs | 316 µs | -15.0 % | neo-j2k 1.18× faster |
| Decode 1,024² whole | 1 | 37.176 ms | 26.158 ms | -30.0 % | neo-j2k 1.42× faster |
| Decode 4,096², one-tile 9/7 | 1 | 392 ms | 234 ms | -40.0 % | neo-j2k 1.68× faster |
| Decode 4,096², one-tile 9/7 | 8 | 136 ms | 46 ms | -66.0 % | neo-j2k 2.96× faster |
| Encode 1,024² RGB, lossy | 1 | 240.4 ms | 183.7 ms | -24.0 % | neo-j2k 1.31× faster |
| Encode 1,024² RGB, lossless | 1 | 253.9 ms | 178.8 ms | -30.0 % | neo-j2k 1.42× faster |

The encode rows are the equal one-thread comparison, measured after the MQ coder redesign (`doc/bench.md`,
"Stage 6c"; before it neo-j2k took 284.5 and 295.9 ms against OpenJPEG's 250.0 and 262.8). They do not include
the branch-free preprocessing of "Stage 6e" (about 3.3 to 4.4 % off the encode with MSVC) and the cleanup pass's fixed
column mask of "Stage 6f" (a further 4 to 6 % with MSVC; whole encode -6.2 % to -4.1 % against before; the kernel is 5 to 6 %
faster on six Linux builds, GCC 14, GCC 15 and Clang, AVX2 and scalar). The refinement pass's pair list ("Stage 6g") takes the MSVC kernel a further -6.0 % to -3.6 % on one machine. On Linux (same-machine kernel runs), GCC 14 and GCC 15 gain -7.5 % to -4.9 % on Intel Xeon, in lossless and lossy alike, and GCC 15 on AMD EPYC machines gains -3.6 % to -1.8 %; clang gains -4.6 % to -4.4 % lossy but only -2.1 % and -2.0 % lossless, short of 3 %, and clang-cl SCALAR on Windows misses -3.0 % by 0.3 and 0.7 points on the two large lossless cells. There is no whole-encode figure for E3. No GCC or clang whole-encode figure exists. With a host executor, neo-j2k C++ encodes the same 1,024² inputs in 62.43 ms lossy and
52.57 ms lossless at eight threads; that table measures C++ scaling rather than a like-for-like OpenJPEG comparison.
All encoded bytes remain identical.

### Small images, one thread (stages 2-4)

The Second Life viewer's requests, the median per file, then over each class. 1,437 configurations, load 4 %
mean. "Change" is neo-j2k's time against OpenJPEG's (minus is faster).

| Request | OpenJPEG µs | neo-j2k µs | Change |
|---|---|---|---|
| 32² whole | 34.1 | 8.1 | -76.0 % |
| 64² whole | 373 | 316 | -15.0 % |
| 128² whole | 614 | 491 | -20.0 % |
| 256² whole | 2,227 | 1,839 | -17.0 % |
| 512² whole | 11,376 | 8,966 | -21.0 % |
| 1,024² whole | 37,176 | 26,158 | -30.0 % |
| 1,024² at reduce 3 | 1,552 | 1,360 | -12.0 % |
| 128² at reduce 5 | 27.2 | 6.54 | -76.0 % |
| 1,024², first 600 bytes | 68.3 | 24.4 | -64.0 % |
| 64², first 600 bytes | 24.2 | 4.41 | -82.0 % |

Requests whose output is a few pixels (the 1x1 floor, high reduce levels, the first 600 bytes) are 66.7 % to 85.7 % faster
than OpenJPEG (3-7x). They are also 1-3 % slower than stage 1c, a fixed cost of 0.05-0.17 µs on requests of 3-25 µs.
Its cause was narrowed down but not found (`doc/bench.md`). After warm-up, no request up to 512 x 512 calls
the allocator.

### Large images, threads lent by the host (stage 5)

Time (change: minus is faster) and peak memory against OpenJPEG `opj_codec_set_threads(N)`, from memory. Three rounds (two for 16,384²),
medians and the highest peak. Load 26 % mean.

| Image | N | OpenJPEG ms / MB | neo-j2k ms / MB | Change |
|---|---|---|---|---|
| 4,096², one tile, 9/7 | 1 | 392 / 205 | 234 / 200 | -40.0 % |
| | 8 | 136 / 207 | 46 / 201 | -66.0 % |
| 4,096², 16 tiles, 5/3 | 1 | 3,203 / 248 | 2,948 / 244 | -8.0 % |
| | 8 | 488 / 249 | 418 / 245 | -14.0 % |
| 8,192², one tile, 9/7 | 1 | 1,584 / 813 | 946 / 792 | -40.0 % |
| | 8 | 567 / 817 | 238 / 793 | -58.0 % |
| 16,384², 256 tiles, 9/7 | 1 | 5,731 / 3,134 | 3,615 / 3,131 | -37.0 % |
| | 8 | 2,002 / 3,135 | 747 / 3,131 | -63.0 % |

neo-j2k is faster at every thread count and needs no more memory. One exception: the stream API's peak on
multi-tile images is higher, 238 MB against 209 at 4,096² in 16 tiles and 3,131 against 3,096 at 16,384² in
256 tiles. The j2k-tile module's low-memory stream closes that. The memory API is not affected.

### Busy or idle: the host decides (stage 5b)

The library owns no threads. The host lends an executor before a decode, and should lend it only when it has
idle cores. One decode at a time on an otherwise idle host, decodes a second (in brackets: the change in time per decode, minus is faster; then the speed-up factor):

| Image | No executor | 4 threads lent | 8 threads lent |
|---|---|---|---|
| 512² RGB | 174.2 | 254.6 (-31.5 %, 1.46x) | 278.5 (-37.5 %, 1.60x) |
| 1,024² RGB | 43.7 | 51.9 (too noisy to claim) | 74.8 (-41.5 %, 1.71x) |
| 2,048² RGB | 9.7 | 15.5 (-37.5 %, 1.60x) | 19.0 (-49.0 %, 1.96x) |

On a saturated host (8 threads, each decoding 1,024² RGB), lending the pool gains no throughput: the median is
1.02 (time -2.0 %), within noise. It costs about 3 % more CPU, so a busy host should not lend. A tile is split only when its
largest component has at least 2^18 samples (512 x 512).

### The upload encode, one thread

The Second Life viewer's upload settings, three components, byte for byte OpenJPEG's codestream. Load 28 %
mean. This table is from before the MQ coder redesign (stage 6c in `doc/bench.md`), which made the one-thread
encode about -40.0 % to -35.0 % against these times (1,024²: 183.7 ms lossy and 178.8 ms lossless, against OpenJPEG's 240.4
and 253.9 in a later run). The adaptive multi-core results follow in `doc/bench.md`.

| Size | Mode | OpenJPEG ms | neo-j2k ms | Change | Peak MB, OpenJPEG / neo-j2k |
|---|---|---|---|---|---|
| 64² | lossy | 1.2 | 1.2 | -3.0 % | 0.6 / 0.3 |
| 256² | lossy | 13.2 | 15.1 | +14.0 % | 2.7 / 2.4 |
| 1,024² | lossy | 250.0 | 284.5 | +14.0 % | 38.9 / 37.2 |
| 1,024² | lossless | 262.8 | 295.9 | +13.0 % | 38.8 / 37.5 |
| 2,048² | lossy | 1,007.1 | 1,176.3 | +17.0 % | 153.9 / 148.0 |

On one thread the encoder's time is -3.0 % to +17.0 % against OpenJPEG's, most rows +11.0 % to +17.0 %, with a peak 0.3 to 6 MB
lower. On the benchmark machine, eight threads changed the time of 256² uploads by -75.6 % to -73.0 % (3.7-4.1x faster), 512² uploads by -80.0 % to -76.7 % (4.3-5.0x faster), and 1,024² uploads by -82.1 % to -77.8 %
(4.5-5.6x faster); `doc/bench.md` records the runs and crossover.

### In the Vulkanstorm viewer

1,200 real Second Life textures from a viewer cache, decoded through the viewer's own code with its 8-worker
decode pool. RelWithDebInfo, one run each, about 15 % run-to-run noise. Mean ms per request:

| Class | OpenJPEG | neo-j2k, one thread | neo-j2k, idle pool lent |
|---|---|---|---|
| All 1,200 | 30.5 | 22.7 | 11.1 |
| 512 (228 textures) | 8.1 | 6.8 | 5.8 |
| 1,024 (803 textures) | 32.3 | 23.7 | 11.8 |
| 2,048 (59 textures) | 147.6 | 110.1 | 37.4 |

With the pool saturated (nothing lent), wall time per decode is 4.59 ms with OpenJPEG and 3.03 ms with
neo-j2k. Across 14,400 decodes of those textures, whole and cut at every level, the size, result and pixels
were identical to OpenJPEG's in every case.

## Identical output

Decodes are bit-identical to OpenJPEG 2.5.4 on conforming codestreams. The exact reference is x86-64 through
OpenJPEG's SSE 9/7 path, with floating-point contraction off. Encodes are byte for byte OpenJPEG's for the
supported settings. Where OpenJPEG departs from the standard, refusing valid input or accepting invalid input,
neo-j2k follows the standard and lists the departure with its test (`doc/departures.md`). The evidence
(`doc/testing.md`):
- the generated corpus and its reference tables at every reduce level and cut point;
- OpenJPEG's conformance and non-regression data;
- 3,172 generated upload textures plus the corpus images for the encoder;
- identity at 1, 2, 4, 8, 16 and 64 threads;
- ASan, UBSan, ThreadSanitizer and MemorySanitizer builds;
- libFuzzer targets under ClusterFuzzLite on every PR and nightly.

## Build and test

### C++

```
cmake --preset windows-msvc          # also windows-clang-cl, windows-msvc-asan, windows-clang-cl-asan,
cmake --build --preset windows-msvc  # linux-gcc, linux-gcc-scalar, linux-clang, linux-clang-asan-ubsan,
                                     # linux-clang-tsan, linux-clang-fuzz
python -I tools/fetch_testdata.py    # ISO 15444-4 class 1 data for the conformance test
ctest --preset windows-msvc
```

CMake 3.25+ (4.2+ for the Visual Studio 18 2026 generator of the Windows presets), a C++20 compiler (VS 2026,
GCC 14+, Clang), Python 3 for the checks. The library needs nothing else.

Options, given at configure time (`-D<option>=<value>`):

| Option | Default | What |
|---|---|---|
| `NJ2K_BUILD_STATIC` | `ON` | the static library |
| `NJ2K_BUILD_SHARED` | `ON` | the shared library, `neoj2k.dll` or `libneoj2k.so` (exports `nj2k_*` only) |
| `NJ2K_SIMD` | `AVX2` | `AVX2` (x86-64-v3) or `SCALAR` (x86-64). Both give identical output |
| `NJ2K_MSVC_STATIC_RUNTIME` | `OFF` | link the static C runtime (`/MT`) instead of `/MD` |
| `NJ2K_PROFILE_OPENSIM` | `ON` | the OpenSim decoder limit profile |
| `NJ2K_BUILD_TESTS`, `NJ2K_BUILD_BENCH`, `NJ2K_BUILD_TOOLS` | on as the top-level project | tests, oracle and harness; the bench; the tools |
| `NJ2K_BUILD_FUZZERS` | `OFF` | libFuzzer targets (Clang, or MSVC with `/fsanitize=fuzzer`) |
| `NJ2K_BUILD_RUST_SHIM` | `OFF` | the test-only C interface supplied by the separate Rust oracle crate; requires `NJ2K_RUST_SHIM_SOURCE` and is never installed |

The benches are in `bench/`, with their commands in `doc/bench.md`. Run compiled tests with `TEMP`/`TMP`
(Windows) or `TMPDIR` (POSIX) pointing at a folder your antivirus excludes.

### Rust

The maintained native Rust codec now lives in the separate
[neoj2k-rust repository](https://github.com/anne-skydancer/neoj2k-rust). It decodes J2K, JP2 and JPH
Part 1 codestreams and provides reversible 5/3 and irreversible 9/7 encoding. Its test-only oracle
supplies `NJ2K_RUST_SHIM_SOURCE` when it builds this C++ repository for differential tests.

## Layout

| Folder | What |
|---|---|
| `include/neoj2k/` | `neoj2k.h` (C API), `neoj2k_openjpeg.h` (OpenJPEG names), `neoj2k.hpp` (C++ wrapper), `compat/` |
| `src/` | the library |
| `modules/` | the modules the core does not depend on: `openjp2`, OpenJPEG's API as `openjp2.dll` / `libopenjp2.so.7` with neo-j2k inside (`doc/api.md`) |
| `oracle/` | OpenJPEG at the pinned fork commit (the head of the stage 1a hotfix branch, as the package `v2.5.4-cpu-avx2.5` builds it) and any patches `oracle/PIN` lists, built as the test oracle (`doc/oracle.md`) |
| `tests/` | the runner (`nj2k_diff`), conformance, unit tests, the shared harness (`tests/common`) |
| `corpus/generated/` | the generated corpus and the oracle's reference tables |
| `fuzz/` | libFuzzer targets, dictionary, regression inputs |
| `bench/` | the bench (`nj2k_bench`, `doc/bench.md`) |
| `tools/` | corpus generator, oracle sync, compatibility-header generator, export, object and header checks |
| `abi/` | the shared library's export list |
| `doc/` | API, oracle, testing, bench, departures, stages |

<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026, Anne Skydancer (neo-j2k) -->

# neo-j2k

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
departure with its test (`doc/departures.md`). A native Rust crate follows later and is tested against the C++
library bit for bit.

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
(`nj2k_decoder_set_executor`), and a tile whose largest component has 2^18 samples or more is split across it,
with the same output at any thread count. The host lends only when it has idle cores (stage 5b,
`doc/api.md`).

Stage 6a, the encoder: the settings the Second Life viewer encodes with (RLCP, 64 x 64 code-blocks, 5/3 lossless or
9/7 with rate layers, the colour transform, 8-bit components of up to 4,096 samples a side), through OpenJPEG's
encoder API, with codestreams byte for byte OpenJPEG's (`encode_unit`, the encode fuzz target); the rest of
OpenJPEG's encoder gives `NJ2K_E_UNSUPPORTED` (the j2k-extra module). One thread, correctness first: speed is
stage 6b's (`doc/api.md`, `doc/bench.md`).

Version: 0.1.0 is the first release. The Rust crate will carry the same version number for the same output.

## How fast

All numbers are measured, against OpenJPEG 2.5.4 at the oracle pin (`doc/oracle.md`), built with the same flags.
Every decode and encode in these tables gives the oracle's output. The machine is a Ryzen 9 9950X3D (8 cores, 8
threads used), Windows, MSVC 14.51 RelWithDebInfo, AVX2. It was not quiet: other jobs shared it, so the ratios
are more reliable than the milliseconds. Each section names its load. The full tables, with every run and how
to reproduce them, are in `doc/bench.md`.

### Small images, one thread (stages 2-4)

The Second Life viewer's requests, the median per file, then over each class. 1,437 configurations, load 4 %
mean. "Ratio" is neo-j2k's time over OpenJPEG's.

| Request | OpenJPEG µs | neo-j2k µs | Ratio |
|---|---|---|---|
| 32² whole | 34.1 | 8.1 | 0.24 |
| 64² whole | 373 | 316 | 0.85 |
| 128² whole | 614 | 491 | 0.80 |
| 256² whole | 2,227 | 1,839 | 0.83 |
| 512² whole | 11,376 | 8,966 | 0.79 |
| 1,024² whole | 37,176 | 26,158 | 0.70 |
| 1,024² at reduce 3 | 1,552 | 1,360 | 0.88 |
| 128² at reduce 5 | 27.2 | 6.54 | 0.24 |
| 1,024², first 600 bytes | 68.3 | 24.4 | 0.36 |
| 64², first 600 bytes | 24.2 | 4.41 | 0.18 |

Requests whose output is a few pixels (the 1x1 floor, high reduce levels, the first 600 bytes) are 3-7x faster
than OpenJPEG. They are also 1-3 % slower than stage 1c, a fixed cost of 0.05-0.17 µs on requests of 3-25 µs.
Its cause was narrowed down but not found (`doc/bench.md`). After warm-up, no request up to 512 x 512 calls
the allocator.

### Large images, threads lent by the host (stage 5)

Time and peak memory against OpenJPEG `opj_codec_set_threads(N)`, from memory. Three rounds (two for 16,384²),
medians and the highest peak. Load 26 % mean.

| Image | N | OpenJPEG ms / MB | neo-j2k ms / MB | Ratio |
|---|---|---|---|---|
| 4,096², one tile, 9/7 | 1 | 392 / 205 | 234 / 200 | 0.60 |
| | 8 | 136 / 207 | 46 / 201 | 0.34 |
| 4,096², 16 tiles, 5/3 | 1 | 3,203 / 248 | 2,948 / 244 | 0.92 |
| | 8 | 488 / 249 | 418 / 245 | 0.86 |
| 8,192², one tile, 9/7 | 1 | 1,584 / 813 | 946 / 792 | 0.60 |
| | 8 | 567 / 817 | 238 / 793 | 0.42 |
| 16,384², 256 tiles, 9/7 | 1 | 5,731 / 3,134 | 3,615 / 3,131 | 0.63 |
| | 8 | 2,002 / 3,135 | 747 / 3,131 | 0.37 |

neo-j2k is faster at every thread count and needs no more memory. One exception: the stream API's peak on
multi-tile images is higher, 238 MB against 209 at 4,096² in 16 tiles and 3,131 against 3,096 at 16,384² in
256 tiles. The j2k-tile module's low-memory stream closes that. The memory API is not affected.

### Busy or idle: the host decides (stage 5b)

The library owns no threads. The host lends an executor before a decode, and should lend it only when it has
idle cores. One decode at a time on an otherwise idle host, decodes a second:

| Image | No executor | 4 threads lent | 8 threads lent |
|---|---|---|---|
| 512² RGB | 174.2 | 254.6 (1.46x) | 278.5 (1.60x) |
| 1,024² RGB | 43.7 | 51.9 (too noisy to claim) | 74.8 (1.71x) |
| 2,048² RGB | 9.7 | 15.5 (1.60x) | 19.0 (1.96x) |

On a saturated host (8 threads, each decoding 1,024² RGB), lending the pool gains no throughput: the median is
1.02, within noise. It costs about 3 % more CPU, so a busy host should not lend. A tile is split only when its
largest component has at least 2^18 samples (512 x 512).

### The upload encode, one thread (stage 6a)

The Second Life viewer's upload settings, three components, byte for byte OpenJPEG's codestream. Load 28 %
mean. Stage 6b is the speed stage.

| Size | Mode | OpenJPEG ms | neo-j2k ms | Ratio | Peak MB, OpenJPEG / neo-j2k |
|---|---|---|---|---|---|
| 64² | lossy | 1.2 | 1.2 | 0.97 | 0.6 / 0.3 |
| 256² | lossy | 13.2 | 15.1 | 1.14 | 2.7 / 2.4 |
| 1,024² | lossy | 250.0 | 284.5 | 1.14 | 38.9 / 37.2 |
| 1,024² | lossless | 262.8 | 295.9 | 1.13 | 38.8 / 37.5 |
| 2,048² | lossy | 1,007.1 | 1,176.3 | 1.17 | 153.9 / 148.0 |

The encoder takes 0.97 to 1.17 of OpenJPEG's time, most rows 1.11 to 1.17, with a peak 0.3 to 6 MB lower.

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
| `NJ2K_BUILD_RUST_SHIM` | `OFF` | the test-only C interface the Rust twin's oracle crate links (`rust/README.md`); never installed |

The benches are in `bench/`, with their commands in `doc/bench.md`. Run compiled tests with `TEMP`/`TMP`
(Windows) or `TMPDIR` (POSIX) pointing at a folder your antivirus excludes.

### Rust

The native Rust twin is in progress (stages R0-R6): a transcription of this library, bit-identical to it.
The crate is native Rust, with no C or C++ and no FFI in it or its dependency tree; the C++ is only its test
oracle. Stage R0, the scaffolding, is in `rust/`; `rust/README.md` has every command, and these are the main
ones:

```
cd rust
rustup toolchain install                     # rust-toolchain.toml pins Rust 1.100.0
cargo test --release --workspace             # builds the C++ oracle with CMake, or links NJ2K_ORACLE_DIR's
RUSTFLAGS="-C target-cpu=x86-64-v3" CARGO_TARGET_DIR=target-v3 cargo test --release --workspace
cargo clippy --workspace --all-targets --all-features -- -D warnings
cargo fmt --all --check
python -I ../tools/check_rust_native.py      # the crate's tree holds no C or C++, its sources no FFI
```

The tests link the C++ library as the oracle through `rust/neo-j2k-oracle`, a test-only crate that is never
published, with the test-only CMake target `nj2k_rust_shim` (`-DNJ2K_BUILD_RUST_SHIM=ON`, off by default).
Miri runs on a separately pinned nightly (`rust/nightly-toolchain`); the crate itself never needs one.

## Layout

| Folder | What |
|---|---|
| `include/neoj2k/` | `neoj2k.h` (C API), `neoj2k_openjpeg.h` (OpenJPEG names), `neoj2k.hpp` (C++ wrapper), `compat/` |
| `src/` | the library |
| `oracle/` | OpenJPEG at the pinned fork commit (the head of the stage 1a hotfix branch, as the package `v2.5.4-cpu-avx2.5` builds it) and any patches `oracle/PIN` lists, built as the test oracle (`doc/oracle.md`) |
| `tests/` | the runner (`nj2k_diff`), conformance, unit tests, the shared harness (`tests/common`) |
| `rust/` | the native Rust twin: the crate, its test kit and its C++ oracle crate (`rust/README.md`) |
| `corpus/generated/` | the generated corpus and the oracle's reference tables |
| `fuzz/` | libFuzzer targets, dictionary, regression inputs |
| `bench/` | the bench (`nj2k_bench`, `doc/bench.md`) |
| `tools/` | corpus generator, oracle sync, compatibility-header generator, export, object and header checks |
| `abi/` | the shared library's export list |
| `doc/` | API, oracle, testing, bench, departures, stages |

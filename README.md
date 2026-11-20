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
Decoding by tile and decode areas come later, the encoder in 6a (`doc/stages.md`).

Stages 2-4, speed on one thread with the same output: the colour transform, DC shift and copy into the
caller's buffers fused into one pass; the inverse wavelet row-wise on large levels; the MQ decoder's
renormalisation by count-leading-zeros (`doc/bench.md`).

## Build and test

```
cmake --preset windows-msvc        # windows-clang-cl, windows-msvc-asan, linux-gcc, linux-gcc-scalar,
cmake --build --preset windows-msvc  # linux-clang, linux-clang-asan-ubsan, linux-clang-fuzz
python -I tools/fetch_testdata.py  # ISO 15444-4 class 1 data for the conformance test
ctest --preset windows-msvc
```

CMake 3.25+ (4.2+ for the Visual Studio 18 2026 generator of the Windows presets), a C++20 compiler (VS 2026,
GCC 14+, Clang), Python 3 for the checks. The library needs nothing else.

## Layout

| Folder | What |
|---|---|
| `include/neoj2k/` | `neoj2k.h` (C API), `neoj2k_openjpeg.h` (OpenJPEG names), `neoj2k.hpp` (C++ wrapper), `compat/` |
| `src/` | the library |
| `oracle/` | OpenJPEG at the pinned fork commit (the head of the stage 1a hotfix branch, as the package `v2.5.4-cpu-avx2.5` builds it) and any patches `oracle/PIN` lists, built as the test oracle (`doc/oracle.md`) |
| `tests/` | the runner (`nj2k_diff`), conformance, unit tests, the shared harness (`tests/common`) |
| `corpus/generated/` | the generated corpus and the oracle's reference tables |
| `fuzz/` | libFuzzer targets, dictionary, regression inputs |
| `bench/` | the bench (`nj2k_bench`, `doc/bench.md`) |
| `tools/` | corpus generator, oracle sync, compatibility-header generator, export, object and header checks |
| `abi/` | the shared library's export list |
| `doc/` | API, oracle, testing, bench, departures, stages |

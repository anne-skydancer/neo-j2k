<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026, Anne Skydancer (neo-j2k) -->

# neo-j2k

A JPEG 2000 Part 1 codec (decode and encode, lossless 5/3 and lossy 9/7) in C++20, built as a static library or
as its own DLL (`neoj2k.dll`, `libneoj2k.so`). Its arithmetic is OpenJPEG 2.5.4's, so whole-file decodes are
bit-identical to it; everything around the arithmetic is rebuilt for small images, flawless partial decodes and
bounded resources. A native Rust crate follows later and is tested against the C++ library bit for bit.

Licence: BSD-2-Clause (`LICENSE`). Code ported from OpenJPEG keeps its own BSD-2-Clause notices (`NOTICE.md`,
`LICENSE.OpenJPEG`).

The repository is private until the C++ library and the Rust crate are at parity and fuzzed.

## State

Stage 0, the skeleton: the build, the draft API (OpenJPEG's API under `nj2k_` names, with a compile-time `opj_*`
compatibility header), CI, the test oracle, the corpus and its reference tables, the differential, cut-point,
layer and conformance harness, fuzz targets and the bench.

Stage 1b, the header and the limits: streams, the decoder up to the decode, the main and tile-part header
model and the JP2/JPH box reader, the decoder limits (the Second Life profile built in, an OpenSim module),
statuses (`NJ2K_E_NEED_DATA`, `NJ2K_TRUNCATED` for 1c's partial decodes), the allocator hook, the reduce
policy and the decoded dimensions (`doc/api.md`). Decoding comes in stage 1c, the encoder in 6a
(`doc/stages.md`).

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
| `oracle/` | OpenJPEG at the pinned fork commit (the stage 1a hotfix) and any patches `oracle/PIN` lists, built as the test oracle (`doc/oracle.md`) |
| `tests/` | the runner (`nj2k_diff`), conformance, unit tests, the shared harness (`tests/common`) |
| `corpus/generated/` | the generated corpus and the oracle's reference tables |
| `fuzz/` | libFuzzer targets, dictionary, regression inputs |
| `bench/` | the bench (`nj2k_bench`, `doc/bench.md`) |
| `tools/` | corpus generator, oracle sync, compatibility-header generator, export, object and header checks |
| `abi/` | the shared library's export list |
| `doc/` | API, oracle, testing, bench, departures, stages |

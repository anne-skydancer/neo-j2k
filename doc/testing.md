<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026, Anne Skydancer (neo-j2k) -->

# Testing

## Running the tests

```
cmake --preset windows-msvc            # or linux-gcc, linux-clang, ...; CMakePresets.json
cmake --build --preset windows-msvc
python -I tools/fetch_testdata.py      # ISO 15444-4 class 1 files, fetched, never committed
ctest --preset windows-msvc            # NJ2K_TESTDATA (environment or CMake) points at .testdata/openjpeg-data
```

Compiled tests run with `TEMP`/`TMP`/`TMPDIR` in `NJ2K_TEST_TMP` when it is set (the owner's machine keeps
a Defender-excluded `.fixture-tmp` for this).

| Test | What it proves |
|---|---|
| `api_unit` | neo-j2k's structures have OpenJPEG's layout; its parameter defaults and image functions behave as OpenJPEG's (compared with the oracle); the version, engine string (at most 31 characters) and CPU check keep their contracts; the test helpers read real codestreams |
| `oracle_whole` | the oracle reproduces `corpus/generated/oracle-reference.tsv` for every generated file at reduce 0-5, in plain and in viewer mode: the same output on every compiler and platform in CI |
| `oracle_cuts.<file>` | the same for every cut point of every generated file up to 4 KB (every 13th one in sanitizer builds) |
| `oracle_layers` | a cut where a layer starts decodes exactly as the whole file limited to the layers before it (`cp_layer`): an oracle for cut semantics that does not depend on the truncation code (plan audit B5) |
| `conformance_class1` | ISO/IEC 15444-4 class 1, profiles 0 and 1, with the T.803 (2024) peak and MSE limits; skipped without the data |
| `corpus_regenerates` | the oracle's encoder writes the committed corpus byte for byte on this compiler and platform: the encoder oracle that stage 6a's byte identity relies on |
| `fuzz_replay.*` | the fuzz targets run over the corpus and `fuzz/regressions` on every platform |
| `exports` | `neoj2k.dll` / `libneoj2k.so` export exactly the functions `neoj2k.h` declares, all `nj2k_*`, as `abi/exports.txt` lists |
| `global_state` | the static library's objects hold no writable globals, no thread-local storage and no static initialisers; `nj2k_cpu_supported()`'s object needs nothing beyond baseline x86-64 |
| header checks (build) | `neoj2k.h` and the compatibility header compile as strict C99 (MSVC: C11) and C++20 with warnings as errors |

Nightly (`NJ2K_TEST_NIGHTLY=ON`): `oracle_cuts_nightly`, every cut point of the generated files from 4 to
16 KB, in plain and in viewer mode.

## The runner: `nj2k_diff`

Backends: `oracle` (linked), `neoj2k` (through the compatibility header; refuses until stage 1c), and
`lib:<path>[=<name>]`, any library with OpenJPEG's ABI loaded at run time. Modes: `plain`, `strict`,
`viewer` (the viewer's two-pass request, its reduce clamp, size guard, geometry checks and 8-bit copy).

```
# the stage-0 gate: the oracle against the shipped DLL and the fix build on the bench corpus (local, private)
nj2k_diff compare --a oracle --b "lib:C:/Program Files/Vulkanstorm-Release/openjp2.dll=shipped" \
    --files <bench list> --mode viewer --kinds full,budget,first --reduce-policy available
# every cut point of a file, two decoders
nj2k_diff compare --a oracle --b neoj2k --files corpus/generated/sl_32_rgb_97.j2k --kinds cuts
# one request, described
nj2k_diff decode --a oracle --files corpus/generated/packed_ppm_sop_eph_97.j2k --bytes 484 --reduce 0
```

Kinds: `full`, `budget` (the viewer's byte budget at the reduce level), `first` (600 bytes at reduce 5),
`cuts` (every prefix of files up to `--cut-max`).

## The corpora

- **Generated** (`corpus/generated`, committed, 59 files, 226 KB): written by `nj2k_corpus_gen` with the
  oracle's encoder; `MANIFEST.tsv` lists each file's features. It covers 1x1 to 4096 px, 1 to 5
  components, 8, 12, 16-bit and signed samples, subsampling, 5/3 and 9/7, the five progressions, 1 to 8
  layers, 1 to 6 resolutions, image and tile origins, tiles, precincts, code-block sizes, SOP, EPH, every
  mode switch, PLT, TLM, POC, and PPM and PPT (rewritten from SOP+EPH streams, then checked to decode as
  their sources at every reduce level). `nj2k_corpus_gen --verify corpus/generated` encodes again and
  compares.
- **ISO 15444-4 and OpenJPEG's data**: fetched at test time (`tools/fetch_testdata.py`, pinned commit).
- **Second Life textures**: the owner's private corpus stays on the owner's PC; lists of local paths go to
  `--files`.

## Re-recording the reference tables

Only when the oracle's output changes on purpose (a new pin):

```
nj2k_diff record --a oracle --files corpus/generated --root corpus/generated \
    --out corpus/generated/oracle-reference.tsv --modes plain,viewer --reduce 0-5 --cut-max 4096 --cut-steps 1,13
nj2k_diff record --a oracle --files corpus/generated --root corpus/generated \
    --out corpus/generated/oracle-reference-nightly.tsv --modes plain,viewer --reduce 0-5 \
    --cut-min 4096 --cut-max 16384 --cut-steps 1 --no-whole
```

The tables were written on Windows with MSVC; CI checks every other compiler against them.

## Fuzzing

Targets (`fuzz/`): `header`, `decode` (options in four leading bytes: reduce level, plain, strict or
viewer mode, layers, cut length; a stream starting with SOC is taken with the defaults, so corpus files
are seeds as they are) and `roundtrip` (lossless encode and decode must give the samples back). Until
neo-j2k decodes (stage 1c) they drive the oracle, behind resource limits that keep OpenJPEG's known bombs
(fork audit S2, S3) out of the way. Pull requests: ClusterFuzzLite, five minutes with ASan and five with
UBSan. Nightly: ten minutes a target. Local: the `linux-clang-fuzz` preset, or MSVC with
`-DNJ2K_BUILD_FUZZERS=ON -DNJ2K_SANITIZE=address`.

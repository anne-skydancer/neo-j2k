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
a Defender-excluded `.fixture-tmp` for this). Every test program, tool and fuzz driver reports its
failures on the console, never in a dialog (`console_reports()`, `tests/common/common.h`: on Windows no
error-reporting box, and `abort()`, `assert()` and the debug C runtime write to stderr), and the replay
driver turns a fuzz target's abort into exit code 134 after the target's report.

| Test | What it proves |
|---|---|
| `api_unit` | neo-j2k's structures have OpenJPEG's layout; its parameter defaults and image functions behave as OpenJPEG's (compared with the oracle); the version, engine string (at most 31 characters) and CPU check keep their contracts; the test helpers read real codestreams |
| `oracle_whole` | the oracle reproduces `corpus/generated/oracle-reference.tsv` for every generated file at reduce 0-5, in plain and in viewer mode: the same output on every compiler and platform in CI |
| `oracle_cuts.<file>` | the same for every cut point of every generated file up to 4 KB (every 13th one in sanitizer builds) |
| `oracle_layers` | a cut where a layer starts decodes exactly as the whole file limited to the layers before it (`cp_layer`): an oracle for cut semantics that does not depend on the truncation code (plan audit B5); the oracle's known cases are in `tests/oracle-known-defects.txt` |
| `oracle_known_resources` | each input of `tests/known-defects` still makes the oracle ask the heap for what `tests/oracle-known-resource-defects.txt` says (OD-3, OD-5), through the counting oracle module (builds without sanitizers) |
| `oracle_known_defect.od4` | with the round-trip target's guard off, its OD-4 input still makes the oracle's MQ encoder write past its buffer (AddressSanitizer builds) |
| `conformance_class1` | ISO/IEC 15444-4 class 1, profiles 0 and 1, with the T.803 (2024) peak and MSE limits; skipped without the data |
| `corpus_regenerates` | the oracle's encoder writes the committed corpus byte for byte on this compiler and platform: the encoder oracle that stage 6a's byte identity relies on |
| `fuzz_replay.*` | the fuzz targets run over their seeds (the corpus; the decode seeds with option bytes, made by `fuzz_seeds.decode`; `fuzz/seeds/roundtrip`) and `fuzz/regressions` on every platform; a replay fails when no input gets past the target's filters and guards |
| `fuzz_filters` | the fuzz filters keep every bomb of `fuzz/bombs` from the oracle and let the Second Life sized corpus files through |
| `exports` | `neoj2k.dll` / `libneoj2k.so` export exactly the functions `neoj2k.h` declares, all `nj2k_*`, as `abi/exports.txt` lists |
| `global_state` | the static library's objects hold no writable globals, no thread-local storage, no static initialisers and no LTO objects (MSVC `/GL`, LLVM bitcode, GCC `.gnu.lto_*`); `nj2k_cpu_supported()`'s object needs nothing beyond baseline x86-64, and in the scalar build (`NJ2K_SIMD=SCALAR`) no object of the library does |
| header checks (build) | `neoj2k.h` and the compatibility header compile as strict C99 (MSVC: C11) and C++20 with warnings as errors |

The reference tables are committed: configuring without one, or with an `NJ2K_TEST_CUT_STEP` it does not
hold, is an error, and `nj2k_diff verify` fails when no line of the table matches its options, so no
reference test can pass having checked nothing. The tests, tools, fuzz targets and the bench clear
`OPJ_NUM_THREADS` before any OpenJPEG codec exists, so the oracle never starts its thread pool.

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

Kinds: `full`, `budget` (the viewer's byte budget at the reduce level, from the size the viewer stores:
the tile grid's, tiles x tile size), `first` (600 bytes at reduce 5), `cuts` (every prefix of files up to
`--cut-max`). `nj2k_diff resources` checks the known resource defects through a counting backend.

## The corpora

- **Generated** (`corpus/generated`, committed, 63 files, 236 KB): written by `nj2k_corpus_gen` with the
  oracle's encoder; `MANIFEST.tsv` lists each file's features. It covers 1x1 to 4096 px, 1 to 5
  components, 8, 12, 16-bit and signed samples, subsampling, 5/3 and 9/7, the five progressions, 1 to 8
  layers, 1 to 6 resolutions, image and tile origins, tiles, precincts, code-block sizes, SOP, EPH, every
  mode switch, PLT, TLM, POC, and what OpenJPEG's encoder cannot write, rewritten from its output: PPM
  and PPT (from SOP+EPH streams), and a last tile-part of Psot = 0, one tile and four, with EOC and
  without (`psot0_*`). Each rewrite is checked to decode as its source at every reduce level (the
  Psot = 0 ones without EOC only since the 1a pin: OpenJPEG 2.5.4 drops their last two data bytes, plan
  W5).
  `nj2k_corpus_gen --verify corpus/generated` encodes again and compares.
- **ISO 15444-4 and OpenJPEG's data**: fetched at test time (`tools/fetch_testdata.py`, pinned commit).
- **Second Life textures**: the owner's private corpus stays on the owner's PC; lists of local paths go to
  `--files`.

## Re-recording the reference tables

Only when the oracle's output changes on purpose (a new pin), and only after the suite has run against the
tables as they are, so that every changed line is one the new pin explains (`doc/oracle.md`):

```
nj2k_diff record --a oracle --files corpus/generated --root corpus/generated \
    --out corpus/generated/oracle-reference.tsv --modes plain,viewer --reduce 0-5 --cut-max 4096 --cut-steps 1,13
nj2k_diff record --a oracle --files corpus/generated --root corpus/generated \
    --out corpus/generated/oracle-reference-nightly.tsv --modes plain,viewer --reduce 0-5 \
    --cut-min 4096 --cut-max 16384 --cut-steps 1 --no-whole
```

The tables were written on Windows with MSVC; CI checks every other compiler against them.

## Fuzzing

Targets (`fuzz/`):

- `header`: the test scanner, and the oracle's header pass (`opj_read_header`, `opj_get_cstr_info`).
- `decode`: options in four leading bytes (reduce level; plain, strict or viewer mode; layers; cut
  length). A stream starting with SOC takes the defaults, so corpus files are seeds as they are, and
  `fuzz/make_inputs.py --decode-seeds` puts option bytes in front of each (the viewer's request at reduce
  5 on its first 600 bytes and at reduce 2 on half the file, plain decodes at reduce 1-5, strict mode with
  one layer, a plain cut).
- `roundtrip`: a small image and its coding from the input, encoded losslessly and decoded: every sample
  must come back. Seeds: `fuzz/seeds/roundtrip`.

Until neo-j2k decodes (stage 1c) they drive the oracle, behind filters (`fuzz/fuzz_common.h`) that keep
OpenJPEG's known bombs out of the way (fork audit S2, S3; OD-3, OD-5). The header filter bounds the tiles
and components of SIZ, which must follow SOC. The decode filter fails closed: it refuses a stream whose
main header it cannot parse, or that holds a marker OpenJPEG skips by scanning (its reading would then
differ from OpenJPEG's), and bounds the worst of every COD and COC, of the main header and of every
tile-part header: layers, packets, and the code-blocks of every resolution at full size (OpenJPEG builds
them whatever the reduce factor), plus the samples at the decoded level and each tile-part's declared
length. `fuzz_filters` checks the filters against `fuzz/bombs`.

Pull requests: ClusterFuzzLite, five minutes with ASan and five with UBSan. Nightly: ten minutes a target
with ASan and UBSan, on the corpus the earlier nights grew (kept in the Actions cache). Local: the
`linux-clang-fuzz` preset, or MSVC with `-DNJ2K_BUILD_FUZZERS=ON -DNJ2K_SANITIZE=address`.

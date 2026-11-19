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

Windows AddressSanitizer builds (`windows-msvc-asan`, `windows-clang-cl-asan`, or `NJ2K_SANITIZE=address`)
need the compiler's runtime, `clang_rt.asan_dynamic-x86_64.dll`, which is not on `PATH`. The configure
finds it where the compiler keeps it, or stops, and the build copies it beside every program (the test
programs, the fuzz drivers and libFuzzer targets, the tools) before any of them links, on every build:
no program of such a build starts without it, and nothing needs adding to `PATH`. With clang-cl the
configure also checks the runtime libraries the link names (clang-cl 20 and later), or stops. CI builds
and tests both presets.

| Test | What it proves |
|---|---|
| `api_unit` | neo-j2k's structures have OpenJPEG's layout (the decoder limits too); its parameter defaults and image functions behave as OpenJPEG's (compared with the oracle); the version, engine string (at most 31 characters) and CPU check keep their contracts; the test helpers read real codestreams; neo-j2k's header read gives the oracle's facts through the compatibility header |
| `header_unit` | stage 1b: the limit profiles (the Second Life one equal to the fork's, field for field); `NJ2K_E_NEED_DATA` at every cut inside every generated file's main header, and through a one-byte stream buffer; JP2 reading and its cuts, and against the oracle: a JP2 file from a stream that does not give its length (no box is read, as in OpenJPEG: 28 bytes that declare a 4 GiB box make the codec take under 16 KB), the codestream box whatever its length says, an empty bits-per-component box before the image header, image boxes before the JP2 header box (DP-11); component depths (OpenJPEG's JP2 warning for a file whose image header gives one depth, none for a codestream); the fork audit's S2 and S3 probe rows and the plan audit's A1 rows (header state flat at 2,312 bytes whatever the header declares, and each refused by the Second Life profile before what it bounds is allocated); the fuzz bombs and the oracle's known resource defects; marker precedence (W7), quantization counts (W8), unknown and Part 2 markers and invalid values (DP-5 to DP-8, the oracle reading each), and PPM markers whose packet headers hold no bytes (refused by both); every tile-part header of every generated file against the test scanner (W5); tile-part markers in any order at the same cost (65,535 tiles in descending order, 16,384 COC markers in one tile), each tile resolved whole as each component alone; the decoded dimensions against the oracle's decodes at every reduce level, the factor given before and after the header; the reduce policy (W4) and the setter (S1); the allocator hook, with every allocation of a header read failed in turn; the header model owning its bytes (the input overwritten and freed after the read: the codestream information, the dimensions, the merged PPM and the PPT data; plan audit D5); the component selection (repeated calls take no more memory, a refused one keeps the earlier one, as OpenJPEG's); 128,000 header reads of mutated headers, every one balanced in the allocator |
| `header_facts` | neo-j2k's header read gives the oracle's image and codestream information for every generated file at reduce 0-5, the factor in the parameters and set before the header, and every prefix through the main header refuses where the oracle refuses |
| `header_facts_conformance` | the same over OpenJPEG's conformance data (ISO class 1, the other codestreams, and the JP2 files), prefixes up to 1 KB; present when the data was fetched before configuring |
| `oracle_whole` | the oracle reproduces `corpus/generated/oracle-reference.tsv` for every generated file at reduce 0-5, in plain and in viewer mode: the same output on every compiler and platform in CI |
| `oracle_cuts.<file>` | the same for every cut point of every generated file up to 4 KB (every 13th one in sanitizer builds) |
| `oracle_layers` | a cut where a layer starts decodes exactly as the whole file limited to the layers before it (`cp_layer`): an oracle for cut semantics that does not depend on the truncation code (plan audit B5); the oracle's known cases are in `tests/oracle-known-defects.txt` |
| `oracle_known_resources` | each input of `tests/known-defects` still makes the oracle ask the heap for what `tests/oracle-known-resource-defects.txt` says (OD-3, OD-5), through the counting oracle module (builds without sanitizers) |
| `oracle_known_defect.od4` | with the round-trip target's guard off, its OD-4 input still makes the oracle's MQ encoder write past its buffer (AddressSanitizer builds) |
| `oracle_known_defect.od6` | with the round-trip target's guard off, its OD-6 input (`fuzz/regressions`) still comes back with other samples: the oracle's encoder overflows its two guard bits (every compiler and platform) |
| `conformance_class1` | ISO/IEC 15444-4 class 1, profiles 0 and 1, with the T.803 (2024) peak and MSE limits; skipped without the data |
| `corpus_regenerates` | the oracle's encoder writes the committed corpus byte for byte on this compiler and platform: the encoder oracle that stage 6a's byte identity relies on |
| `fuzz_replay.*` | the fuzz targets run over their seeds (the corpus; the JP2 header seeds, made by `fuzz_seeds.header`; the decode seeds with option bytes, made by `fuzz_seeds.decode`; `fuzz/seeds/roundtrip`) and `fuzz/regressions` on every platform; a replay fails when no input gets past the target's filters and guards, and the header target's when a header read differs from the oracle's without a departure to explain it |
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

Backends: `oracle` (linked), `neoj2k` (through the compatibility header; reads headers since stage 1b,
decodes from stage 1c), and `lib:<path>[=<name>]`, any library with OpenJPEG's ABI loaded at run time.
Modes: `plain`, `strict`, `viewer` (the viewer's two-pass request, its reduce clamp, size guard, geometry
checks and 8-bit copy).

```
# the stage-0 gate: the oracle against the shipped DLL and the fix build on the bench corpus (local, private)
nj2k_diff compare --a oracle --b "lib:C:/Program Files/Vulkanstorm-Release/openjp2.dll=shipped" \
    --files <bench list> --mode viewer --kinds full,budget,first --reduce-policy available
# every cut point of a file, two decoders
nj2k_diff compare --a oracle --b neoj2k --files corpus/generated/sl_32_rgb_97.j2k --kinds cuts
# one request, described
nj2k_diff decode --a oracle --files corpus/generated/packed_ppm_sop_eph_97.j2k --bytes 484 --reduce 0
# header facts, two decoders: every file (JP2 and JPH too), reduce level and mode, and the prefixes
nj2k_diff header --a oracle --b neoj2k --files <list or folder> --modes plain,viewer --reduce 0-5 --bytes 2048
```

Kinds: `full`, `budget` (the viewer's byte budget at the reduce level, from the size the viewer stores:
the tile grid's, tiles x tile size), `first` (600 bytes at reduce 5), `cuts` (every prefix of files up to
`--cut-max`). `nj2k_diff resources` checks the known resource defects through a counting backend.
`nj2k_diff header` prints each file whose reads differ, with the count and the first difference.

## Stage 1b's local gates (private data, on the owner's machine)

```
# header facts on the private bench corpus (212 files)
nj2k_diff header --a oracle --b neoj2k --files <bench list> --modes plain,viewer --reduce 0-5 --bytes 2048
# every texture of the viewer's cache: header facts against the oracle, and the Second Life profile at
# reduce 0-5 through the API as the viewer will ask (the header read with the factor set before it and
# NJ2K_REDUCE_CLAMP, then nj2k_get_decoded_dimensions); --opensim for the OpenSim profile, exit status
# 2 when the build lacks it (prints aggregates only)
nj2k_cache_check "%LOCALAPPDATA%\Vulkanstorm_x64\texturecache"
```

Results on 2026-11-19 (Windows, MSVC 14.51), again after the review round: the bench corpus, 69,478 reads,
0 different; the cache, 60,045 textures, every header read as the oracle reads it, 360,270 profile checks
at reduce 0-5 with none refused, under the Second Life profile and under the OpenSim one (the largest: 5
components, 8 layers, 1 tile, 2,048 px a side, 81.3 MiB of the 256 MiB working memory). OpenJPEG's
non-regression data (`tools/fetch_testdata.py --all`, 149 codestreams and JP2 files, 374,674 reads with
prefixes): 4 files differ, every one refused by neo-j2k where OpenJPEG reads it, for DP-6 (3) and DP-7
(1); none reads in neo-j2k that OpenJPEG refuses.

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
- **ISO 15444-4 and OpenJPEG's data**: fetched at test time (`tools/fetch_testdata.py`, pinned commit):
  the class 1 streams with their references, and the other codestreams and JP2 files of
  `input/conformance` for the header tests; `--all` adds the non-regression data.
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

- `header`: neo-j2k's header read through the API (J2K, or JP2 when the input starts with the JP2
  signature), with no limits and with the Second Life profile, then the decoded dimensions, the
  codestream information and a component selection; the input's last byte picks the reduce factor and
  whether the stream gives its length. The library's tile-part header reader over every tile-part, each
  tile's parameters resolved whole and component by component, which must agree; the test scanner. Then
  the oracle's header read (`opj_read_header`, `opj_get_cstr_info`) against neo-j2k's, through the same
  request code: the outcome, the image and the codestream information must agree at the input's reduce
  factor, unless the main header or a JP2 file's boxes show one of the departures DP-5 to DP-8 and DP-11
  (`doc/departures.md`), so the target holds the header gate and not only memory safety. neo-j2k gets no filter: its allocations go
  through a counting hook, and the target aborts when the header state outgrows the input (64 bytes for
  every input byte, plus 256 KiB) or when the codec does not give back everything it took. Seeds: the
  corpus, and the corpus in JP2 files (`fuzz/make_inputs.py --header-seeds`); the dictionary has the JP2
  boxes too.
- `decode`: options in four leading bytes (reduce level; plain, strict or viewer mode; layers; cut
  length). A stream starting with SOC takes the defaults, so corpus files are seeds as they are, and
  `fuzz/make_inputs.py --decode-seeds` puts option bytes in front of each (the viewer's request at reduce
  5 on its first 600 bytes and at reduce 2 on half the file, plain decodes at reduce 1-5, strict mode with
  one layer, a plain cut).
- `roundtrip`: a small image and its coding from the input, encoded losslessly and decoded: every sample
  must come back, unless the transform outgrows the two guard bits the oracle's encoder gives each
  band (OD-6): such an image is encoded and decoded without the comparison. Seeds: `fuzz/seeds/roundtrip`.

Until neo-j2k decodes (stage 1c) the decode and round-trip targets drive the oracle, and the header target
reads every header with it too, behind filters (`fuzz/fuzz_common.h`; for a JP2 file, applied to the
codestream OpenJPEG reaches through its boxes) that keep OpenJPEG's known bombs out of the way (fork
audit S2, S3; OD-3, OD-5). The header filter bounds the tiles and components of SIZ, which must follow
SOC. The decode filter fails closed: it refuses a stream whose main header it cannot parse, or that
holds a marker OpenJPEG skips by scanning (its reading would then differ from OpenJPEG's), and bounds
the worst of every COD and COC, of the main header and of every tile-part header: layers, packets, and
the code-blocks of every resolution at full size (OpenJPEG builds them whatever the reduce factor), plus
the samples at the decoded level and each tile-part's declared length. `fuzz_filters` checks the filters
against `fuzz/bombs`.

Pull requests: ClusterFuzzLite, five minutes with ASan and five with UBSan. Nightly: ten minutes a target
with ASan and UBSan, on the corpus the earlier nights grew (kept in the Actions cache). Local: the
`linux-clang-fuzz` preset, or MSVC with `-DNJ2K_BUILD_FUZZERS=ON -DNJ2K_SANITIZE=address`.

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
| `decode_unit` | stage 1c: DP-3 (the empty tiles of `edge_ppt_empty_tiles_97.j2k` flat at the mid-value, and every reference request on the whole file and every prefix equal to the oracle's on the file with the empty tile-parts padded; a tile-part with neither data bytes nor packed headers refused by both); `BUG_WEIRD_TWO_INVK` (the 9/7 kernels take OpenJPEG's 1.625732422f, not 2/K, and 9/7 files decode to the oracle's samples); DP-2 (the DC shift's edges, scalar, AVX2 and per tile-component); DP-12 (a main header POC before the COD: neo-j2k decodes the file's image, the oracle a flat one; after the COD both the file's); DP-13 (a broken box after a JP2 codestream); DP-15 (a box after a JP2 file's codestream box: `Psot = 0` decodes whole, a codestream without EOC as the bare codestream); the scalar kernels against their AVX2 versions (RCT, ICT and the 9/7 DC shift over 2^20 + 5 samples each in several ranges, the 5/3 rows and columns, groups of 16 included, and the 9/7 lifting over 2 to 4 million samples each, int32 wrap-around included); the memory decode into I32, U8 (bottom-up, interleaved) and U16 (padded rows) planes against the oracle's image for every corpus file at reduce 0-5, and every 7th prefix of the files up to 4 KB, each `NJ2K_TRUNCATED` when it decodes and `NJ2K_E_NEED_DATA` when it does not (a multi-tile file cut before its first tile's data too); the memory API's states and its component selection (held for the decodes of the header it was made for, dropped by the next header read); the working memory a decode takes against its estimate, with the record per reduce level of the file that takes most, and a tile whose own COD needs more refused before it is allocated; the viewer's path (one codec for every corpus file at reduce 0-5, the Second Life limits, `NJ2K_REDUCE_CLAMP`, 8-bit planes with guard bytes sized once `nj2k_get_decoded_dimensions` accepts them, each decode equal to the oracle's at the factor in effect); a tile COD with fewer resolutions than the factor, refused under both reduce policies with the planes' guard bytes untouched; markers in and between tile-part headers (DP-7); the zero-heap gate (no allocator call and no `operator new` once warmed up, for every corpus file up to 256 x 256, repeated and in turn, every decode succeeding under `NJ2K_REDUCE_CLAMP`); stage 2: the fused output pass against the unfused steps (the transform over the whole buffers, the DC shift, the copy) over every plane layout (the viewer's interleaved bottom-up image of 1 to 4 components, planar, interleaved with a gap; 8, 16 and 32 bits; guard bytes around the planes), in both builds, and its row kernels scalar against AVX2; stage 3: whole tile-components of random geometry (1 to 7 resolutions, odd origins and sizes, up to 300 samples a side, special and wrapping samples) through the inverse wavelet against stage 1c's transform (OpenJPEG's structure, kept as the reference), in both builds: as decodes take it, with its scalar kernels, and with the row-wise path forced on every level, rows padded beyond the level and fewer levels decoded than the buffer holds; stages 2-4's review round: single-tile images from the oracle's encoder whose levels reach the row-wise wavelet (5/3 at 1024 x 1024, 2048 x 600 and 600 x 2048, 9/7 at 2048 x 600) in every layout and through the compatibility API at reduce 0-2, and cut into the viewer's 8-bit image and 16-bit planes; the colour transform over three regions of one size and different shapes, which OpenJPEG pairs as flat arrays (one tile and a grid, 5/3 and 9/7: the output pass's in-place path); a single tile whose last resolutions are never decoded (a POC after the COD: a decoded region smaller than its buffer); every prefix decode also into the viewer's 8-bit image; the owner's items before the merge (2026-11-20): the zero-heap gate per request (the viewer's request through the bench's backend, one codec for every request, every file up to 512 x 512 at reduce 0-5, the DP-3 file included, whole, cut to the bytes the viewer asks for and to the first 600 bytes, each taken once and then again: the repeat makes no allocator call and no `operator new`, and every file's whole request decodes at every level but `sub_420.j2k`'s, which the viewer refuses for its component sizes; `--heap-files <list>` runs it alone on other files, the private bench corpus locally), and the fused output pass's 8-bit set (unsigned components of 1 to 8 bits, every one 5/3, 9/7 or mixed, the mixed ones without the ICT (`doc/departures.md`), widths around 16, regions of up to four pixels and small contiguous ones, padded rows: the register pass, the pixel path and the run against the unfused steps, in both builds) |
| `decode_threads` | stage 5, threads in one image: one thread against 2, 4, 8, 16 and 64 parts (64: the most a stage is cut into, and 128 Tier-1 runs) of a shared pool of 15 workers (`tests/common/thread_pool.h`), the samples, the status and every message in order the same: every corpus file at reduce 0-5, whole and cut (the files up to 64 KB at 600 bytes, a seventh, a third, two thirds and all but a byte), into the int32 planes with guard bytes and the viewer's 8-bit image, every stage forced through the executor however small (each codec decoding twice: the second decode reuses what the first kept); five large single-tile images from the oracle's encoder, each with a component of more than 2^22 samples (large images: 5/3 at 2051 x 2051 RGB, 2049 x 2050 and 2100 x 2000 grey; 9/7 at 2051 x 2052 RGB and 2053 x 2049 grey), and a 1024 x 1024 RGB image (above the 2^18 tile floor, so threaded too), at reduce 0-2, whole and cut; a 2112 x 2112 grey image in four tiles of 1056 (the image above 2^22 samples, each tile above the 2^18 floor: threaded tile by tile); multi-tile grey images of small tiles (1024 x 1024 in 64 x 64 tiles at the library's thresholds, whose 4 MiB plane clear goes through the executor; 512 x 512 in 128 x 128 tiles forced); the stream API (`nj2k_decode` into the image's planes) for the corpus files up to 64 KB forced at reduce 0-1, three large images at the library's thresholds, whole and two thirds; under capped allocators (eight corpus files, single- and multi-tile, and the 512 x 512 in 16 tiles): one thread's outcome through the executor at every cap from below the least one thread decodes with to above what the threads need; one large image decoded 100 times at 8 threads; four decoders sharing the pool at once, three rounds of the five; no call to the executor for any image whose tiles are below the tile floor and whose planes are below the clear threshold (the corpus into both layouts, 512 x 512 in 4 and in 16 tiles, a single 511 x 511 tile), and calls for the 1024 x 1024 image in 64 x 64 tiles (the clear), a single 512 x 512 tile (the floor), the 1024 x 1024 and the 2051 x 2051 RGB images and the 2112 x 2112 image in four tiles; the floor applies to the decoded size (1024 x 1024 at reduce 1 is threaded, at reduce 2 not); no call at all once the executor is taken back with a null `nj2k_decoder_set_executor`, at any size; caller's planes that share bytes (every component into one plane's bytes, rows that overlap) decoded on the calling thread with one thread's bytes; the API's checks. `nj2k_decode_unit <corpus> --threads --files <list>` runs it on other files too (the private bench corpus and the large images, locally). The ThreadSanitizer job runs it |
| `encode_unit` | stage 6a: the viewer's upload encode (`JPEG2KEncode::encode`: RLCP, 64 x 64 code-blocks, one tile, 5/3 lossless or 9/7 with rate layers by surface, the colour transform from three components, fewer resolutions under 128 px, the comment) replayed against neo-j2k through the compatibility header and against the oracle (`oracle_upload`, `neoj2k_upload`), every codestream the oracle's byte for byte, or both refusing: 3,172 generated textures (4 to 257 px sides with 1, 3, 4 and 5 components, both wavelets, eight kinds of content from flat to noise, and the sizes of uploads and bakes up to 2,048 x 2,048), the 50 images the corpus decodes to (100 codestreams), and 156 variations of the settings the viewer varies (rate sets with equal, rising, tiny and huge rates, one to eight resolutions, the colour transform on and off, comments of none, 0, 1, 17 and 300 bytes); neo-j2k's streams decoded by both decoders alike and the lossless ones to the samples; what the core refuses (`NJ2K_E_UNSUPPORTED` for each setting of OpenJPEG's encoder outside the viewer's, `NJ2K_E_LIMIT` and `NJ2K_E_ARGUMENT` cases), calls out of order, retries after a refusal, the limits' edges, output streams of 1, 16 and 4,096 bytes with a host that takes 7 or 1 bytes a call, a failing output stream (every refusal of the settings writes no byte, seen through a one-byte buffer), hostile rates, comments at the edge of Lcom, a size cap compared with the oracle, and DP-16 to DP-19 |
| `enc_t1_unit` (`enc_t1_exhaustive`, nightly) | the encoder's Tier-1 against a frozen copy of the earlier operations (`tests/unit/enc_t1_frozen.cpp`, `enc_mq_frozen.h`): random MQ decision streams with pass ends at full and short capacities, single decisions (a pseudo-random combination in ctest, all 4.4 billion nightly), `enc_t1_encode` on 6,288 generated code-blocks (passes, distortion bit for bit, bytes), and E1's `enc_preprocess` (`src/enc_pre.h`) against OpenJPEG's per-sample branch: a trimmed pass in ctest (the neighbourhoods of 0, INT32_MIN, INT32_MAX and every power of two, a stride over the whole range, random blocks), all 2^32 inputs nightly, and `enc_t1_encode` on 640 blocks of every bit-plane count from 0 to 25 with INT32_MIN in them |
| `neoj2k_whole`, `neoj2k_cuts.<file>`, `neoj2k_layers`, `neoj2k_cuts_nightly` | neo-j2k against the oracle's reference tables and the layer-boundary oracle, as the `oracle_*` tests below (stage 1c's bit-identity gate); the files of `tests/neoj2k-departures.txt` (DP-3) are left out and tested in `decode_unit` |
| `conformance_class1_neoj2k` | ISO class 1 conformance with neo-j2k |
| `neoj2k_conformance_differential` | every conformance codestream and JP2 file, in plain, strict and viewer mode at reduce 0-5, decoded by neo-j2k as by the oracle; present when the data was fetched |
| `neoj2k_nonregression_differential` | the same over OpenJPEG's non-regression data (`fetch_testdata.py --all`), plain and viewer mode, the files of `tests/neoj2k-departures-nonregression.txt` (DP-6) left out |
| `header_facts` | neo-j2k's header read gives the oracle's image and codestream information for every generated file at reduce 0-5, the factor in the parameters and set before the header, and every prefix through the main header refuses where the oracle refuses |
| `header_facts_conformance` | the same over OpenJPEG's conformance data (ISO class 1, the other codestreams, and the JP2 files), prefixes up to 1 KB; present when the data was fetched before configuring |
| `oracle_whole` | the oracle reproduces `corpus/generated/oracle-reference.tsv` for every generated file at reduce 0-5, in plain and in viewer mode: the same output on every compiler and platform in CI |
| `oracle_cuts.<file>` | the same for every cut point of every generated file up to 4 KB (every 13th one in sanitizer builds) |
| `oracle_layers` | a cut where a layer starts decodes exactly as the whole file limited to the layers before it (`cp_layer`): an oracle for cut semantics that does not depend on the truncation code (plan audit B5); the oracle's known cases are in `tests/oracle-known-defects.txt` |
| `oracle_known_resources` | each input of `tests/known-defects` still makes the oracle ask the heap for what `tests/oracle-known-resource-defects.txt` says (OD-3, OD-5), through the counting oracle module (builds without sanitizers) |
| `conformance_class1` | ISO/IEC 15444-4 class 1, profiles 0 and 1, with the T.803 (2024) peak and MSE limits; skipped without the data |
| `corpus_regenerates` | the oracle's encoder writes the committed corpus byte for byte on this compiler and platform: the encoder oracle that stage 6a's byte identity relies on |
| `fuzz_replay.*` | the fuzz targets run over their seeds (the corpus; the JP2 header seeds, made by `fuzz_seeds.header`; the decode seeds with option bytes, made by `fuzz_seeds.decode`; `fuzz/seeds/roundtrip`, `fuzz/seeds/encode`) and `fuzz/regressions` on every platform; a replay fails when no input gets past the target's filters and guards, and the header target's when a header read differs from the oracle's without a departure to explain it |
| `fuzz_replay.decode_threads` | stage 5: the decode target built with `NJ2K_FUZZ_THREADS` over the same inputs: each one decoded on one thread and again through 2 and 4 parts of a pool, every stage forced, the statuses, the planes of both decodes and every message the same, then compared with the oracle as the plain target; the ThreadSanitizer job runs it. The plain decode target (`fuzz_replay.decode`, libFuzzer and ClusterFuzzLite) makes the same comparison through an executor that starts no thread and runs the parts one after the other, the last first (3 parts), so the sanitizers there see the threaded path's partitions |
| `fuzz_filters` | the fuzz targets' guards of the oracle: every decode bomb of `fuzz/bombs` kept from it (OD-3) or refused by it under the decode target's limits, with a limit's message; every header bomb refused by the header filter; the Second Life sized corpus files through both, decoding under the limits |
| `findings_c2` | the Rust twin's plan, finding C2 (whether a decode at the memory limit depends on the codec's history): for every corpus file and fuzz regression at reduce 0-5, the smallest `max_memory` a fresh codec decodes at, found by bisection; at one byte less a fresh codec refuses with `NJ2K_E_LIMIT` and so must a codec that decoded the same bytes before. Held at `--expect none`: the working-memory estimate refuses first at every boundary, so the work arena's history-dependent high-water mark never decides a status (311 cases with `fuzz/seeds` and `fuzz/bombs`, which run by hand) |
| `findings_c3` | finding C3, held at `--expect same` since the floating-point hotfix: every 9/7 corpus file up to 2^20 samples (26 of 28) decoded with every stage forced through an executor whose worker set MXCSR to the default (the control), round toward zero, flush-to-zero with denormals-are-zero, or both gives the samples of one thread. Before the fix round toward zero changed about half of them in every file. `fpenv_unit` covers the calling thread, round up and down, and the lossy encode |
| `fpenv_unit` | the output does not depend on the host's floating-point environment: 9/7 decodes (workers and calling thread, one thread and threaded) and a lossy encode under six MXCSR environments equal the default environment's, and the caller's MXCSR is put back after each call |
| `openjp2_whole`, `openjp2_whole_threads`, `openjp2_layers`, `openjp2_header`, `openjp2_conformance_differential` | the openjp2 module (`doc/api.md`) loaded by path as a viewer loads its OpenJPEG (`nj2k_diff`'s `lib:` backend, the requests driven through the oracle's own `openjpeg.h`): whole files at every reduce level in plain and viewer mode against the oracle's reference tables (the core's departures left out), the same with 4 threads asked of every decoder (`--threads`: the large corpus files reach the module's pool), the layer-boundary oracle, the header reads, and with the test data every conformance file; the cut points are the core's, tested above |
| `openjp2_unit` | the module's own behaviour: `opj_version` is "nj2k" and Firestorm's engine string built from it (`OpenJPEG: 2.5.4, Runtime: nj2k`) fits the 31 characters its texture cache header holds; `opj_codec_set_threads` starts, replaces, caps (64) and refuses (negative count, no codec) as documented, answers true for an encoder, and `opj_destroy_codec` joins the threads (the pool's threads, which the module names, counted: five codecs at once, none for an encoder); 144 decodes of the large corpus files at 0, 1, 2, 3, 8 and 64 threads, reduce 0-2, plain and viewer, equal to the oracle's; four decoders at once with pools of 2 to 5 threads (the ThreadSanitizer job runs it); the viewer's upload encode, lossless and lossy, byte for byte the oracle's |
| `openjp2_exports`, `openjp2_forwarders` | the module exports exactly the 55 functions of the oracle's `openjpeg.h`, all `opj_*` and no `nj2k_*` (`tools/check_openjp2.py`); `modules/openjp2/opj_forward.inc` is current, and every function's return and parameter types equal `openjpeg.h`'s (`tools/gen_openjp2.py --check`) |
| `exports` | `neoj2k.dll` / `libneoj2k.so` export exactly the functions `neoj2k.h` declares, all `nj2k_*`, as `abi/exports.txt` lists |
| `global_state` | the static library's objects hold no writable globals, no thread-local storage, no static initialisers and no LTO objects (MSVC `/GL`, LLVM bitcode, GCC `.gnu.lto_*`); `nj2k_cpu_supported()`'s object needs nothing beyond baseline x86-64, and in the scalar build (`NJ2K_SIMD=SCALAR`) no object of the library does |
| header checks (build) | `neoj2k.h` and the compatibility header compile as strict C99 (MSVC: C11) and C++20 with warnings as errors |

The reference tables are committed: configuring without one, or with an `NJ2K_TEST_CUT_STEP` it does not
hold, is an error, and `nj2k_diff verify` fails when no line of the table matches its options, so no
reference test can pass having checked nothing. The tests, tools, fuzz targets and the bench clear
`OPJ_NUM_THREADS` before any OpenJPEG codec exists, so the oracle never starts its thread pool.

Nightly (`NJ2K_TEST_NIGHTLY=ON`): `oracle_cuts_nightly` and `neoj2k_cuts_nightly`, every cut point of the
generated files from 4 to 16 KB, in plain and in viewer mode.

Stage 1c's results on 2026-11-19, again on 2026-11-20 with the oracle at the hotfix head (Windows, MSVC
14.51 and clang-cl, RelWithDebInfo; the scalar MSVC build; MSVC with AddressSanitizer): every test passes (110
of 110, and 107 of 107 under AddressSanitizer, which has no bench tests; CI runs 109 in its plain builds and
106 in its sanitizer builds). neo-j2k reproduces the reference
table's 1,812 lines (756 whole-file lines and 1,056 cut digests over 1,028,088 decodes) but for the 30 of its
DP-3 file, and the nightly table's 204 digests over 1,239,348 decodes; the layer-boundary oracle's 198 checks;
class 1 conformance 23 of 23 in plain mode (18 of 23 in viewer mode, as the oracle); every conformance
codestream and JP2 file in plain, strict and viewer mode at reduce 0-5 (342 of 342 identical); OpenJPEG's
non-regression data, 894 decodes, identical but for the three DP-6 files. Both tables, recorded again at the
new pin, are byte for byte the committed ones.

Stage 6a's results on 2026-11-20 (Windows: MSVC 14.51, clang-cl, MSVC with AddressSanitizer, and the scalar MSVC
build; the oracle at the hotfix head): every test passes (109 of 109, the two class 1 conformance tests aside, which
need the fetched data); `encode_unit` writes 3,172 + 100 + 156 codestreams (and the cases below) byte for byte the oracle's and
refuses what the oracle refuses, in each build; the encode fuzz target replays its 36 inputs (30 seeds and 6 regressions; 13 reach the code under test)
with no difference, and its local runs of 15,000 random inputs (about 3,000 of them encoded, each decoded by the
oracle and, in the viewer's settings, compared with the oracle's encoder) found no difference, under
AddressSanitizer too. The decoder's reference tables and
`corpus_regenerates` are unchanged (the Tier-1 tables moved to a shared header, `src/t1_tables.h`).

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
# the zero-heap gate per request on the bench corpus (stages 2-4; every file up to 512 x 512), in each build
nj2k_decode_unit corpus/generated --heap-files <bench list>
# every texture of the viewer's cache: header facts against the oracle, and the Second Life profile at
# reduce 0-5 through the API as the viewer will ask (the header read with the factor set before it and
# NJ2K_REDUCE_CLAMP, then nj2k_get_decoded_dimensions); --opensim for the OpenSim profile, exit status
# 2 when the build lacks it (prints aggregates only)
nj2k_cache_check "%LOCALAPPDATA%\Vulkanstorm_x64\texturecache"
```

Results on 2026-11-19 (Windows, MSVC 14.51), again after the review round: the bench corpus, 69,478 reads,
0 different; the cache, 60,045 textures, every header read as the oracle reads it, 360,270 profile checks
at reduce 0-5 with none refused, under the Second Life profile and under the OpenSim one (the largest: 5
components, 8 layers, 1 tile, 2,048 px a side, 81.3 MiB of the 256 MiB working memory). Again in stage
1c's review round, with its estimate (the packet iterator's table counted, and the arena's doubling
reservation for the structures): the cache then held 12,942 textures, every header read as the oracle
reads it, 77,652 profile checks with none refused under either profile, the largest 82.2 MiB. OpenJPEG's
non-regression data (`tools/fetch_testdata.py --all`, 149 codestreams and JP2 files, 374,674 reads with
prefixes): 4 files differ, every one refused by neo-j2k where OpenJPEG reads it, for DP-6 (3) and DP-7
(1); none reads in neo-j2k that OpenJPEG refuses.

## Stage 1c's local gates (private data, on the owner's machine)

```
# the damaged corpus: the bench's 2,068 damaged codestreams (eleven kinds of damage to each of its 188
# textures: flipped bits in the body and the headers, cut headers, SOT markers and bodies, changed SIZ,
# COD and QCD values, length fields and markers), every outcome against the oracle's, in a RelWithDebInfo
# and an AddressSanitizer build
nj2k_diff compare --a oracle --b neoj2k --files <mutant list> --reduce 0,5 --modes plain,viewer \
    --kinds full --allow-diff --csv damaged.csv
# the same files through the decode fuzz target: the memory path under the Second Life limits, its
# memory bound, its guard bytes and its balance checked, and the comparison with the oracle under the
# same limits
nj2k_fuzz_decode_replay <mutant folder>
```

Results on 2026-11-19 (Windows, MSVC 14.51, RelWithDebInfo and AddressSanitizer), stage 1c's review round,
and the same on 2026-11-20 with the oracle at the hotfix head: 8,272 damaged decodes (the plan's 4,136 at
reduce 0 and 5, in plain and in viewer mode), the same outcome table from both builds and from both pins,
row for row, no crash, hang or sanitizer report (50 s, and 3 min 57 s under AddressSanitizer). Both
decoders refuse 4,165 requests and decode 3,695 to the same image; in 412 (103 files) OpenJPEG decodes
what neo-j2k refuses at the header, every one a recorded departure: a reserved quantization style (DP-8,
71 files) or fewer step sizes than sub-bands (DP-6, 32 files). neo-j2k decodes nothing OpenJPEG refuses.
The decode fuzz target took all 2,068 files without a stop (no memory beyond its bound, no byte written
outside the planes, every allocation given back, no difference the departures do not explain), since
2026-11-20 comparing each with the oracle under the same limits, with no filter: the OD-3 guard kept one
file (a SOT declaring more than 64 MiB) from the oracle. The outcome table names private textures, so it
stays off the repository.

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
  one layer, a plain cut). neo-j2k decodes every input from memory, with no filter, under the Second Life
  limits with 64 MiB of working memory, twice (the second time from the header it holds), into 8-bit
  planes with guard bytes around them, sized only once `nj2k_get_decoded_dimensions` accepts the header
  (a header it refuses must be refused by the decode too, with the same status); its allocations go
  through a counting hook, and the target aborts when the codec holds more than twice the
  working-memory limit plus its header state, or does not give everything back, or writes a guard
  byte. Then the oracle decodes the same request under the same limits (the Second Life profile with
  `max_memory` at 64 MiB, through the fork's `opj_decoder_set_limits`), and so does neo-j2k through the
  same request code. The two must agree (outcome, image, the viewer's 8-bit copy), unless one of them
  refused for memory (each library counts its own structures against `max_memory`; their other limits
  are the same and must refuse alike), the codestream shows DP-3, DP-5 to DP-8, DP-12 or DP-14
  (`fuzz/departures.h`, shared with the header target, each check as narrow as its departure), or the
  images differ only in samples one decoder clamps to the minimum and the other to the maximum (DP-2).
  Both targets replay OpenJPEG's conformance and non-regression data (318 files) without an
  unexplained difference.
- `roundtrip`: a small image and its coding from the input, encoded losslessly and decoded: every sample
  of every stream the encoder writes must come back. Nothing is skipped: the oracle's encoder refuses an
  image its guard bits cannot hold (OD-6) and keeps terminated passes inside its buffers (OD-4), both
  fixed at the pin. Seeds: `fuzz/seeds/roundtrip`.

- `encode` (stage 6a): sixteen bytes choose an image (sides up to past the encoder's limits, 1 to 10 components,
  mostly 8 bits unsigned, now and then another precision, signedness, subsampling or an offset) and OpenJPEG's
  encoder settings (resolutions, layers and rates, wavelet, colour transform, comment, and now and then a setting the
  core does not take: tiles, precincts, another progression, code-block size, mode switches, tile-parts, a region of
  interest; a size cap it takes and compares); the rest are the samples, some of them out of the 8-bit range. neo-j2k's encoder takes it
  through its own API: it must not crash or hang, a refusal must be `NJ2K_E_UNSUPPORTED`, `NJ2K_E_LIMIT`,
  `NJ2K_E_ARGUMENT` or `NJ2K_E_NO_MEMORY`, writing nothing for an unsupported setting, and a codestream it writes
  must decode in the oracle to the image (the samples, when lossless: the 5/3 with a last rate of at most 1 as
  the setup writes it back, after a size cap has lifted it). Where the settings are the viewer's, the
  oracle's encoder takes the same image and the two must write the same bytes or both refuse (departures DP-16 and
  DP-18 are kept from the oracle). Seeds: `fuzz/seeds/encode` (`make_inputs.py --encode-seeds`). Local runs of 15,000
  random inputs (about 3,000 of them encoded) found nothing.

The round-trip target drives the oracle (the encoder's differential is the encode target's), and the header and decode targets
run the oracle beside neo-j2k, guarded against OpenJPEG's known bombs (fork audit S2, S3; OD-3, OD-5;
`fuzz/fuzz_common.h`). The header target reads with no limits behind a filter (for a JP2 file, applied
to the codestream OpenJPEG reaches through its boxes) that bounds the tiles and components of SIZ,
which must follow SOC. The decode target has no filter: the oracle decodes under the limits above, which
it checks from the headers before it allocates what they bound (tiles, components and layers, the
decoded side, and through `max_memory` the structures of every resolution, OD-5). The one bomb they
leave, a tile-part's declared length (OD-3), is kept from it in the non-strict modes by a scan of the
bytes for any SOT that declares more than 64 MiB. `fuzz_filters` checks the guards against
`fuzz/bombs`.

Pull requests: ClusterFuzzLite, five minutes each with ASan, UBSan and MSan, from the corpus batch
fuzzing stores and on the code the change touches. ClusterFuzzLite batch fuzzing runs an hour a day with
the three sanitizers (`cflite-batch.yml`), then prunes the stored corpus (`cflite-prune.yml`) and measures
its coverage (`cflite-coverage.yml`); corpus and coverage are workflow artifacts. Nightly: ten minutes a
target with ASan and UBSan, on the corpus the earlier nights grew (kept in the Actions cache). Local: the
`linux-clang-fuzz` preset, or MSVC with `-DNJ2K_BUILD_FUZZERS=ON -DNJ2K_SANITIZE=address`.

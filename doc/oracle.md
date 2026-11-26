<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026, Anne Skydancer (neo-j2k) -->

# The oracle

neo-j2k's decoded pixels are defined by OpenJPEG 2.5.4 as the viewer ships it (owner decision D2):
whole files bit-identical at every reduce level, lossy included. Partial downloads follow upstream
OpenJPEG by discarding accumulated chunks of code-blocks whose contribution is cut. The oracle
retains the fork's bounded parsing and packed-header safety fixes, with that reconstruction policy
restored by `upstream-partial-blocks.patch`.

## What it is

- `oracle/PIN` names the fork (`anne-skydancer/openjpeg`), the commit and the patches (`upstream-partial-blocks.patch`).
- The pin: `66b16561`, the head of the fork's `vkstorm-2.5.4-hotfix` branch, as the package `3p-openjpeg`
  `v2.5.4-cpu-avx2.6` builds it (its recipe commit `1ad8ff6`); `841b2de4` is the head of `v2.5.4-cpu-avx2.5` (tag `dc27a45`). It is OpenJPEG 2.5.4 as the package `v2.5.4-cpu-avx2.3`
  ships it (`3b1a6a96`) with the 1a decoder fixes:
  - every complete codeword segment of a cut tile kept, a cut packet header an absent packet (`c51e47ed`:
    the fork audit's general truncation fix, which the oracle carried as a patch before);
  - a reduce factor checked before it applies (`14277522`);
  - decoder limits, `opj_decoder_limits_init` and `opj_decoder_set_limits` (`f7a8f43d`, refused after the
    header since `6bad83c0`, a whole decode counted against the memory limit since `19c04f26`, a JP2
    palette's components against the component limit since `ed7727f8`): opt-in, and no viewer code sets
    them yet, so the reference tables are recorded without them; the decode fuzz target sets them
    (`doc/testing.md`); tile parameters allocated only for tiles that appear;
  - the packet walk stopped once a tile's data is exhausted (`5b0ac326`);
  - cut codestreams with EPH markers (`1666206e`);
  - the last bytes of a cut `Psot = 0` tile-part kept (plan W5), and 9/7 tiles that vanish at the decoded
    resolution decoded (W9) (`dfcc4d1c`);
  - packed packet headers (PPM, PPT) kept in step with the packets of a cut tile: an absent packet
    consumes its packed header, and a code-block that loses passes takes no more (`06efaa3c`, `5c869d3a`),
    which fixed OD-1;

  and the fixes `v2.5.4-cpu-avx2.5` adds:
  - the windowed inverse wavelet of `opj_set_decode_area` starts from defined samples (`f83f98e3`): no
    decoded sample changes, and neither the viewer nor the tests set a decode area;
  - the MQ encoder's code-block buffer covers the passes a code-block terminates (`e64db843`), which fixed
    OD-4;
  - the encoder refuses an image whose code-blocks do not fit the guard bits of their band (`841b2de4`),
    which fixed OD-6.

  and the fixes `v2.5.4-cpu-avx2.6` adds (`66b16561`, found by the UBSan fuzz run of PR #13; no encoded byte
  changes):
  - the encoder's check of the resolution count against the tile size shifts an unsigned 1 (a signed shift by 31
    was undefined at 32 resolutions);
  - the rate allocator's maximum squared-error sum shifts an unsigned 1 by the component precision (`tcd.c`).

  The viewer ships `v2.5.4-cpu-avx2.4` (`03b3cddc`) until its next OpenJPEG change; the two decode alike
  (the move to the hotfix head, below).
- The pin follows the published hotfix: when the fork's package moves on, the pin moves with it.
- `oracle/openjp2/` holds the library sources the package compiles (no GPU back-ends, no tools), with
  OpenJPEG's own notices, and the fork's `LICENSE`. `tools/oracle_sync.py --verify` (CI, lint job)
  fetches the pinned commit, applies the patches and compares every file.
- `oracle/CMakeLists.txt` builds `nj2k_oracle`, a static library, with what decides the output exactly as
  the package: `/fp:precise /arch:AVX2` (MSVC), `-mavx2 -ffp-contract=off -fno-fast-math` (GCC, Clang), and
  `-ffp-contract=off` for clang-cl too, which contracts under `/fp:precise` (literature audit A4).
- The oracle links next to neo-j2k without clashes: neo-j2k exports `nj2k_*` only.

## Moving the pin

```
python -I tools/oracle_sync.py --commit <commit> --patches ""
```

`--source <checkout>` takes a local checkout of the fork instead of fetching, if it stands at that commit
with the vendored files unchanged. Then, in this order:

1. `python -I tools/gen_compat.py --check`: new API in `openjpeg.h` is mapped, or listed in `PENDING`
   with the stage that adds it (`doc/api.md`).
2. Rebuild and run CTest against the tables as they are, before re-recording anything: a mismatch is
   either a change the new commit lists, or its regression.
3. Re-record the tables (`doc/testing.md`) for the changes it explains only, record them below, and
   review the known-defect lists, whose checks fail once a listed defect is gone.

## The move to 1a (2026-11-19)

From `3b1a6a96` plus `0001-keep-layers-general.patch` to `dfcc4d1c`, checked against the tables recorded
with the fix-only oracle (commit `24e535a`), which are the fix-only reference of 1a's own gate (plan audit
B3). Windows, MSVC 14.51.

| Check | Result |
|---|---|
| Generated corpus, whole files (708 decodes, reduce 0-5, plain and viewer) | identical, but `psot0_sop_noeoc_97.j2k` at reduce 0 in both modes: W5 keeps the last two data bytes of its `Psot = 0` tile-part, which has no EOC |
| Generated corpus, every cut point up to 4 KB (1,028,088 decodes, 1,056 digests) | identical for the 59 files without `Psot = 0` (907,128 decodes); the four `psot0_*` files differ in 82 of their 96 digests, as W5 says: no cut ends with EOC, so every cut keeps two more bytes (and in each sweep of every cut point two more decode: those with less than two bytes after SOD) |
| Nightly table, cut points from 4 to 16 KB (1,239,348 decodes) | identical |
| Layer-boundary check (198 checks) | as before: the five OD-1 cases are still refused (1a's EPH change does not reach packed headers) |
| ISO/IEC 15444-4 class 1, T.803 (2024) limits | 23 of 23 |
| W9, outside the tables (ISO data is fetched, and class 1 decodes at reduce 0) | `p1_05.j2k` at reduce 5 and `p1_06.j2k` at reduce 2 and 3 decode (16x16, 3x3, 2x2), where the shipped DLL refuses them; reduce 1 is unchanged |
| Private bench corpus (212 files, the viewer's request): whole files against the shipped `openjp2.dll` (sha256 `2fd94064…`) | 1,272 of 1,272 identical |
| The same files, every decode of the fork audit's comparison (whole, viewer byte budget, first 600 bytes; 2,050) against its fix build `openjp2-fix.dll` | 2,050 of 2,050 identical, in viewer and in plain mode |
| The same cut decodes against the shipped DLL (expected to differ: the fix keeps delivered bytes) | budget 531 of 613 and first request 122 of 165 identical, the counts the fork audit measured for its fix |

The 84 changed lines of `corpus/generated/oracle-reference.tsv` were re-recorded; nothing else changed,
and the nightly table did not. Run the comparisons with `nj2k_diff compare` (`doc/testing.md`).

## The move to the published 1a (2026-11-19)

From `dfcc4d1c` to `03b3cddc` (the `v2.5.4-cpu-avx2.4` package), checked against the tables as they
were, then re-recorded. Windows, MSVC 14.51.

| Check | Result |
|---|---|
| Generated corpus, whole files (756 decodes, reduce 0-5, plain and viewer) | identical |
| Generated corpus, every cut point up to 4 KB (1,028,088 decodes, 1,056 digests) | identical but the 22 digests of `packed_ppm_sop_eph_97.j2k`, where more cut points decode (1,062 of 1,453 at a step of 1, 81 of 112 at 13; before 636 to 650 and 49 to 50): the review round's packed-header fix (`06efaa3c`, `5c869d3a`) |
| Nightly table, cut points from 4 to 16 KB (1,239,348 decodes) | identical |
| Layer-boundary check (198 checks) | no mismatch; the five OD-1 cases decode as the whole file limited to their layers, so OD-1 left the known-defect list |
| The rest of CTest (header facts, conformance, fuzz replays, known resource defects) | unchanged |

The 22 changed lines of `corpus/generated/oracle-reference.tsv` were re-recorded; nothing else changed,
and the nightly table did not.

## The move to the hotfix head (2026-11-20)

From `03b3cddc` to `841b2de4` (the `v2.5.4-cpu-avx2.5` package), checked against the tables as they
were, then recorded again. Windows, MSVC 14.51.

| Check | Result |
|---|---|
| Generated corpus, whole files (756 decodes, reduce 0-5, plain and viewer) | identical |
| Generated corpus, every cut point up to 4 KB (1,028,088 decodes, 1,056 digests) | identical |
| Nightly table, cut points from 4 to 16 KB (1,239,348 decodes) | identical |
| Both tables recorded again with the new pin | byte for byte the committed ones |
| The corpus encoded again (`corpus_regenerates`) | byte for byte: the encoder's new refusal leaves every image it encoded as it was |
| `oracle_known_defect.od6` | fails, as it should: the encoder refuses OD-6's image ("needs 3 bit planes, its band holds 2 with 2 guard bits") |
| OD-4's input under AddressSanitizer, through the round-trip target without its TERMALL skip | its code-blocks are coded with no write past a buffer; the encode then fails in Tier-2 ("only 12 bytes remaining in output buffer. 62 needed"), as OpenJPEG refuses an image whose terminated passes outgrow the tile's output buffer |
| The rest of CTest (header facts, layer boundaries, conformance, fuzz replays, known resource defects) | unchanged |

No line changed: the decoder's only change, the windowed wavelet, is on no path the tables take. The
checks of OD-4 and OD-6 left with their defects; their inputs are round-trip regressions in
`fuzz/regressions`, and the round-trip target lost its TERMALL skip and its model of the guard bits
(`doc/testing.md`).

## Known defects of the oracle

Each open defect has an input and a check that fails once the defect is gone, so the list follows the
pin: `tests/oracle-known-defects.txt` (the layer-boundary check) and
`tests/oracle-known-resource-defects.txt` (heap requests, through the counting oracle module). The
inputs are in `tests/known-defects` and `corpus/generated`, those of fixed defects in
`fuzz/regressions`. They are numbered OD-n, apart from the owner's decisions D1-D15 and the fork
audit's findings O1-O3.

- **OD-1 (fixed at the pin): a cut packed-header stream with EPH markers was refused.**
  `corpus/generated/packed_ppm_sop_eph_97.j2k` (PPM, SOP, EPH, three layers) cut where layer 2 starts (484
  bytes), or anywhere before layer 3, gave "Expected EPH marker" and no image, in the shipped DLL and in
  the oracle up to `dfcc4d1c`: a code-block's first inclusion was recorded (`numsegs`) only when its data
  was read, so once the data ran out the later packed headers were parsed as if those code-blocks had
  never been included. The fork's review round keeps the packed headers in step (`06efaa3c`,
  `5c869d3a`); since the pin `03b3cddc` the layers that arrived decode, and neo-j2k matches it (DP-4 is
  closed).
- **OD-2: a tile whose tile-parts hold no data bytes is refused.** With packed headers, a tile whose packets
  are all empty has no bytes after SOD; that is valid, but `opj_j2k_decode_tile` refuses a tile without
  data. Kept as `corpus/generated/edge_ppt_empty_tiles_97.j2k` (the reference records the refusal);
  neo-j2k decodes such a tile as the standard has it (departure DP-3, decided 2026-11-19).
- **OD-3: a tile-part's declared length is allocated before it is read.** In non-strict mode
  `opj_j2k_read_sod` (`j2k.c:5122-5149` at the pin) only warns when `Psot` exceeds what is left of the
  stream and then allocates all of it. `sl_32_rgb_97.j2k` (782 bytes) with `Psot` set to `0xFFFFFF00`
  decodes normally and commits 4,105 MB per decode, in the shipped DLL and in the oracle, through the
  viewer's request; a commit bomb on every decode thread, which no viewer guard sees and which 1a's
  limits do not bound (`max_memory` covers the tile parameters and the tile's structures, not the data
  buffer). Found by ClusterFuzzLite on the stage-0 pull request (an out-of-memory at `j2k.c`), measured
  with `nj2k_diff decode`. Input: `tests/known-defects/od3-psot-1g.j2k` (`Psot` = 1 GiB; its request asks
  the heap for 1,024 MiB). Fix for the fork: in non-strict mode allocate at most the bytes left. The
  decode fuzz target keeps from the oracle meanwhile, in the non-strict modes, any stream in which a SOT
  declares more than 64 MiB (a scan of every byte: the one bomb its limits leave); neo-j2k never
  allocates from `Psot`.
- **OD-4 (fixed at the pin): the MQ encoder wrote past the code-block buffer** (fork audit S5). The lossless
  round-trip fuzzer (ClusterFuzzLite, ASan) found a one-byte heap write past an 83-byte code-block buffer
  (`opj_mqc_byteout`, `mqc.c:494`, from `opj_mqc_flush`, allocated in
  `opj_tcd_code_block_enc_allocate_data` with 74 bytes plus 4 a sample): a 28x2 image, three 16-bit
  signed components, 4x32 code-blocks, RESET and TERMALL, four layers, EPH. Every terminated pass adds
  bytes the fixed margin does not cover; upstream PR #1649 reports the same with 16-bit input and
  TERMALL. Reproduced with MSVC's ASan as well. The round-trip target skipped TERMALL above 8 bits. That
  bound was probed under MSVC's ASan with random TERMALL encodes (every mode switch combination with it,
  1-64 px, 1-5 components, the colour transform, code-blocks of 4-64, up to four layers): 16,192 at 1-8
  bits and 1,947 at 9 bits wrote nothing past a buffer, while 9-16 bits overflowed in every batch of
  1,000. By count, 8 bits through the colour transform and the HH gain give at most 12 bit-planes, 34
  terminated passes of about two bytes each, inside 78 bytes for a one-sample code-block; 16 bits give
  58 passes. The viewer's uploads (8-bit, no mode switches) never reached it. The fork's `e64db843`
  sizes the buffer for three bytes for each pass a code-block can terminate, part of the pin since
  2026-11-20: the round-trip target skips TERMALL no more, and the input is a round-trip regression,
  `fuzz/regressions/roundtrip-mqc-termall-16bit.bin`. Its encode now fails later, in Tier-2: the tile's
  output buffer, sized from the samples, does not hold the terminated passes, and OpenJPEG refuses the
  image there, with no write past a buffer. neo-j2k's encoder bounds its MQ output itself (stage 6a).
- **OD-5: a reduced decode builds the structures of the full resolution.** `opj_tcd_init_tile`
  (`tcd.c:1013` at the pin) sets up the precincts, code-blocks (each with its segment array) and tag trees
  of every resolution, whatever the reduce factor; only the samples are sized to the decoded level. A
  119-byte 2048 x 2048 RGBA header with 4 x 4 code-blocks, decoded through the viewer's request at reduce 5
  (a 64 x 64 output), asks the heap for 392 MiB in about a million calls (the audit measured 416 MB of
  commit; an 8192 x 8192 grey one 1,669 MB in 1.5 s, the same with 64 x 64 code-blocks 8 MB). The viewer's
  decode guard bounds only the samples after the reduce, and 1a's `max_memory` would bound it but is not
  set by the viewer. Input: `tests/known-defects/od5-fullres-cblk4-2048-rgba.j2k`. neo-j2k keeps
  header-only state for the resolutions it drops (plan section 2.11) and its limits bound the rest
  (stage 1b); the decode fuzz target sets the oracle's `max_memory` (64 MiB), which bounds them.
- **OD-6 (fixed at the pin): the encoder's two guard bits overflowed unchecked.** OpenJPEG's encoder
  gives every component two guard bits (`opj_j2k_setup_encoder`, `j2k.c:8435-8438` at the pin; only the
  `GUARD_BITS=` option of `opj_encoder_set_extra_options` changes it) and never checks that a code-block fits its band. A
  band holds Mb = precision + gain + guard bits - 1 bit planes (gain 0 for LL, 1 for HL and LH, 2 for
  HH), and the 5/3 transform can grow a band by up to 1.5 in each direction at every level, against
  the factor of 4 two guard bits leave. A code-block that needs more bit planes than its band holds
  gets a negative zero-bit-plane count (`t2.c:825`), which the tag tree writes as 0: the decoder reads
  its passes from the wrong bit plane, and the samples come back different, with no message from
  either end. The lossless round-trip fuzzer found it (ClusterFuzzLite with ASan on the stage-0 pull
  request, run 44467415713): a 33 x 31 image of two signed 1-bit components at five resolutions, whose
  second component reaches 4 in the coarsest LL band, where two bit planes hold 3; 127 of that
  component's 1,023 samples come back different. With `GUARD_BITS=3` the image round-trips; at one to
  four resolutions, or at 2 bits, the same samples do too. The round-trip target computed the
  transform the encoder applies (the DC level shift, the RCT, the 5/3 levels) and compared the samples
  only where every band fit. The fork's `841b2de4`, part of the pin since 2026-11-20, makes the encoder
  refuse such an image ("use at least 3 guard bits"), and every image it encoded correctly encodes as
  before (`corpus_regenerates`): the round-trip target compares every stream the encoder writes, and
  the input, `fuzz/regressions/roundtrip-guard-bit-overflow.bin`, stays a regression the encoder
  refuses. The viewer's reversible uploads have the same two guard bits (8-bit samples, the colour
  transform, six resolutions from 64 px), so a contrived texture could come back different until the
  viewer takes the fixed package. neo-j2k's encoder is held to the oracle at this pin, its refusal
  included (D9, stage 6a).

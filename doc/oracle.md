<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026, Anne Skydancer (neo-j2k) -->

# The oracle

neo-j2k's decoded pixels are defined by OpenJPEG 2.5.4 as the viewer ships it (owner decision D2):
whole files bit-identical at every reduce level, lossy included, and cut files identical to the fork
with the general truncation fix (D1). The oracle is that source, built into the tests.

## What it is

- `oracle/PIN` names the fork (`anne-skydancer/openjpeg`), the commit and the patches.
- Today: commit `3b1a6a96` (what the package `v2.5.4-cpu-avx2.3` ships) plus
  `oracle/patches/0001-keep-layers-general.patch`, the fork audit's general truncation fix. Stage 1a's
  hotfix (branch `vkstorm-2.5.4-hotfix`) is not pushed yet; it carries this fix and more (caps, the
  exhaustion exit, `Psot = 0`, the 9/7 empty-tile guard).
- `oracle/openjp2/` holds the library sources the package compiles (no GPU back-ends, no tools), patched,
  with OpenJPEG's own notices, and the fork's `LICENSE`. `tools/oracle_sync.py --verify` (CI, lint job)
  fetches the pinned commit, applies the patches and compares every file.
- `oracle/CMakeLists.txt` builds `nj2k_oracle`, a static library, with what decides the output exactly as
  the package: `/fp:precise /arch:AVX2` (MSVC), `-mavx2 -ffp-contract=off -fno-fast-math` (GCC, Clang), and
  `-ffp-contract=off` for clang-cl too, which contracts under `/fp:precise` (literature audit A4).
- The oracle links next to neo-j2k without clashes: neo-j2k exports `nj2k_*` only.

## Moving the pin (when 1a lands)

```
python -I tools/oracle_sync.py --commit <1a commit> --patches ""
```

Then rebuild, re-record the reference tables (`doc/testing.md`) only if the outputs changed on purpose
(1a changes cut decodes of `Psot = 0` streams and three ISO decodes at reduced levels: audit B4), and
review the known-defect list below.

## Gates of stage 0 (Windows, MSVC 14.51, 2026-11-19)

| Check | Result |
|---|---|
| Whole files of the bench corpus (212 files, reduce 0-5, the viewer's request): oracle against the shipped `openjp2.dll` (sha256 `2fd94064…`) | 1,272 of 1,272 identical |
| Every decode of the fork audit's comparison (full, viewer byte budget, first 600 bytes; 2,050): oracle against its fix build `openjp2-fix.dll` | 2,050 of 2,050 identical, in viewer and in plain mode |
| Cut decodes against the shipped DLL (expected to differ: the fix keeps delivered bytes) | budget 531 of 613 and first request 122 of 165 identical, the same counts the fork audit measured for its fix |
| ISO/IEC 15444-4 class 1, T.803 (2024) limits | 23 of 23 |

Run them with `nj2k_diff compare` (`doc/testing.md`).

## Known defects of the oracle

`tests/oracle-known-defects.txt` lists them for the layer-boundary check; the check fails when a listed
case starts to pass, so the list follows the pin.

- **O-1: a cut packed-header stream with EPH markers is refused.** `corpus/generated/packed_ppm_sop_eph_97.j2k`
  (PPM, SOP, EPH, three layers) cut where layer 2 starts (484 bytes), or anywhere before layer 3, gives
  "Expected EPH marker" and no image, in the shipped DLL and in the oracle; the whole file limited to one
  layer decodes. Likely cause, from reading `t2.c` (not instrumented): a code-block's first inclusion is
  recorded (`numsegs`) only when its data is read, so once the data runs out the later packed headers are
  parsed as if those code-blocks had never been included, and the parse goes astray; EPH makes it fail,
  and without EPH it would go unnoticed. A build without the fix's segment close-up fails the same way, so
  the defect is OpenJPEG's, not the fix's. It bears on stage 1a's exhaustion exit (audit B3 asked for packed-header streams in its gate).
  neo-j2k keeps the complete layers here (`doc/departures.md`, D-4).
- **O-3: a tile-part's declared length is allocated before it is read.** In non-strict mode
  `opj_j2k_read_sod` (`j2k.c:5016-5046`) only warns when `Psot` exceeds what is left of the stream and then
  allocates all of it. `sl_32_rgb_97.j2k` (782 bytes) with `Psot` set to `0xFFFFFF00` decodes normally and
  commits 4,105 MB per decode (1,027 MB with `Psot` = 1 GiB), in the shipped DLL and in the oracle, through
  the viewer's request; a commit bomb on every decode thread, which no viewer guard sees. Found by
  ClusterFuzzLite on the stage-0 pull request (an out-of-memory at `j2k.c:5045`), then measured with
  `nj2k_diff decode`. Fix for the fork: in non-strict mode allocate at most the bytes left. The fuzz targets
  keep tile-parts above 64 MB away from the oracle meanwhile; neo-j2k never allocates from `Psot`.
- **O-2: a tile whose tile-parts hold no data bytes is refused.** With packed headers, a tile whose packets
  are all empty has no bytes after SOD; that is valid, but `opj_j2k_decode_tile` refuses a tile without
  data. Kept as `corpus/generated/edge_ppt_empty_tiles_97.j2k` (the reference records the refusal);
  candidate departure D-3.

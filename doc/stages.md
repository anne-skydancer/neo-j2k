<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026, Anne Skydancer (neo-j2k) -->

# Stages and the owner's decisions

The plan and its audit live with the viewer (Vulkanstorm `reports/neo-j2k-survey-2026-11-18/`); this is
the summary a stage needs. Each stage is a branch `stage-<id>` cut from the previous stage's branch, and
a pull request into it (stage 0 into `main`); the stack lands on `main` in order.

| Stage | Scope |
|---|---|
| 0 | skeleton: CMake (static and shared), the draft API and compatibility header, CI, the oracle, the corpus, the differential, cut-point, layer and conformance harness, fuzz targets, the bench |
| 1b | header model and limits: immutable main and tile-part headers, marker precedence, limits from the header before any allocation (profile modules), reduce policy, `NEED_DATA`, JP2/JPH box reader (done: `doc/api.md`, `doc/departures.md` DP-5 to DP-10) |
| 1c | decode core: flat layout in an arena, Tier-2 rewritten (every progression, POC, PPM, PPT, SOP, EPH, keep-every-byte cuts), OpenJPEG's Tier-1, wavelets and colour transforms ported, output into the caller's buffers; zero heap calls after warm-up; then the viewer switches its decodes (D11) (done: `doc/api.md`, `doc/departures.md` DP-2, DP-3, DP-5 and DP-7 in tile-part headers, DP-12 to DP-15, `doc/bench.md`; what is left is listed below) |
| 2 | fused output pass: the colour transform, the DC shift and the copy into the caller's buffers in one pass; the viewer's interleaved 8-bit image in registers sixteen pixels a step, regions of a few pixels a pixel at a time (done: `src/output.cpp`, `doc/bench.md`; the output tail 5.05-5.12x net at 1024^2 on a quiet machine with its planes 64-byte aligned as the decoder allocates them (4.88-5.07x on unaligned planes, which the library never produces), 3-8 % above the floor of reading its 12.6 MB of planes and writing its pixels: the plan's 5x in every quiet run) |
| 3 | row-wise wavelet on large levels: every access along whole rows, the horizontal pass fused into the vertical one, the vertical steps one register sweep in the steady state, the rows put in order and their columns interleaved in one pass at the end (9/7: 5.1-5.4x net at 1024^2 in each of fifteen runs, median 5.2x); small levels keep OpenJPEG's structure; int16 lanes for the 5/3 not tried (they need a range proof from the decoded bit-planes, D2) (done: `src/dwt.cpp`, `doc/bench.md`) |
| 4 | Tier-1 tuning, each kept only when a bench shows it: the MQ renormalisation by count-leading-zeros (kept under the faster-and-bit-exact rule; at about 6-8 % of Tier-1 it is below the plan's 10 % gate); empty-column skipping and select-based decisions (dropped); the lane-per-code-block MQ prototype (`bench/nj2k_mq_lanes.cpp`, not adopted); the renormalisation costs few-pixel requests some Tier-1 time (code layout; four ways of keeping it for whole decodes only measured slower), which the tile set-up's step sizes without `ldexp` more than pay back (`doc/bench.md`) |
| 5 | threads for large images and encodes |
| 6a-6b | encoder, byte-identical to OpenJPEG for good (D9), then its speed |
| 7a-7b | the native Rust twin, bit-identical to the C++ (no FFI; the C++ library is the oracle in its tests) |
| 8 | packages and the viewer relinked against `neoj2k.dll` / `libneoj2k.so`, A/B on the devel build |

Handed from 1b to 1c, and how 1c settled them:

- the per-thread decoder: the codec itself, kept by the caller (one per thread) and fed codestreams from
  memory; its working memory is kept between decodes up to `nj2k_decoder_set_retained_bytes` (8 MiB by
  default), so a warmed-up decoder makes no heap call for images up to 512 x 512, from the first repeat of any request on (`decode_unit`, the generated corpus in CI and the bench corpus locally);
- the tile-part order rules and the `Psot = 0` data length, applied as tile-parts are read, as OpenJPEG
  applies them (TNsot correction, `find_final_eoc`);
- each tile's parameters resolved when its first tile-part arrives (DP-6 checked there), the reduce factor
  checked against them under both reduce policies (DP-5: under `NJ2K_REDUCE_CLAMP` a tile whose own coding
  has fewer resolutions than the factor has no level of the image's size), and the working memory checked
  again with them when the tile's own COD or COC changes it;
- POC in both headers: the tile's list replaces the main header's, and a change's layer end is bounded by
  the tile's layers when it decodes, whatever the marker order (DP-12);
- the working-memory estimate holds for every corpus file at every reduce level (`decode_unit`
  `test_memory_bound`: the memory a decode takes beyond the header against `decode_working_memory`).

Stage 1c's gates that were not met, and how the owner settled them (2026-11-20):

- **Early stop in resolution-first streams** (plan 2.11): not done, by decision. OpenJPEG reads every packet
  header of a tile, those of the resolutions a reduced decode drops included, and fails the decode when one
  of them is damaged; stopping after the last resolution needed would decode such a stream instead. D2's
  identity of outcomes stays: the dropped resolutions keep reading their packet headers, and a damaged file
  fails as in OpenJPEG (an early stop would give the viewer incomplete textures). The dropped resolutions keep
  only the state their packet headers need, and the structure bytes per request are recorded
  (`test_memory_bound` prints the memory a decode takes at each reduce level of the file that takes most).
- **Large images through the stream API**: `nj2k_decode` holds the whole codestream (as `nj2k_decode_memory`
  holds the caller's) before it decodes, where OpenJPEG holds one tile's data at a time, so a multi-tile
  image peaks higher by about its codestream's size (`doc/bench.md`). The low-memory stream for large
  multi-tile images belongs to the j2k-tile module, not to the core; the memory API meets the gate.
- **The damaged corpus** (plan 3.2) is met: the bench's 2,068 damaged codestreams at reduce 0 and 5, in
  plain and viewer mode (8,272 decodes, twice the gate's 4,136), against the oracle in the RelWithDebInfo
  and the AddressSanitizer build, with no crash, hang or sanitizer report and every outcome recorded
  (`doc/testing.md`).

The core and its modules (owner, 2026-11-20): the library is j2k-core, which Vulkanstorm builds with every
module off: the decoder (1b, 1c), its speed (2-5, threads for uploads and large images), the encoder for the
viewer's settings (6a, 6b) and the packages and viewer switch (8). Modules come later, each a configure-time
option; a function of a module that is not built gives `NJ2K_E_UNSUPPORTED`, and the core never depends on a
module (j2k-extra, j2k-htj and the low-memory stream's place in j2k-tile as decided; the rest of each module's
contents as proposed):

| Module | What |
|---|---|
| j2k-extra | the full encoder |
| j2k-tile | decoding by tile, decode areas, the low-memory stream for large multi-tile images |
| j2k-htj | HTJ2K (Part 15) decode and encode |
| j2k-codestream | JPT streams, the codestream index, the dump |
| j2k-utils | the command-line tools (compress, decompress, dump) with their image-format input and output |

Decided (2026-11-18/19):

- D1 cut downloads keep every delivered byte (the fork's stage 1a hotfix now, neo-j2k by design).
- D2 whole files bit-identical to OpenJPEG 2.5.4, lossy included; no FMA, no fast-math.
- D3 the repository stays private until the C++ library and the Rust crate are at parity and fuzzed
  (superseded 2026-11-19: the owner made it public and keeps it public).
- D4 BSD-2-Clause.
- D5 OpenJPEG-compatible API: `nj2k_*` exports, a compile-time `opj_*` compatibility header.
- D6 Rust: a native twin, C++ as the oracle.
- D7 HTJ2K decode as a pluggable module later (not used by Second Life; patent question open).
- D8 AVX2 at compile time with scalar references compared bit for bit; threads only for large images
  and encodes; small images one thread each.
- D9 the upload encoder stays byte-identical to OpenJPEG (no PCRD-opt).
- D10 limits from the header before any allocation, as profile modules: Second Life (2,048 px a side,
  8 components, a decoded-memory budget, about 1,000 tiles), OpenSim (16,384 px, same components and
  budget); every cached texture must pass.
- D11 the viewer switches after the decode core (1c); OpenJPEG keeps uploads until the encoder lands.
- D12 static library or DLL, both for anyone; the Vulkanstorm viewer uses `neoj2k.dll` / `libneoj2k.so`;
  the C++ library stays maintained next to the Rust crate.
- D13 the texture cache's decoder-output version is bumped at the fork hotfix, not at the switch.
- D14 no packet index until re-decodes are counted.
- D15 the viewer's upload encode moves off the main thread (a viewer change).
- Distribution: source (BSD-2), built static or as a DLL; no binary drop-in for `openjp2.dll`.

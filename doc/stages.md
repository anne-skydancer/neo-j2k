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
| 2-5 | fused output pass; row-wise wavelet; Tier-1 tuning (kept only when a bench shows it, the lane-per-code-block block-coder prototype included); threads for large images |
| 6a-6b | encoder, byte-identical to OpenJPEG for good (D9), then its speed |
| 7a-7b | the native Rust twin, bit-identical to the C++ (no FFI; the C++ library is the oracle in its tests) |
| 8 | packages and the viewer relinked against `neoj2k.dll` / `libneoj2k.so`, A/B on the devel build |

Handed from 1b to 1c, and how 1c settled them:

- the per-thread decoder: the codec itself, kept by the caller (one per thread) and fed codestreams from
  memory; its working memory is kept between decodes up to `nj2k_decoder_set_retained_bytes` (8 MiB by
  default), so a warmed-up decoder makes no heap call for images up to 256 x 256 (`decode_unit`);
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

Stage 1c's gates that are not met, or met only in part (2026-11-19):

- **Early stop in resolution-first streams** (plan 2.11): not done. OpenJPEG reads every packet header of a
  tile, those of the resolutions a reduced decode drops included, and fails the decode when one of them is
  damaged; stopping after the last resolution needed would decode such a stream instead. That is a
  departure on damaged input, against D2's identity of outcomes, so it waits for an owner decision. The
  dropped resolutions keep only the state their packet headers need, and the structure bytes per request
  are recorded (`test_memory_bound` prints the memory a decode takes at each reduce level of the file that
  takes most).
- **Large images through the stream API**: `nj2k_decode` holds the whole codestream (as `nj2k_decode_memory`
  holds the caller's) before it decodes, where OpenJPEG holds one tile's data at a time, so a multi-tile
  image peaks higher by about its codestream's size (`doc/bench.md`). Reading tile-parts from the stream as
  the walk reaches them changes the stream input layer; the memory API meets the gate. Left for a later
  stage, or the owner's call.
- **The damaged corpus** (plan 3.2) is met: the bench's 2,068 damaged codestreams at reduce 0 and 5, in
  plain and viewer mode (8,272 decodes, twice the gate's 4,136), against the oracle in the RelWithDebInfo
  and the AddressSanitizer build, with no crash, hang or sanitizer report and every outcome recorded
  (`doc/testing.md`).

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

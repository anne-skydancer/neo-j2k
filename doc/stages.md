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
| 1c | decode core: flat layout in an arena, Tier-2 rewritten (every progression, POC, PPM, PPT, SOP, EPH, keep-every-byte cuts), OpenJPEG's Tier-1, wavelets and colour transforms ported, output into the caller's buffers; zero heap calls after warm-up; then the viewer switches its decodes (D11) |
| 2-5 | fused output pass; row-wise wavelet; Tier-1 tuning (kept only when a bench shows it, the lane-per-code-block block-coder prototype included); threads for large images |
| 6a-6b | encoder, byte-identical to OpenJPEG for good (D9), then its speed |
| 7a-7b | the native Rust twin, bit-identical to the C++ (no FFI; the C++ library is the oracle in its tests) |
| 8 | packages and the viewer relinked against `neoj2k.dll` / `libneoj2k.so`, A/B on the devel build |

Handed from 1b to 1c (the header model holds what they need; the decode loop decides):

- where the per-thread decoder lives, for the zero-heap gate (`doc/api.md`, open for the planner and the
  owner);
- the tile-part order rules (TPsot in sequence, OpenJPEG's TNsot correction) and the `Psot = 0` data
  length (`psot0_data_length`), applied as tile-parts are read;
- each tile's parameters with precedence applied and the quantization count of each tile-component
  checked (`resolve_tile`, every component in one pass; DP-6), and the working memory of each tile once its
  headers are in;
- POC with changes in both the main header and a tile's: OpenJPEG appends the tile's changes to the main
  header's, the standard has the tile's replace them; the model keeps both lists;
- the per-item figures of the working-memory estimate, against measured high-water marks.

Decided (2026-11-18/19):

- D1 cut downloads keep every delivered byte (the fork's stage 1a hotfix now, neo-j2k by design).
- D2 whole files bit-identical to OpenJPEG 2.5.4, lossy included; no FMA, no fast-math.
- D3 the repository stays private until the C++ library and the Rust crate are at parity and fuzzed.
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

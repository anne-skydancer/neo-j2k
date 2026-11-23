<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026, Anne Skydancer (neo-j2k) -->

# Tier-1 bench dumps

The code-blocks that the decoder's Tier-1 bench (`bench/nj2k_t1_dec.cpp`) times, six files of the same synthetic texture:
`256_lossy.blk` (75 code-blocks), `256_lossless.blk` (75), `512_lossy.blk` (179), `512_lossless.blk` (210), `1024_lossy.blk` (718),
`1024_lossless.blk` (777); 3,711,342 bytes in all. They are for timing, so unlike `tests/vectors/t1` they are not run by a ctest
test (the bench checks every block against the frozen reference before it times anything).

## Provenance

- The image is the synthetic texture of `bench/nj2k_encode_bench.cpp` (gradients, a few edges and noise from a fixed xorshift
  generator, three 8-bit components): it is computed by a formula, no file of any kind went into it. This repository's own
  data, under the repository's licence (BSD-2-Clause); the encoder is a port of OpenJPEG (BSD-2-Clause, see
  `LICENSE.OpenJPEG`); the dumps carry no OpenJPEG data.
- It was encoded by this repository's own encoder with the viewer's upload parameters (one layer, 64 x 64 code-blocks, code-block
  style 00; lossy = the 9/7 transform, lossless = the 5/3), at 256, 512 and 1,024 pixels square.
- Not in these files: any block derived from openjpeg-data, the conformance suite, or textures of the viewer. (The study's other
  dump set, the 35,390 conformance blocks, is kept out of the repository; `tests/vectors/t1/README.md` says where it is used.)
- The dumping tool is **not in the repository**: the blocks were written by `mqng_profile` (`--out DIR`, without `--files`) of the
  decoder study, which decodes the encoder's codestream through a copy of the library that writes each block as `t1_decode_cblk`
  receives it. That harness is archived at the tag `archive/studies` (`bench/mqng_study/`, see `doc/studies/README.md`); it builds only
  against the commit it was written for.
  This directory's files are the `256_lossy.blk` ... `1024_lossless.blk` of its output directory, byte for byte.

## Format

The "MQNG" format of `tests/vectors/t1/README.md` (every field a little-endian u32, except the code-block bytes): the magic `MQNG`, the block count, then for each
block `w h numbps orient roishift cblksty`, the segment count, `len passes` for each segment, the byte count and the bytes.

## Use

```
nj2k_t1_dec --rounds 21 --inner auto tests/vectors/t1_bench/*.blk
```

`--inner auto` repeats a file's blocks within a timed unit until the unit takes at least 30 Mcycles. The Linux comparison of two refs
is `.github/workflows/bench-decoder-compare.yml` (`doc/testing.md`).

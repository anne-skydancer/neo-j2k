<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026, Anne Skydancer (neo-j2k) -->

# Tier-1 test vectors

Code-blocks as the decoder's Tier-1 receives them (`T1Block`: size, bit-planes, orientation, ROI shift, code-block
style, the codeword segments' lengths and pass counts, the gathered bytes), dumped from real decodes. `tests/unit/t1_unit.cpp`
decodes each with the library and with the frozen reference and compares the coefficients.

- `corpus-generated.blk`: 3,777 code-blocks of the files of `corpus/generated` (styles 00, 01, 04, 05, 12, 28 and 3f among them).
- `study-256.blk`: 150 code-blocks of two 256 x 256 RGB study textures (lossy and lossless, the viewer's upload parameters).

Format ("MQNG", all little-endian u32 but the bytes): the magic `MQNG`, the block count, then for each block `w h numbps
orient roishift cblksty`, the segment count, `len passes` for each segment, the byte count and the bytes.

The dumps came from a copy of the library that writes each block as `t1_decode_cblk` is called; that tooling is not in the
repository. The full dump set (37,424 blocks, 18 MB: the 35,390 of the mode-switch corpus with the conformance files, and the
six study textures) is not committed; `NJ2K_T1_VECTORS` runs it where it exists.

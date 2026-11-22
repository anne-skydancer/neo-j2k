<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026, Anne Skydancer (neo-j2k) -->

# Fuzz seeds

- `roundtrip/`: coding parameters and samples for the round-trip target (`fuzz/fuzz_roundtrip.cpp`
  reads twelve parameter bytes, then samples): every progression, mode switch but TERMALL above 8 bits,
  1 to 5 components, 1 to 16 bits, signed samples, 1 to 6 resolutions, code-blocks from 4 to 64,
  layers, SOP, EPH and the colour transform. `fuzz/make_inputs.py --roundtrip-seeds` writes them.
- The decode target starts from `corpus/generated` and from `make_inputs.py --decode-seeds`, which the
  fuzz jobs and CTest make at run time: every corpus file with option bytes in front (the viewer's
  request at reduce 5 on its first 600 bytes and at reduce 2 on half the file, plain decodes at
  reduce 1 to 5, strict mode with one layer, a plain cut).
- The header target starts from `corpus/generated`.
- `t1/`: code-blocks for the Tier-1 target (`fuzz/fuzz_t1.cpp`: a header of six bytes, two bytes a segment, then the
  bytes), a few small ones of each code-block style taken from `tests/vectors/t1`.
  `fuzz/make_inputs.py --t1-seeds` writes them.

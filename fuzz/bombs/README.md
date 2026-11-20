<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026, Anne Skydancer (neo-j2k) -->

# Resource bombs the fuzz targets must keep from the oracle

Inputs that make the oracle (OpenJPEG) allocate or loop by what a header declares, each named for the
target it is made for: `decode-` (with the target's option bytes in front when the name ends in `.bin`)
and `header-`. `fuzz_filters` (`nj2k_fuzz_filter_check`) checks the guards the targets give the oracle
(`fuzz/fuzz_common.h`):

- a decode bomb is either kept from the oracle by the decode target's OD-3 guard (a SOT that declares
  more than 64 MiB, which OpenJPEG would allocate before reading) or refused by the oracle under the
  decode target's limits (the Second Life profile with `max_memory` at 64 MiB), which it checks before
  it allocates what they bound; the check decodes it exactly as the target does;
- a header bomb is refused by the header filter (the header target reads with no limits), and is only
  scanned;
- the Second Life sized corpus files pass both guards and decode under the limits.

`fuzz/make_inputs.py --bombs fuzz/bombs` writes them, from `corpus/generated/sl_32_rgb_97.j2k` and from
nothing: a 256 MiB Psot, in sight and hidden in an unknown main-header marker (with a length that
fits, and one that runs past the end); full-resolution code-blocks of 4 x 4 at 2048 to 65536 px behind
a reduced request, also from a tile-part COC; 65,535 layers from a tile-part COD and from a second main
COD; 65,535 tiles in SIZ, also behind an unknown marker.

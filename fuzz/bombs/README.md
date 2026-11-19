<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026, Anne Skydancer (neo-j2k) -->

# Resource bombs the fuzz filters must refuse

Inputs that make the oracle (OpenJPEG) allocate or loop by what a header declares. The fuzz filters
(`fuzz/fuzz_common.h`) must keep each from the target its name starts with: `decode-` from the decode
target (with the target's option bytes in front when the name ends in `.bin`), `header-` from the
header target. `fuzz_filters` (`nj2k_fuzz_filter_check`) checks that they do, and that the Second Life
sized corpus files still pass. Nothing decodes them.

`fuzz/make_inputs.py --bombs fuzz/bombs` writes them, from `corpus/generated/sl_32_rgb_97.j2k` and from
nothing: a 256 MiB Psot, in sight and hidden in an unknown main-header marker (with a length that
fits, and one that runs past the end); full-resolution code-blocks of 4 x 4 at 2048 to 65536 px behind
a reduced request, also from a tile-part COC; 65,535 layers from a tile-part COD and from a second main
COD; 65,535 tiles in SIZ, also behind an unknown marker.

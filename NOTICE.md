<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026, Anne Skydancer (neo-j2k) -->

# Notices

neo-j2k is licensed under the BSD 2-Clause License (`LICENSE`).

## OpenJPEG

Parts of neo-j2k derive from OpenJPEG (https://github.com/uclouvain/openjpeg), BSD 2-Clause, through the
owner's fork (https://github.com/anne-skydancer/openjpeg):

- `include/neoj2k/neoj2k.h` (the declarations of OpenJPEG's `openjpeg.h`, renamed),
- `src/image.cpp`, `src/defaults.cpp` (ported from `image.c` and `openjpeg.c`),
- `src/header.cpp`, `src/jp2.cpp` (new readers whose header checks and messages derive from `j2k.c`
  and `jp2.c`),
- `src/codec.cpp` (the image and codestream information a header read gives, and the messages of the
  reduce factor and the component selection, from `j2k.c` and `openjpeg.c`), `src/stream.cpp` (the
  stream's behaviour and the file stream's functions, from `cio.c` and `openjpeg.c`),
- `oracle/openjp2/` (OpenJPEG's library sources, the test oracle; never part of the library),
- `oracle/patches/` (patches to those sources, when `oracle/PIN` lists any; none today).

Each of these files keeps OpenJPEG's copyright notice; `oracle/openjp2/LICENSE` is OpenJPEG's licence.
`LICENSE.OpenJPEG` carries that licence and the notices of the ported files for the built libraries:
the install puts it, with `LICENSE` and this file, in `share/licenses/neoj2k`, and binary
distributions ship them.

## Test data

The ISO/IEC 15444-4 conformance files and OpenJPEG's other test data are fetched at test time by
`tools/fetch_testdata.py` and are not part of this repository; their notice
(`input/conformance/COPYRIGHT` in openjpeg-data) limits them to JPEG 2000 conformance testing. The
generated corpus (`corpus/generated`) is synthetic and part of neo-j2k.

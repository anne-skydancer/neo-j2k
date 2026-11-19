<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026, Anne Skydancer (neo-j2k) -->

# neo-j2k

A JPEG 2000 Part 1 codec (decode and encode, lossless 5/3 and lossy 9/7) in C++20, built as a static library or
as its own DLL (`neoj2k.dll`, `libneoj2k.so`). Its arithmetic is OpenJPEG 2.5.4's, so whole-file decodes are
bit-identical to it; everything around the arithmetic is rebuilt for small images, flawless partial decodes and
bounded resources. A native Rust crate follows later and is tested against the C++ library bit for bit.

Licence: BSD-2-Clause (`LICENSE`). Code ported from OpenJPEG keeps its own BSD-2-Clause notices.

The repository is private until the C++ library and the Rust crate are at parity and fuzzed.

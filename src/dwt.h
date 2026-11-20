// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The inverse wavelet transforms of a whole tile-component, ported from OpenJPEG 2.5.4's dwt.c
// (opj_dwt_decode_tile and opj_dwt_decode_tile_97; its notice is in dwt.cpp): the same lifting
// steps in the same order for every sample, so the results are OpenJPEG's, lossy included. The
// reference is OpenJPEG built for x86-64 with AVX2, as the viewer's package is: the 5/3 vertical
// pass takes OpenJPEG's SIMD formulas for each group of 16 columns and its scalar ones for the
// rest, and the scalar build here does the same, so both builds give the same samples even where
// the int32 arithmetic wraps. The 9/7 transform works on floats without contraction (D2).
// Stage 3: a level of at least 128^2 samples (9/7) or 512^2 (5/3) is transformed row-wise, every
// access along whole rows, the horizontal pass fused into the vertical one and the rows put in
// order at the end; smaller levels, which fit in the first caches, keep OpenJPEG's structure.
#pragma once

#include <cstddef>
#include <cstdint>

namespace nj2k::detail {

// A resolution's area in its own coordinates (tile-component at that level).
struct ResRect {
    std::int32_t x0, y0, x1, y1;
};

// The scratch dwt_decode needs (64-byte aligned), for resolutions 0 .. numres - 1.
std::size_t dwt_scratch_bytes(const ResRect* res, std::uint32_t numres, bool reversible) noexcept;

// Transforms data in place from resolution 0 up to numres - 1. data holds the decoded
// resolution's samples, w per row (the width of the tile-component's decoded resolution): int32
// for 5/3, float bits for 9/7, the sub-bands of each level in OpenJPEG's quadrant layout.
void dwt_decode(std::int32_t* data, std::uint32_t w, const ResRect* res, std::uint32_t numres, bool reversible,
                void* scratch) noexcept;

namespace kernels {
// Stage 1c's transform, OpenJPEG's structure (eight rows or columns at a time, gathered into
// opj_v8dwt_t), kept as the reference the current one is compared with (tests, the kernel bench),
// and the scratch it needs.
std::size_t dwt_scratch_bytes_reference(const ResRect* res, std::uint32_t numres, bool reversible) noexcept;
void dwt_decode_reference(std::int32_t* data, std::uint32_t w, const ResRect* res, std::uint32_t numres,
                          bool reversible, void* scratch) noexcept;
// The current transform with the scalar kernels, in every build (the AVX2 build's dwt_decode uses
// the AVX2 ones)
void dwt_decode_scalar(std::int32_t* data, std::uint32_t w, const ResRect* res, std::uint32_t numres, bool reversible,
                       void* scratch) noexcept;
// The row-wise transform on every level, whatever its size (dwt_decode takes it from an area on),
// with the scalar kernels or the build's
void dwt_decode_row_wise(std::int32_t* data, std::uint32_t w, const ResRect* res, std::uint32_t numres,
                         bool reversible, bool scalar, void* scratch) noexcept;

// The lifting kernels, scalar references and AVX2 versions, for the kernel tests. The 5/3 ones
// work on one row (h) or on cols columns at once (v); the 9/7 one on 8 rows or columns at once,
// interleaved as OpenJPEG's opj_v8dwt_t (sn low then dn high, cas the parity of the first sample,
// 8 floats per sample). tmp holds a line (h) or 16 lines (v) and is 32-byte aligned: the AVX2 5/3
// column kernel loads and stores it aligned, as OpenJPEG's does (dwt_decode's scratch is).
void idwt53_h_scalar(std::int32_t* row, std::int32_t sn, std::int32_t dn, std::int32_t cas,
                     std::int32_t* tmp) noexcept;
void idwt53_v_scalar(std::int32_t* col, std::size_t stride, std::int32_t sn, std::int32_t dn, std::int32_t cas,
                     std::int32_t cols, std::int32_t* tmp) noexcept;
void v8dwt_decode_scalar(float* wavelet, std::int32_t sn, std::int32_t dn, std::int32_t cas) noexcept;
#if defined(__AVX2__)
void idwt53_h_avx2(std::int32_t* row, std::int32_t sn, std::int32_t dn, std::int32_t cas, std::int32_t* tmp) noexcept;
void idwt53_v_avx2(std::int32_t* col, std::size_t stride, std::int32_t sn, std::int32_t dn, std::int32_t cas,
                   std::int32_t cols, std::int32_t* tmp) noexcept;
void v8dwt_decode_avx2(float* wavelet, std::int32_t sn, std::int32_t dn, std::int32_t cas) noexcept;
#endif
}  // namespace kernels

}  // namespace nj2k::detail

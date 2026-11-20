// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The inverse colour transforms (RCT, ICT) and the DC level shift of a decoded tile, after
// OpenJPEG 2.5.4's mct.c (opj_mct_decode, opj_mct_decode_real) and tcd.c
// (opj_tcd_dc_level_shift_decode): the same operations per sample, so the samples are OpenJPEG's.
// Departure DP-2: the 9/7 shift decides the 2^31 edge the same way on every platform (OpenJPEG's
// MSVC build rounds 2^31 to INT_MIN, its Linux build to 2^31), and the 5/3 shift adds in 64 bits
// (OpenJPEG's int32 sum is undefined on overflow); both clamp as OpenJPEG does.
#pragma once

#include <cstddef>
#include <cstdint>

namespace nj2k::detail {

// The whole-buffer forms, in place, as OpenJPEG runs them: the unfused reference of the output
// pass's tests and kernel bench (the library runs the row kernels below, from output.cpp).
// opj_mct_decode: n samples of three int32 components, in place
void mct_decode_rct(std::int32_t* c0, std::int32_t* c1, std::int32_t* c2, std::size_t n) noexcept;
// opj_mct_decode_real: n samples of three float components (their bits in int32 buffers), in place
void mct_decode_ict(std::int32_t* c0, std::int32_t* c1, std::int32_t* c2, std::size_t n) noexcept;

// opj_tcd_dc_level_shift_decode for one tile-component: w x h samples with stride samples per row,
// shift added and the result clamped to [lo, hi]; float samples (9/7) rounded to nearest, ties to
// even, first.
void dc_shift_53(std::int32_t* p, std::uint32_t w, std::uint32_t h, std::size_t stride, std::int32_t shift,
                 std::int32_t lo, std::int32_t hi) noexcept;
void dc_shift_97(std::int32_t* p, std::uint32_t w, std::uint32_t h, std::size_t stride, std::int32_t shift,
                 std::int32_t lo, std::int32_t hi) noexcept;

// The row kernels, the only copy of the per-sample operations: the scalar references and their
// AVX2 versions (the kernel tests compare them bit for bit). The colour transforms read c0-c2 and
// write o0-o2, which may be c0-c2 (in place); the DC shifts work on one row in place.
namespace kernels {
void rct_scalar(const std::int32_t* c0, const std::int32_t* c1, const std::int32_t* c2, std::int32_t* o0,
                std::int32_t* o1, std::int32_t* o2, std::size_t n) noexcept;
void ict_scalar(const std::int32_t* c0, const std::int32_t* c1, const std::int32_t* c2, std::int32_t* o0,
                std::int32_t* o1, std::int32_t* o2, std::size_t n) noexcept;
void dc_shift_53_scalar(std::int32_t* p, std::size_t n, std::int32_t shift, std::int32_t lo, std::int32_t hi) noexcept;
void dc_shift_97_scalar(std::int32_t* p, std::size_t n, std::int32_t shift, std::int32_t lo, std::int32_t hi) noexcept;
#if defined(__AVX2__)
void rct_avx2(const std::int32_t* c0, const std::int32_t* c1, const std::int32_t* c2, std::int32_t* o0,
              std::int32_t* o1, std::int32_t* o2, std::size_t n) noexcept;
void ict_avx2(const std::int32_t* c0, const std::int32_t* c1, const std::int32_t* c2, std::int32_t* o0,
              std::int32_t* o1, std::int32_t* o2, std::size_t n) noexcept;
void dc_shift_53_avx2(std::int32_t* p, std::size_t n, std::int32_t shift, std::int32_t lo, std::int32_t hi) noexcept;
void dc_shift_97_avx2(std::int32_t* p, std::size_t n, std::int32_t shift, std::int32_t lo, std::int32_t hi) noexcept;
#endif
}  // namespace kernels

}  // namespace nj2k::detail

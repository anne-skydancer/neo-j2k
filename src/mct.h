// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The inverse colour transforms (RCT, ICT) and the DC level shift of a decoded tile, after
// OpenJPEG 2.5.4's mct.c (opj_mct_decode, opj_mct_decode_real) and tcd.c
// (opj_tcd_dc_level_shift_decode): the same operations per sample, so the samples are OpenJPEG's.
// Departure DP-2: the 9/7 shift decides the 2^31 edge the same way on every platform (OpenJPEG's
// MSVC build rounds 2^31 to INT_MIN, its Linux build to 2^31), and the 5/3 shift adds in 64 bits
// (OpenJPEG's int32 sum is undefined on overflow); both clamp as OpenJPEG does. The per-sample
// operations below are OpenJPEG's; its notice:
//
/*
 * The copyright in this software is being made available under the 2-clauses
 * BSD License, included below. This software may be subject to other third
 * party and contributor rights, including patent rights, and no such rights
 * are granted under this license.
 *
 * Copyright (c) 2002-2014, Universite catholique de Louvain (UCL), Belgium
 * Copyright (c) 2002-2014, Professor Benoit Macq
 * Copyright (c) 2001-2003, David Janssens
 * Copyright (c) 2002-2003, Yannick Verschueren
 * Copyright (c) 2003-2007, Francois-Olivier Devaux
 * Copyright (c) 2003-2014, Antonin Descampe
 * Copyright (c) 2005, Herve Drolon, FreeImage Team
 * Copyright (c) 2006-2007, Parvatha Elangovan
 * Copyright (c) 2008, 2011-2012, Centre National d'Etudes Spatiales (CNES), FR
 * Copyright (c) 2012, CS Systemes d'Information, France
 * Copyright (c) 2017, IntoPIX SA <support@intopix.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS `AS IS'
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#pragma once

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
#endif

namespace nj2k::detail {

// One sample of each operation, inline: the only copy of the per-sample arithmetic, which the
// scalar row kernels loop over and the output pass's tiny-region path (output.cpp) calls a pixel at
// a time. The AVX2 row kernels do the same per lane.
namespace sample {

// std::bit_cast, not memcpy: MSVC builds the memcpy of a value held in a register out of byte
// stores on the stack, and the float load that follows them stalls on store forwarding
inline float as_float(std::int32_t v) noexcept
{
    return std::bit_cast<float>(v);
}

inline std::int32_t as_int(float f) noexcept
{
    return std::bit_cast<std::int32_t>(f);
}

inline std::int32_t wrap_add(std::int32_t a, std::int32_t b) noexcept
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}

inline std::int32_t wrap_sub(std::int32_t a, std::int32_t b) noexcept
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
}

// opj_mct_decode on one sample, int32 wrap-around
inline void rct(std::int32_t& c0, std::int32_t& c1, std::int32_t& c2) noexcept
{
    const std::int32_t y = c0, u = c1, v = c2;
    const std::int32_t g = wrap_sub(y, wrap_add(u, v) >> 2);
    c0 = wrap_add(v, g);
    c1 = g;
    c2 = wrap_add(u, g);
}

// opj_mct_decode_real on one sample (float bits in int32)
inline void ict(std::int32_t& c0, std::int32_t& c1, std::int32_t& c2) noexcept
{
    const float y = as_float(c0), u = as_float(c1), v = as_float(c2);
    const float r = y + (v * 1.402f);
    const float g = y - (u * 0.34413f) - (v * (0.71414f));
    const float b = y + (u * 1.772f);
    c0 = as_int(r);
    c1 = as_int(g);
    c2 = as_int(b);
}

// The 5/3 DC shift of one sample: the add in 64 bits, clamped
inline std::int32_t shift_53(std::int32_t p, std::int32_t shift, std::int32_t lo, std::int32_t hi) noexcept
{
    const std::int64_t v = std::int64_t(p) + shift;
    return v < lo ? lo : (v > hi ? hi : static_cast<std::int32_t>(v));
}

// The 9/7 DC shift of one sample (float bits): OpenJPEG's l_value > (float)INT_MAX -> max, < INT_MIN
// -> min, else lrintf plus the shift in 64 bits, clamped; 2^31 itself gives max on every platform
// (DP-2), NaN gives min as on every OpenJPEG build
inline std::int32_t shift_97(std::int32_t bits, std::int32_t shift, std::int32_t lo, std::int32_t hi) noexcept
{
    const float v = as_float(bits);
    if (v >= 2147483648.0f) {
        return hi;
    }
    if (!(v >= -2147483648.0f)) {
        return lo;  // below INT_MIN, or NaN
    }
#if defined(_M_X64) || defined(__x86_64__)
    // v is in int32's range here, where cvtss2si rounds as nearbyint does (the current rounding
    // mode, to nearest with ties to even by default) and gives the same integer, without a call into
    // the C runtime (MSVC's nearbyintf is one)
    const std::int64_t r = static_cast<std::int64_t>(_mm_cvtss_si32(_mm_set_ss(v))) + shift;
#else
    const std::int64_t r = static_cast<std::int64_t>(std::nearbyint(v)) + shift;
#endif
    return r < lo ? lo : (r > hi ? hi : static_cast<std::int32_t>(r));
}

}  // namespace sample

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

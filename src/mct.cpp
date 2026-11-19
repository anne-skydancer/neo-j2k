// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The inverse colour transforms and the DC level shift, after OpenJPEG 2.5.4's mct.c
// (opj_mct_decode, opj_mct_decode_real) and tcd.c (opj_tcd_dc_level_shift_decode): the same
// operations per sample. OpenJPEG's notice:
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
#include "mct.h"

#include <cmath>
#include <cstring>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace nj2k::detail {

namespace {

inline float as_float(std::int32_t v) noexcept
{
    float f;
    std::memcpy(&f, &v, sizeof f);
    return f;
}

inline std::int32_t as_int(float f) noexcept
{
    std::int32_t v;
    std::memcpy(&v, &f, sizeof v);
    return v;
}

inline std::int32_t wrap_add(std::int32_t a, std::int32_t b) noexcept
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}

inline std::int32_t wrap_sub(std::int32_t a, std::int32_t b) noexcept
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
}

// One 9/7 sample: OpenJPEG's l_value > (float)INT_MAX -> max, < INT_MIN -> min, else lrintf plus the
// shift in 64 bits, clamped; 2^31 itself gives max on every platform (DP-2), NaN gives min as on
// every OpenJPEG build
inline std::int32_t shift_97_one(float v, std::int32_t shift, std::int32_t lo, std::int32_t hi) noexcept
{
    if (v >= 2147483648.0f) {
        return hi;
    }
    if (!(v >= -2147483648.0f)) {
        return lo;  // below INT_MIN, or NaN
    }
    const std::int64_t r = static_cast<std::int64_t>(std::nearbyint(v)) + shift;
    return r < lo ? lo : (r > hi ? hi : static_cast<std::int32_t>(r));
}

}  // namespace

namespace kernels {

void rct_scalar(std::int32_t* c0, std::int32_t* c1, std::int32_t* c2, std::size_t n) noexcept
{
    for (std::size_t i = 0; i < n; ++i) {
        const std::int32_t y = c0[i], u = c1[i], v = c2[i];
        const std::int32_t g = wrap_sub(y, wrap_add(u, v) >> 2);
        c0[i] = wrap_add(v, g);
        c1[i] = g;
        c2[i] = wrap_add(u, g);
    }
}

void ict_scalar(std::int32_t* c0, std::int32_t* c1, std::int32_t* c2, std::size_t n) noexcept
{
    for (std::size_t i = 0; i < n; ++i) {
        const float y = as_float(c0[i]), u = as_float(c1[i]), v = as_float(c2[i]);
        const float r = y + (v * 1.402f);
        const float g = y - (u * 0.34413f) - (v * (0.71414f));
        const float b = y + (u * 1.772f);
        c0[i] = as_int(r);
        c1[i] = as_int(g);
        c2[i] = as_int(b);
    }
}

void dc_shift_97_scalar(std::int32_t* p, std::size_t n, std::int32_t shift, std::int32_t lo, std::int32_t hi) noexcept
{
    for (std::size_t i = 0; i < n; ++i) {
        p[i] = shift_97_one(as_float(p[i]), shift, lo, hi);
    }
}

#if defined(__AVX2__)
void rct_avx2(std::int32_t* c0, std::int32_t* c1, std::int32_t* c2, std::size_t n) noexcept
{
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        const __m256i y = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(c0 + i));
        const __m256i u = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(c1 + i));
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(c2 + i));
        const __m256i g = _mm256_sub_epi32(y, _mm256_srai_epi32(_mm256_add_epi32(u, v), 2));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(c0 + i), _mm256_add_epi32(v, g));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(c1 + i), g);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(c2 + i), _mm256_add_epi32(u, g));
    }
    rct_scalar(c0 + i, c1 + i, c2 + i, n - i);
}

void ict_avx2(std::int32_t* c0, std::int32_t* c1, std::int32_t* c2, std::size_t n) noexcept
{
    const __m256 vrv = _mm256_set1_ps(1.402f), vgu = _mm256_set1_ps(0.34413f), vgv = _mm256_set1_ps(0.71414f),
                 vbu = _mm256_set1_ps(1.772f);
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        const __m256 y = _mm256_castsi256_ps(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(c0 + i)));
        const __m256 u = _mm256_castsi256_ps(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(c1 + i)));
        const __m256 v = _mm256_castsi256_ps(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(c2 + i)));
        const __m256 r = _mm256_add_ps(y, _mm256_mul_ps(v, vrv));
        const __m256 g = _mm256_sub_ps(_mm256_sub_ps(y, _mm256_mul_ps(u, vgu)), _mm256_mul_ps(v, vgv));
        const __m256 b = _mm256_add_ps(y, _mm256_mul_ps(u, vbu));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(c0 + i), _mm256_castps_si256(r));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(c1 + i), _mm256_castps_si256(g));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(c2 + i), _mm256_castps_si256(b));
    }
    ict_scalar(c0 + i, c1 + i, c2 + i, n - i);
}

// cvtps rounds to nearest, ties to even, and gives INT_MIN for anything out of range and NaN: those
// clamp to lo as they should, but for 2^31 and above, blended to hi
void dc_shift_97_avx2(std::int32_t* p, std::size_t n, std::int32_t shift, std::int32_t lo, std::int32_t hi) noexcept
{
    const __m256i vshift = _mm256_set1_epi32(shift);
    const __m256i vlo = _mm256_set1_epi32(static_cast<std::int32_t>(std::int64_t(lo) - shift));
    const __m256i vhi = _mm256_set1_epi32(static_cast<std::int32_t>(std::int64_t(hi) - shift));
    const __m256i vmax = _mm256_set1_epi32(hi);
    const __m256 two31 = _mm256_set1_ps(2147483648.0f);
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        const __m256 v = _mm256_castsi256_ps(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + i)));
        __m256i r = _mm256_cvtps_epi32(v);
        r = _mm256_min_epi32(_mm256_max_epi32(r, vlo), vhi);
        r = _mm256_add_epi32(r, vshift);
        const __m256i big = _mm256_castps_si256(_mm256_cmp_ps(v, two31, _CMP_GE_OQ));
        r = _mm256_blendv_epi8(r, vmax, big);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(p + i), r);
    }
    dc_shift_97_scalar(p + i, n - i, shift, lo, hi);
}
#endif

}  // namespace kernels

void mct_decode_rct(std::int32_t* c0, std::int32_t* c1, std::int32_t* c2, std::size_t n) noexcept
{
#if defined(__AVX2__)
    kernels::rct_avx2(c0, c1, c2, n);
#else
    kernels::rct_scalar(c0, c1, c2, n);
#endif
}

void mct_decode_ict(std::int32_t* c0, std::int32_t* c1, std::int32_t* c2, std::size_t n) noexcept
{
#if defined(__AVX2__)
    kernels::ict_avx2(c0, c1, c2, n);
#else
    kernels::ict_scalar(c0, c1, c2, n);
#endif
}

void dc_shift_53(std::int32_t* p, std::uint32_t w, std::uint32_t h, std::size_t stride, std::int32_t shift,
                 std::int32_t lo, std::int32_t hi) noexcept
{
    for (std::uint32_t j = 0; j < h; ++j, p += stride) {
        for (std::uint32_t i = 0; i < w; ++i) {
            const std::int64_t v = std::int64_t(p[i]) + shift;
            p[i] = v < lo ? lo : (v > hi ? hi : static_cast<std::int32_t>(v));
        }
    }
}

void dc_shift_97(std::int32_t* p, std::uint32_t w, std::uint32_t h, std::size_t stride, std::int32_t shift,
                 std::int32_t lo, std::int32_t hi) noexcept
{
    for (std::uint32_t j = 0; j < h; ++j, p += stride) {
#if defined(__AVX2__)
        kernels::dc_shift_97_avx2(p, w, shift, lo, hi);
#else
        kernels::dc_shift_97_scalar(p, w, shift, lo, hi);
#endif
    }
}

}  // namespace nj2k::detail

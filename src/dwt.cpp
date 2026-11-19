// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The inverse wavelet transforms of a whole tile-component, after OpenJPEG 2.5.4's dwt.c
// (opj_dwt_decode_tile, opj_idwt53_h, opj_idwt53_v and their kernels; opj_dwt_decode_tile_97,
// opj_v8dwt_interleave_h/_v, opj_v8dwt_decode and its steps): the same lifting steps, the same
// operation order per sample, the same special cases for one and two samples, and the same split
// of the 5/3 vertical pass into groups of 16 columns (OpenJPEG's AVX2 build) with the scalar
// formulas for the rest. Every int32 sum wraps (OpenJPEG's SIMD lanes do; its scalar code leaves
// overflow undefined). OpenJPEG's notice:
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
 * Copyright (c) 2007, Jonathan Ballard <dzonatas@dzonux.net>
 * Copyright (c) 2007, Callum Lerwick <seg@haxxed.com>
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
#include "dwt.h"

#include <cstring>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace nj2k::detail {

namespace {

// wrapping int32 arithmetic
inline std::int32_t add(std::int32_t a, std::int32_t b) noexcept
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}
inline std::int32_t sub(std::int32_t a, std::int32_t b) noexcept
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
}

constexpr std::int32_t kCols53 = 16;  // OpenJPEG's PARALLEL_COLS_53 in its AVX2 build
constexpr std::uint32_t kV8 = 8;      // NB_ELTS_V8

// From table F.4 of the standard, and OpenJPEG's BUG_WEIRD_TWO_INVK: the high band is scaled by
// 2 / invK, which tcd.c compensates with log2_gain = 0 for irreversible bands
constexpr float kAlpha = -1.586134342f;
constexpr float kBeta = -0.052980118f;
constexpr float kGamma = 0.882911075f;
constexpr float kDelta = 0.443506852f;
constexpr float kK = 1.230174105f;
constexpr float kTwoInvK = 1.625732422f;

std::uint32_t max_resolution(const ResRect* r, std::uint32_t i) noexcept
{
    std::uint32_t mr = 0, w;
    while (--i) {
        ++r;
        if (mr < (w = static_cast<std::uint32_t>(r->x1 - r->x0))) {
            mr = w;
        }
        if (mr < (w = static_cast<std::uint32_t>(r->y1 - r->y0))) {
            mr = w;
        }
    }
    return mr;
}

// ---- 5/3, horizontal --------------------------------------------------------------------------------

// opj_idwt53_h_cas0 (left-most sample on an even coordinate), len > 1
void h53_cas0_scalar(std::int32_t* tmp, std::int32_t sn, std::int32_t len, std::int32_t* tiledp) noexcept
{
    const std::int32_t* in_even = &tiledp[0];
    const std::int32_t* in_odd = &tiledp[sn];
    std::int32_t i, j, d1c, d1n, s1n, s0c, s0n;
    s1n = in_even[0];
    d1n = in_odd[0];
    s0n = sub(s1n, add(d1n, 1) >> 1);
    for (i = 0, j = 1; i < (len - 3); i += 2, j++) {
        d1c = d1n;
        s0c = s0n;
        s1n = in_even[j];
        d1n = in_odd[j];
        s0n = sub(s1n, add(add(d1c, d1n), 2) >> 2);
        tmp[i] = s0c;
        tmp[i + 1] = add(d1c, add(s0c, s0n) >> 1);
    }
    tmp[i] = s0n;
    if (len & 1) {
        tmp[len - 1] = sub(in_even[(len - 1) / 2], add(d1n, 1) >> 1);
        tmp[len - 2] = add(d1n, add(s0n, tmp[len - 1]) >> 1);
    } else {
        tmp[len - 1] = add(d1n, s0n);
    }
    std::memcpy(tiledp, tmp, static_cast<std::size_t>(len) * sizeof(std::int32_t));
}

// opj_idwt53_h_cas1 (left-most sample on an odd coordinate), len > 2
void h53_cas1(std::int32_t* tmp, std::int32_t sn, std::int32_t len, std::int32_t* tiledp) noexcept
{
    const std::int32_t* in_even = &tiledp[sn];
    const std::int32_t* in_odd = &tiledp[0];
    std::int32_t i, j, s1, s2, dc, dn;
    s1 = in_even[1];
    dc = sub(in_odd[0], add(add(in_even[0], s1), 2) >> 2);
    tmp[0] = add(in_even[0], dc);
    for (i = 1, j = 1; i < (len - 2 - !(len & 1)); i += 2, j++) {
        s2 = in_even[j + 1];
        dn = sub(in_odd[j], add(add(s1, s2), 2) >> 2);
        tmp[i] = dc;
        tmp[i + 1] = add(s1, add(dn, dc) >> 1);
        dc = dn;
        s1 = s2;
    }
    tmp[i] = dc;
    if (!(len & 1)) {
        dn = sub(in_odd[len / 2 - 1], add(s1, 1) >> 1);
        tmp[len - 2] = add(s1, add(dn, dc) >> 1);
        tmp[len - 1] = dn;
    } else {
        tmp[len - 1] = add(s1, dc);
    }
    std::memcpy(tiledp, tmp, static_cast<std::size_t>(len) * sizeof(std::int32_t));
}

#if defined(__AVX2__)
// OpenJPEG's AVX2 opj_idwt53_h_cas0: the same values as the scalar version
void h53_cas0_avx2(std::int32_t* tmp, std::int32_t sn, std::int32_t len, std::int32_t* tiledp) noexcept
{
    const std::int32_t* in_even = &tiledp[0];
    const std::int32_t* in_odd = &tiledp[sn];
    std::int32_t* out_ptr = tmp;
    std::int32_t prev_even = sub(in_even[0], add(in_odd[0], 1) >> 1);
    const __m256i move_right = _mm256_setr_epi32(0, 0, 1, 2, 3, 4, 5, 6);
    const __m256i two = _mm256_set1_epi32(2);
    const std::int32_t simd_batch = (len - 2) / 16;
    for (std::int32_t i = 0; i < simd_batch; i++) {
        const __m256i lf = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(in_even + 1));
        const __m256i hf1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(in_odd));
        const __m256i hf2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(in_odd + 1));
        __m256i even = _mm256_add_epi32(hf1, hf2);
        even = _mm256_add_epi32(even, two);
        even = _mm256_srai_epi32(even, 2);
        even = _mm256_sub_epi32(lf, even);
        const std::int32_t next_even = _mm_extract_epi32(_mm256_extracti128_si256(even, 1), 3);
        __m256i even_m1 = _mm256_permutevar8x32_epi32(even, move_right);
        even_m1 = _mm256_blend_epi32(even_m1, _mm256_set1_epi32(prev_even), (1 << 0));
        __m256i odd = _mm256_add_epi32(even_m1, even);
        odd = _mm256_srai_epi32(odd, 1);
        odd = _mm256_add_epi32(odd, hf1);
        const __m256i unpack1 = _mm256_unpacklo_epi32(even_m1, odd);
        const __m256i unpack2 = _mm256_unpackhi_epi32(even_m1, odd);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out_ptr + 0), _mm256_castsi256_si128(unpack1));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out_ptr + 4), _mm256_castsi256_si128(unpack2));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out_ptr + 8), _mm256_extracti128_si256(unpack1, 0x1));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out_ptr + 12), _mm256_extracti128_si256(unpack2, 0x1));
        prev_even = next_even;
        out_ptr += 16;
        in_even += 8;
        in_odd += 8;
    }
    out_ptr[0] = prev_even;
    for (std::int32_t j = simd_batch * 16 + 1; j < (len - 2); j += 2) {
        out_ptr[2] = sub(in_even[1], add(add(in_odd[0], in_odd[1]), 2) >> 2);
        out_ptr[1] = add(in_odd[0], add(out_ptr[0], out_ptr[2]) >> 1);
        in_even++;
        in_odd++;
        out_ptr += 2;
    }
    if (len & 1) {
        out_ptr[2] = sub(in_even[1], add(in_odd[0], 1) >> 1);
        out_ptr[1] = add(in_odd[0], add(out_ptr[0], out_ptr[2]) >> 1);
    } else {
        out_ptr[1] = add(in_odd[0], out_ptr[0]);
    }
    std::memcpy(tiledp, tmp, static_cast<std::size_t>(len) * sizeof(std::int32_t));
}
#endif

// opj_idwt53_h
template <bool kAvx2>
void h53(std::int32_t* row, std::int32_t sn, std::int32_t dn, std::int32_t cas, std::int32_t* tmp) noexcept
{
    const std::int32_t len = sn + dn;
    if (cas == 0) {
        if (len > 1) {
#if defined(__AVX2__)
            if constexpr (kAvx2) {
                h53_cas0_avx2(tmp, sn, len, row);
            } else {
                h53_cas0_scalar(tmp, sn, len, row);
            }
#else
            h53_cas0_scalar(tmp, sn, len, row);
#endif
        }
    } else {
        if (len == 1) {
            row[0] /= 2;
        } else if (len == 2) {
            const std::int32_t* in_even = &row[sn];
            const std::int32_t* in_odd = &row[0];
            tmp[1] = sub(in_odd[0], add(in_even[0], 1) >> 1);
            tmp[0] = add(in_even[0], tmp[1]);
            row[0] = tmp[0];
            row[1] = tmp[1];
        } else if (len > 2) {
            h53_cas1(tmp, sn, len, row);
        }
    }
}

// ---- 5/3, vertical ----------------------------------------------------------------------------------

// opj_idwt3_v_cas0: one column, OpenJPEG's scalar formulas
void v53_cas0_column(std::int32_t* tmp, std::int32_t sn, std::int32_t len, std::int32_t* col,
                     std::size_t stride) noexcept
{
    std::int32_t i, j, d1c, d1n, s1n, s0c, s0n;
    s1n = col[0];
    d1n = col[static_cast<std::size_t>(sn) * stride];
    s0n = sub(s1n, add(d1n, 1) >> 1);
    for (i = 0, j = 0; i < (len - 3); i += 2, j++) {
        d1c = d1n;
        s0c = s0n;
        s1n = col[static_cast<std::size_t>(j + 1) * stride];
        d1n = col[static_cast<std::size_t>(sn + j + 1) * stride];
        s0n = sub(s1n, add(add(d1c, d1n), 2) >> 2);
        tmp[i] = s0c;
        tmp[i + 1] = add(d1c, add(s0c, s0n) >> 1);
    }
    tmp[i] = s0n;
    if (len & 1) {
        tmp[len - 1] = sub(col[static_cast<std::size_t>((len - 1) / 2) * stride], add(d1n, 1) >> 1);
        tmp[len - 2] = add(d1n, add(s0n, tmp[len - 1]) >> 1);
    } else {
        tmp[len - 1] = add(d1n, s0n);
    }
    for (i = 0; i < len; ++i) {
        col[static_cast<std::size_t>(i) * stride] = tmp[i];
    }
}

// opj_idwt3_v_cas1: one column, OpenJPEG's scalar formulas
void v53_cas1_column(std::int32_t* tmp, std::int32_t sn, std::int32_t len, std::int32_t* col,
                     std::size_t stride) noexcept
{
    std::int32_t i, j, s1, s2, dc, dn;
    const std::int32_t* in_even = &col[static_cast<std::size_t>(sn) * stride];
    const std::int32_t* in_odd = &col[0];
    s1 = in_even[stride];
    dc = sub(in_odd[0], add(add(in_even[0], s1), 2) >> 2);
    tmp[0] = add(in_even[0], dc);
    for (i = 1, j = 1; i < (len - 2 - !(len & 1)); i += 2, j++) {
        s2 = in_even[static_cast<std::size_t>(j + 1) * stride];
        dn = sub(in_odd[static_cast<std::size_t>(j) * stride], add(add(s1, s2), 2) >> 2);
        tmp[i] = dc;
        tmp[i + 1] = add(s1, add(dn, dc) >> 1);
        dc = dn;
        s1 = s2;
    }
    tmp[i] = dc;
    if (!(len & 1)) {
        dn = sub(in_odd[static_cast<std::size_t>(len / 2 - 1) * stride], add(s1, 1) >> 1);
        tmp[len - 2] = add(s1, add(dn, dc) >> 1);
        tmp[len - 1] = dn;
    } else {
        tmp[len - 1] = add(s1, dc);
    }
    for (i = 0; i < len; ++i) {
        col[static_cast<std::size_t>(i) * stride] = tmp[i];
    }
}

// opj_idwt53_v_cas0_mcols_SIMD's formulas on one column ((d + d + 2) >> 2 where the scalar code has
// (d + 1) >> 1), so a group of 16 columns gives what OpenJPEG's AVX2 build gives
void v53_cas0_column_simd_formulas(std::int32_t* tmp, std::int32_t sn, std::int32_t len, std::int32_t* col,
                                   std::size_t stride) noexcept
{
    const std::int32_t* in_even = &col[0];
    const std::int32_t* in_odd = &col[static_cast<std::size_t>(sn) * stride];
    std::int32_t i;
    std::size_t j;
    std::int32_t d1c, d1n, s1n, s0c, s0n;
    s1n = in_even[0];
    d1n = in_odd[0];
    s0n = sub(s1n, add(add(d1n, d1n), 2) >> 2);
    for (i = 0, j = 1; i < (len - 3); i += 2, j++) {
        d1c = d1n;
        s0c = s0n;
        s1n = in_even[j * stride];
        d1n = in_odd[j * stride];
        s0n = sub(s1n, add(add(d1c, d1n), 2) >> 2);
        tmp[i] = s0c;
        tmp[i + 1] = add(d1c, add(s0c, s0n) >> 1);
    }
    tmp[i] = s0n;
    if (len & 1) {
        s1n = in_even[static_cast<std::size_t>((len - 1) / 2) * stride];
        const std::int32_t last = sub(s1n, add(add(d1n, d1n), 2) >> 2);
        tmp[len - 1] = last;
        tmp[len - 2] = add(d1n, add(s0n, last) >> 1);
    } else {
        tmp[len - 1] = add(d1n, s0n);
    }
    for (i = 0; i < len; ++i) {
        col[static_cast<std::size_t>(i) * stride] = tmp[i];
    }
}

// opj_idwt53_v_cas1_mcols_SIMD's formulas on one column ((s + s + 2) >> 2 for the last one)
void v53_cas1_column_simd_formulas(std::int32_t* tmp, std::int32_t sn, std::int32_t len, std::int32_t* col,
                                   std::size_t stride) noexcept
{
    std::int32_t i;
    std::size_t j;
    std::int32_t s1, s2, dc, dn;
    const std::int32_t* in_even = &col[static_cast<std::size_t>(sn) * stride];
    const std::int32_t* in_odd = &col[0];
    s1 = in_even[stride];
    dc = sub(in_odd[0], add(add(in_even[0], s1), 2) >> 2);
    tmp[0] = add(in_even[0], dc);
    for (i = 1, j = 1; i < (len - 2 - !(len & 1)); i += 2, j++) {
        s2 = in_even[(j + 1) * stride];
        dn = sub(in_odd[j * stride], add(add(s1, s2), 2) >> 2);
        tmp[i] = dc;
        tmp[i + 1] = add(s1, add(dn, dc) >> 1);
        dc = dn;
        s1 = s2;
    }
    tmp[i] = dc;
    if (!(len & 1)) {
        dn = sub(in_odd[static_cast<std::size_t>(len / 2 - 1) * stride], add(add(s1, s1), 2) >> 2);
        tmp[len - 2] = add(s1, add(dn, dc) >> 1);
        tmp[len - 1] = dn;
    } else {
        tmp[len - 1] = add(s1, dc);
    }
    for (i = 0; i < len; ++i) {
        col[static_cast<std::size_t>(i) * stride] = tmp[i];
    }
}

#if defined(__AVX2__)
#define NJ2K_V53_LOADU(x) _mm256_loadu_si256(reinterpret_cast<const __m256i*>(x))
#define NJ2K_V53_STORE(x, y) _mm256_store_si256(reinterpret_cast<__m256i*>(x), (y))
#define NJ2K_V53_ADD(x, y) _mm256_add_epi32((x), (y))
#define NJ2K_V53_SUB(x, y) _mm256_sub_epi32((x), (y))
#define NJ2K_V53_SAR(x, y) _mm256_srai_epi32((x), (y))
#define NJ2K_V53_ADD3(x, y, z) NJ2K_V53_ADD(NJ2K_V53_ADD(x, y), z)

// opj_idwt53_v_final_memcpy
void v53_final_copy(std::int32_t* col, const std::int32_t* tmp, std::int32_t len, std::size_t stride) noexcept
{
    for (std::int32_t i = 0; i < len; ++i) {
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(&col[static_cast<std::size_t>(i) * stride + 0]),
                            _mm256_load_si256(reinterpret_cast<const __m256i*>(&tmp[kCols53 * i + 0])));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(&col[static_cast<std::size_t>(i) * stride + 8]),
                            _mm256_load_si256(reinterpret_cast<const __m256i*>(&tmp[kCols53 * i + 8])));
    }
}

// opj_idwt53_v_cas0_mcols_SIMD (AVX2: 16 columns)
void v53_cas0_mcols_avx2(std::int32_t* tmp, std::int32_t sn, std::int32_t len, std::int32_t* col,
                         std::size_t stride) noexcept
{
    const std::int32_t* in_even = &col[0];
    const std::int32_t* in_odd = &col[static_cast<std::size_t>(sn) * stride];
    std::int32_t i;
    std::size_t j;
    __m256i d1c_0, d1n_0, s1n_0, s0c_0, s0n_0;
    __m256i d1c_1, d1n_1, s1n_1, s0c_1, s0n_1;
    const __m256i two = _mm256_set1_epi32(2);
    s1n_0 = NJ2K_V53_LOADU(in_even + 0);
    s1n_1 = NJ2K_V53_LOADU(in_even + 8);
    d1n_0 = NJ2K_V53_LOADU(in_odd);
    d1n_1 = NJ2K_V53_LOADU(in_odd + 8);
    s0n_0 = NJ2K_V53_SUB(s1n_0, NJ2K_V53_SAR(NJ2K_V53_ADD3(d1n_0, d1n_0, two), 2));
    s0n_1 = NJ2K_V53_SUB(s1n_1, NJ2K_V53_SAR(NJ2K_V53_ADD3(d1n_1, d1n_1, two), 2));
    for (i = 0, j = 1; i < (len - 3); i += 2, j++) {
        d1c_0 = d1n_0;
        s0c_0 = s0n_0;
        d1c_1 = d1n_1;
        s0c_1 = s0n_1;
        s1n_0 = NJ2K_V53_LOADU(in_even + j * stride);
        s1n_1 = NJ2K_V53_LOADU(in_even + j * stride + 8);
        d1n_0 = NJ2K_V53_LOADU(in_odd + j * stride);
        d1n_1 = NJ2K_V53_LOADU(in_odd + j * stride + 8);
        s0n_0 = NJ2K_V53_SUB(s1n_0, NJ2K_V53_SAR(NJ2K_V53_ADD3(d1c_0, d1n_0, two), 2));
        s0n_1 = NJ2K_V53_SUB(s1n_1, NJ2K_V53_SAR(NJ2K_V53_ADD3(d1c_1, d1n_1, two), 2));
        NJ2K_V53_STORE(tmp + kCols53 * (i + 0), s0c_0);
        NJ2K_V53_STORE(tmp + kCols53 * (i + 0) + 8, s0c_1);
        NJ2K_V53_STORE(tmp + kCols53 * (i + 1) + 0, NJ2K_V53_ADD(d1c_0, NJ2K_V53_SAR(NJ2K_V53_ADD(s0c_0, s0n_0), 1)));
        NJ2K_V53_STORE(tmp + kCols53 * (i + 1) + 8, NJ2K_V53_ADD(d1c_1, NJ2K_V53_SAR(NJ2K_V53_ADD(s0c_1, s0n_1), 1)));
    }
    NJ2K_V53_STORE(tmp + kCols53 * (i + 0) + 0, s0n_0);
    NJ2K_V53_STORE(tmp + kCols53 * (i + 0) + 8, s0n_1);
    if (len & 1) {
        __m256i last;
        s1n_0 = NJ2K_V53_LOADU(in_even + static_cast<std::size_t>((len - 1) / 2) * stride);
        last = NJ2K_V53_SUB(s1n_0, NJ2K_V53_SAR(NJ2K_V53_ADD3(d1n_0, d1n_0, two), 2));
        NJ2K_V53_STORE(tmp + kCols53 * (len - 1), last);
        NJ2K_V53_STORE(tmp + kCols53 * (len - 2), NJ2K_V53_ADD(d1n_0, NJ2K_V53_SAR(NJ2K_V53_ADD(s0n_0, last), 1)));
        s1n_1 = NJ2K_V53_LOADU(in_even + static_cast<std::size_t>((len - 1) / 2) * stride + 8);
        last = NJ2K_V53_SUB(s1n_1, NJ2K_V53_SAR(NJ2K_V53_ADD3(d1n_1, d1n_1, two), 2));
        NJ2K_V53_STORE(tmp + kCols53 * (len - 1) + 8, last);
        NJ2K_V53_STORE(tmp + kCols53 * (len - 2) + 8, NJ2K_V53_ADD(d1n_1, NJ2K_V53_SAR(NJ2K_V53_ADD(s0n_1, last), 1)));
    } else {
        NJ2K_V53_STORE(tmp + kCols53 * (len - 1) + 0, NJ2K_V53_ADD(d1n_0, s0n_0));
        NJ2K_V53_STORE(tmp + kCols53 * (len - 1) + 8, NJ2K_V53_ADD(d1n_1, s0n_1));
    }
    v53_final_copy(col, tmp, len, stride);
}

// opj_idwt53_v_cas1_mcols_SIMD (AVX2: 16 columns)
void v53_cas1_mcols_avx2(std::int32_t* tmp, std::int32_t sn, std::int32_t len, std::int32_t* col,
                         std::size_t stride) noexcept
{
    std::int32_t i;
    std::size_t j;
    __m256i s1_0, s2_0, dc_0, dn_0;
    __m256i s1_1, s2_1, dc_1, dn_1;
    const __m256i two = _mm256_set1_epi32(2);
    const std::int32_t* in_even = &col[static_cast<std::size_t>(sn) * stride];
    const std::int32_t* in_odd = &col[0];
    s1_0 = NJ2K_V53_LOADU(in_even + stride);
    dc_0 = NJ2K_V53_SUB(NJ2K_V53_LOADU(in_odd + 0), NJ2K_V53_SAR(NJ2K_V53_ADD3(NJ2K_V53_LOADU(in_even + 0), s1_0, two), 2));
    NJ2K_V53_STORE(tmp + kCols53 * 0, NJ2K_V53_ADD(NJ2K_V53_LOADU(in_even + 0), dc_0));
    s1_1 = NJ2K_V53_LOADU(in_even + stride + 8);
    dc_1 = NJ2K_V53_SUB(NJ2K_V53_LOADU(in_odd + 8), NJ2K_V53_SAR(NJ2K_V53_ADD3(NJ2K_V53_LOADU(in_even + 8), s1_1, two), 2));
    NJ2K_V53_STORE(tmp + kCols53 * 0 + 8, NJ2K_V53_ADD(NJ2K_V53_LOADU(in_even + 8), dc_1));
    for (i = 1, j = 1; i < (len - 2 - !(len & 1)); i += 2, j++) {
        s2_0 = NJ2K_V53_LOADU(in_even + (j + 1) * stride);
        s2_1 = NJ2K_V53_LOADU(in_even + (j + 1) * stride + 8);
        dn_0 = NJ2K_V53_SUB(NJ2K_V53_LOADU(in_odd + j * stride), NJ2K_V53_SAR(NJ2K_V53_ADD3(s1_0, s2_0, two), 2));
        dn_1 = NJ2K_V53_SUB(NJ2K_V53_LOADU(in_odd + j * stride + 8), NJ2K_V53_SAR(NJ2K_V53_ADD3(s1_1, s2_1, two), 2));
        NJ2K_V53_STORE(tmp + kCols53 * i, dc_0);
        NJ2K_V53_STORE(tmp + kCols53 * i + 8, dc_1);
        NJ2K_V53_STORE(tmp + kCols53 * (i + 1) + 0, NJ2K_V53_ADD(s1_0, NJ2K_V53_SAR(NJ2K_V53_ADD(dn_0, dc_0), 1)));
        NJ2K_V53_STORE(tmp + kCols53 * (i + 1) + 8, NJ2K_V53_ADD(s1_1, NJ2K_V53_SAR(NJ2K_V53_ADD(dn_1, dc_1), 1)));
        dc_0 = dn_0;
        s1_0 = s2_0;
        dc_1 = dn_1;
        s1_1 = s2_1;
    }
    NJ2K_V53_STORE(tmp + kCols53 * i, dc_0);
    NJ2K_V53_STORE(tmp + kCols53 * i + 8, dc_1);
    if (!(len & 1)) {
        dn_0 = NJ2K_V53_SUB(NJ2K_V53_LOADU(in_odd + static_cast<std::size_t>(len / 2 - 1) * stride),
                            NJ2K_V53_SAR(NJ2K_V53_ADD3(s1_0, s1_0, two), 2));
        dn_1 = NJ2K_V53_SUB(NJ2K_V53_LOADU(in_odd + static_cast<std::size_t>(len / 2 - 1) * stride + 8),
                            NJ2K_V53_SAR(NJ2K_V53_ADD3(s1_1, s1_1, two), 2));
        NJ2K_V53_STORE(tmp + kCols53 * (len - 2) + 0, NJ2K_V53_ADD(s1_0, NJ2K_V53_SAR(NJ2K_V53_ADD(dn_0, dc_0), 1)));
        NJ2K_V53_STORE(tmp + kCols53 * (len - 2) + 8, NJ2K_V53_ADD(s1_1, NJ2K_V53_SAR(NJ2K_V53_ADD(dn_1, dc_1), 1)));
        NJ2K_V53_STORE(tmp + kCols53 * (len - 1) + 0, dn_0);
        NJ2K_V53_STORE(tmp + kCols53 * (len - 1) + 8, dn_1);
    } else {
        NJ2K_V53_STORE(tmp + kCols53 * (len - 1) + 0, NJ2K_V53_ADD(s1_0, dc_0));
        NJ2K_V53_STORE(tmp + kCols53 * (len - 1) + 8, NJ2K_V53_ADD(s1_1, dc_1));
    }
    v53_final_copy(col, tmp, len, stride);
}

#undef NJ2K_V53_LOADU
#undef NJ2K_V53_STORE
#undef NJ2K_V53_ADD
#undef NJ2K_V53_SUB
#undef NJ2K_V53_SAR
#undef NJ2K_V53_ADD3
#endif

// opj_idwt53_v: cols columns from col; a full group of 16 takes OpenJPEG's SIMD formulas
template <bool kAvx2>
void v53(std::int32_t* col, std::size_t stride, std::int32_t sn, std::int32_t dn, std::int32_t cas,
         std::int32_t cols, std::int32_t* tmp) noexcept
{
    const std::int32_t len = sn + dn;
    if (cas == 0) {
        if (len > 1 && cols == kCols53) {
#if defined(__AVX2__)
            if constexpr (kAvx2) {
                v53_cas0_mcols_avx2(tmp, sn, len, col, stride);
            } else {
                for (std::int32_t c = 0; c < cols; c++) {
                    v53_cas0_column_simd_formulas(tmp, sn, len, col + c, stride);
                }
            }
#else
            for (std::int32_t c = 0; c < cols; c++) {
                v53_cas0_column_simd_formulas(tmp, sn, len, col + c, stride);
            }
#endif
            return;
        }
        if (len > 1) {
            for (std::int32_t c = 0; c < cols; c++) {
                v53_cas0_column(tmp, sn, len, col + c, stride);
            }
        }
        return;
    }
    if (len == 1) {
        for (std::int32_t c = 0; c < cols; c++) {
            col[c] /= 2;
        }
        return;
    }
    if (len == 2) {
        for (std::int32_t c = 0; c < cols; c++) {
            std::int32_t* p = col + c;
            const std::int32_t even = p[static_cast<std::size_t>(sn) * stride];
            const std::int32_t odd = p[0];
            const std::int32_t o1 = sub(odd, add(even, 1) >> 1);
            const std::int32_t o0 = add(even, o1);
            p[0] = o0;
            p[stride] = o1;
        }
        return;
    }
    if (len > 2 && cols == kCols53) {
#if defined(__AVX2__)
        if constexpr (kAvx2) {
            v53_cas1_mcols_avx2(tmp, sn, len, col, stride);
        } else {
            for (std::int32_t c = 0; c < cols; c++) {
                v53_cas1_column_simd_formulas(tmp, sn, len, col + c, stride);
            }
        }
#else
        for (std::int32_t c = 0; c < cols; c++) {
            v53_cas1_column_simd_formulas(tmp, sn, len, col + c, stride);
        }
#endif
        return;
    }
    if (len > 2) {
        for (std::int32_t c = 0; c < cols; c++) {
            v53_cas1_column(tmp, sn, len, col + c, stride);
        }
    }
}

// opj_dwt_decode_tile
void decode_53(std::int32_t* data, std::uint32_t w, const ResRect* tr, std::uint32_t numres,
               std::int32_t* mem) noexcept
{
    constexpr bool kAvx2 =
#if defined(__AVX2__)
        true;
#else
        false;
#endif
    std::uint32_t rw = static_cast<std::uint32_t>(tr->x1 - tr->x0);
    std::uint32_t rh = static_cast<std::uint32_t>(tr->y1 - tr->y0);
    if (numres == 1U || w == 0) {
        return;
    }
    while (--numres) {
        ++tr;
        const std::int32_t hsn = static_cast<std::int32_t>(rw);
        const std::int32_t vsn = static_cast<std::int32_t>(rh);
        rw = static_cast<std::uint32_t>(tr->x1 - tr->x0);
        rh = static_cast<std::uint32_t>(tr->y1 - tr->y0);
        const std::int32_t hdn = static_cast<std::int32_t>(rw - static_cast<std::uint32_t>(hsn));
        const std::int32_t hcas = tr->x0 % 2;
        for (std::uint32_t j = 0; j < rh; ++j) {
            h53<kAvx2>(&data[static_cast<std::size_t>(j) * w], hsn, hdn, hcas, mem);
        }
        const std::int32_t vdn = static_cast<std::int32_t>(rh - static_cast<std::uint32_t>(vsn));
        const std::int32_t vcas = tr->y0 % 2;
        std::uint32_t j = 0;
        for (; j + kCols53 <= rw; j += kCols53) {
            v53<kAvx2>(&data[j], w, vsn, vdn, vcas, kCols53, mem);
        }
        if (j < rw) {
            v53<kAvx2>(&data[j], w, vsn, vdn, vcas, static_cast<std::int32_t>(rw - j), mem);
        }
    }
}

// ---- 9/7 --------------------------------------------------------------------------------------------

// opj_v8dwt_t: sn low then dn high samples, interleaved by cas; 8 floats per sample
struct V8 {
    float* wavelet;
    std::int32_t dn, sn, cas;
};

inline float* elem(float* w, std::int64_t i) noexcept
{
    return w + i * static_cast<std::int64_t>(kV8);
}

// opj_v8dwt_interleave_h, the window being the whole band
void v8_interleave_h(const V8& dwt, const float* a, std::uint32_t width, std::uint32_t remaining_height) noexcept
{
    float* bi = elem(dwt.wavelet, dwt.cas);
    std::uint32_t x1 = static_cast<std::uint32_t>(dwt.sn);
    for (std::uint32_t k = 0; k < 2; ++k) {
        for (std::uint32_t i = 0; i < x1; ++i) {
            std::uint32_t j = i;
            float* dst = bi + std::size_t(i) * 2 * kV8;
            const std::uint32_t rows = remaining_height < kV8 ? remaining_height : kV8;
            for (std::uint32_t r = 0; r < rows; ++r, j += width) {
                dst[r] = a[j];
            }
        }
        bi = elem(dwt.wavelet, 1 - dwt.cas);
        a += dwt.sn;
        x1 = static_cast<std::uint32_t>(dwt.dn);
    }
}

// opj_v8dwt_interleave_v
void v8_interleave_v(const V8& dwt, const float* a, std::uint32_t width, std::uint32_t nb_elts_read) noexcept
{
    float* bi = elem(dwt.wavelet, dwt.cas);
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(dwt.sn); ++i) {
        std::memcpy(bi + std::size_t(i) * 2 * kV8, &a[i * static_cast<std::size_t>(width)],
                    static_cast<std::size_t>(nb_elts_read) * sizeof(float));
    }
    a += static_cast<std::size_t>(static_cast<std::uint32_t>(dwt.sn)) * width;
    bi = elem(dwt.wavelet, 1 - dwt.cas);
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(dwt.dn); ++i) {
        std::memcpy(bi + std::size_t(i) * 2 * kV8, &a[i * static_cast<std::size_t>(width)],
                    static_cast<std::size_t>(nb_elts_read) * sizeof(float));
    }
}

// opj_v8dwt_decode_step1 / _step2, scalar: the per-sample operations of the SSE version
void v8_step1_scalar(float* w, std::uint32_t start, std::uint32_t end, float c) noexcept
{
    for (std::uint32_t i = start; i < end; ++i) {
        float* p = w + std::size_t(i) * 2 * kV8;
        for (std::uint32_t k = 0; k < kV8; ++k) {
            p[k] = p[k] * c;
        }
    }
}

void v8_step2_scalar(float* l, float* w, std::uint32_t start, std::uint32_t end, std::uint32_t m, float c) noexcept
{
    // the sample before fw and the one before that, as pointers: kV8 is unsigned, so an index
    // k - kV8 would wrap to a huge offset instead of going back
    const std::uint32_t imax = end < m ? end : m;
    float* fw = w;
    if (start == 0) {
        if (imax >= 1) {
            float* prev = fw - kV8;
            for (std::uint32_t k = 0; k < kV8; ++k) {
                prev[k] = prev[k] + ((l[k] + fw[k]) * c);
            }
            fw += 2 * kV8;
            start = 1;
        }
    } else {
        fw += std::size_t(start) * 2 * kV8;
    }
    for (std::uint32_t i = start; i < imax; ++i) {
        float* prev = fw - kV8;
        const float* prev2 = fw - 2 * kV8;
        for (std::uint32_t k = 0; k < kV8; ++k) {
            prev[k] = prev[k] + ((prev2[k] + fw[k]) * c);
        }
        fw += 2 * kV8;
    }
    if (m < end) {
        c = c + c;
        float* prev = fw - kV8;
        const float* prev2 = fw - 2 * kV8;
        for (std::uint32_t k = 0; k < kV8; ++k) {
            prev[k] = prev[k] + (c * prev2[k]);
        }
    }
}

#if defined(__AVX2__)
// The same with one 8-float register per sample (OpenJPEG's SSE code uses two 4-float ones; each
// lane does the same operations)
void v8_step1_avx2(float* w, std::uint32_t start, std::uint32_t end, float c) noexcept
{
    const __m256 vc = _mm256_set1_ps(c);
    for (std::uint32_t i = start; i < end; ++i) {
        float* p = w + std::size_t(i) * 2 * kV8;
        _mm256_storeu_ps(p, _mm256_mul_ps(_mm256_loadu_ps(p), vc));
    }
}

void v8_step2_avx2(float* l, float* w, std::uint32_t start, std::uint32_t end, std::uint32_t m, float c) noexcept
{
    __m256 vc = _mm256_set1_ps(c);
    const std::uint32_t imax = end < m ? end : m;
    float* fw = w;
    if (start == 0) {
        if (imax >= 1) {
            const __m256 sum = _mm256_add_ps(_mm256_loadu_ps(l), _mm256_loadu_ps(fw));
            _mm256_storeu_ps(fw - kV8, _mm256_add_ps(_mm256_loadu_ps(fw - kV8), _mm256_mul_ps(sum, vc)));
            fw += 2 * kV8;
            start = 1;
        }
    } else {
        fw += std::size_t(start) * 2 * kV8;
    }
    for (std::uint32_t i = start; i < imax; ++i) {
        const __m256 sum = _mm256_add_ps(_mm256_loadu_ps(fw - 2 * kV8), _mm256_loadu_ps(fw));
        _mm256_storeu_ps(fw - kV8, _mm256_add_ps(_mm256_loadu_ps(fw - kV8), _mm256_mul_ps(sum, vc)));
        fw += 2 * kV8;
    }
    if (m < end) {
        vc = _mm256_add_ps(vc, vc);
        _mm256_storeu_ps(fw - kV8,
                         _mm256_add_ps(_mm256_loadu_ps(fw - kV8), _mm256_mul_ps(vc, _mm256_loadu_ps(fw - 2 * kV8))));
    }
}
#endif

inline std::int32_t imin(std::int32_t a, std::int32_t b) noexcept
{
    return a < b ? a : b;
}

// opj_v8dwt_decode
template <bool kAvx2>
void v8_decode(const V8& dwt) noexcept
{
    std::int32_t a, b;
    if (dwt.cas == 0) {
        if (!((dwt.dn > 0) || (dwt.sn > 1))) {
            return;
        }
        a = 0;
        b = 1;
    } else {
        if (!((dwt.sn > 0) || (dwt.dn > 1))) {
            return;
        }
        a = 1;
        b = 0;
    }
    const std::uint32_t sn = static_cast<std::uint32_t>(dwt.sn), dn = static_cast<std::uint32_t>(dwt.dn);
    float* wa = elem(dwt.wavelet, a);
    float* wb = elem(dwt.wavelet, b);
    float* wa1 = elem(dwt.wavelet, a + 1);
    float* wb1 = elem(dwt.wavelet, b + 1);
    const std::uint32_t ml = static_cast<std::uint32_t>(imin(dwt.sn, dwt.dn - a));
    const std::uint32_t mh = static_cast<std::uint32_t>(imin(dwt.dn, dwt.sn - b));
#if defined(__AVX2__)
    if constexpr (kAvx2) {
        v8_step1_avx2(wa, 0, sn, kK);
        v8_step1_avx2(wb, 0, dn, kTwoInvK);
        v8_step2_avx2(wb, wa1, 0, sn, ml, -kDelta);
        v8_step2_avx2(wa, wb1, 0, dn, mh, -kGamma);
        v8_step2_avx2(wb, wa1, 0, sn, ml, -kBeta);
        v8_step2_avx2(wa, wb1, 0, dn, mh, -kAlpha);
        return;
    } else
#endif
    {
        v8_step1_scalar(wa, 0, sn, kK);
        v8_step1_scalar(wb, 0, dn, kTwoInvK);
        v8_step2_scalar(wb, wa1, 0, sn, ml, -kDelta);
        v8_step2_scalar(wa, wb1, 0, dn, mh, -kGamma);
        v8_step2_scalar(wb, wa1, 0, sn, ml, -kBeta);
        v8_step2_scalar(wa, wb1, 0, dn, mh, -kAlpha);
    }
}

// opj_dwt_decode_tile_97, one thread
void decode_97(std::int32_t* data, std::uint32_t w, const ResRect* res, std::uint32_t numres, float* wavelet) noexcept
{
    constexpr bool kAvx2 =
#if defined(__AVX2__)
        true;
#else
        false;
#endif
    std::uint32_t rw = static_cast<std::uint32_t>(res->x1 - res->x0);
    std::uint32_t rh = static_cast<std::uint32_t>(res->y1 - res->y0);
    if (numres == 1 || w == 0) {
        return;
    }
    V8 h{wavelet, 0, 0, 0};
    V8 v{wavelet, 0, 0, 0};
    while (--numres) {
        // the samples hold float bits
        float* aj = reinterpret_cast<float*>(data);
        h.sn = static_cast<std::int32_t>(rw);
        v.sn = static_cast<std::int32_t>(rh);
        ++res;
        rw = static_cast<std::uint32_t>(res->x1 - res->x0);
        rh = static_cast<std::uint32_t>(res->y1 - res->y0);
        h.dn = static_cast<std::int32_t>(rw - static_cast<std::uint32_t>(h.sn));
        h.cas = res->x0 % 2;
        std::uint32_t j = 0;
        for (; j + (kV8 - 1) < rh; j += kV8) {
            v8_interleave_h(h, aj, w, kV8);
            v8_decode<kAvx2>(h);
            // back to the eight rows four at a time, as OpenJPEG writes them: fewer store streams
            // at once on rows whose starts alias in the cache
            for (std::uint32_t l0 = 0; l0 < kV8; l0 += 4) {
                for (std::uint32_t k = 0; k < rw; k++) {
                    const float* f = h.wavelet + std::size_t(k) * kV8;
                    for (std::uint32_t l = l0; l < l0 + 4; ++l) {
                        aj[k + static_cast<std::size_t>(w) * l] = f[l];
                    }
                }
            }
            aj += static_cast<std::size_t>(w) * kV8;
        }
        if (j < rh) {
            v8_interleave_h(h, aj, w, rh - j);
            v8_decode<kAvx2>(h);
            for (std::uint32_t k = 0; k < rw; k++) {
                const float* f = h.wavelet + std::size_t(k) * kV8;
                for (std::uint32_t l = 0; l < rh - j; l++) {
                    aj[k + static_cast<std::size_t>(w) * l] = f[l];
                }
            }
        }
        v.dn = static_cast<std::int32_t>(rh - static_cast<std::uint32_t>(v.sn));
        v.cas = res->y0 % 2;
        aj = reinterpret_cast<float*>(data);
        for (j = rw; j > (kV8 - 1); j -= kV8) {
            v8_interleave_v(v, aj, w, kV8);
            v8_decode<kAvx2>(v);
            for (std::uint32_t k = 0; k < rh; ++k) {
                std::memcpy(&aj[k * static_cast<std::size_t>(w)], v.wavelet + std::size_t(k) * kV8, kV8 * sizeof(float));
            }
            aj += kV8;
        }
        if (rw & (kV8 - 1)) {
            j = rw & (kV8 - 1);
            v8_interleave_v(v, aj, w, j);
            v8_decode<kAvx2>(v);
            for (std::uint32_t k = 0; k < rh; ++k) {
                std::memcpy(&aj[k * static_cast<std::size_t>(w)], v.wavelet + std::size_t(k) * kV8,
                            static_cast<std::size_t>(j) * sizeof(float));
            }
        }
    }
}

}  // namespace

std::size_t dwt_scratch_bytes(const ResRect* res, std::uint32_t numres, bool reversible) noexcept
{
    if (numres <= 1) {
        return 0;
    }
    const std::size_t mr = max_resolution(res, numres);
    // 5/3: kCols53 columns of the longest side; 9/7: 8 floats per sample (opj_v8_t)
    return reversible ? mr * kCols53 * sizeof(std::int32_t) : mr * kV8 * sizeof(float);
}

void dwt_decode(std::int32_t* data, std::uint32_t w, const ResRect* res, std::uint32_t numres, bool reversible,
                void* scratch) noexcept
{
    if (reversible) {
        decode_53(data, w, res, numres, static_cast<std::int32_t*>(scratch));
    } else {
        decode_97(data, w, res, numres, static_cast<float*>(scratch));
    }
}

namespace kernels {

void idwt53_h_scalar(std::int32_t* row, std::int32_t sn, std::int32_t dn, std::int32_t cas, std::int32_t* tmp) noexcept
{
    h53<false>(row, sn, dn, cas, tmp);
}

void idwt53_v_scalar(std::int32_t* col, std::size_t stride, std::int32_t sn, std::int32_t dn, std::int32_t cas,
                     std::int32_t cols, std::int32_t* tmp) noexcept
{
    v53<false>(col, stride, sn, dn, cas, cols, tmp);
}

void v8dwt_decode_scalar(float* wavelet, std::int32_t sn, std::int32_t dn, std::int32_t cas) noexcept
{
    v8_decode<false>(V8{wavelet, dn, sn, cas});
}

#if defined(__AVX2__)
void idwt53_h_avx2(std::int32_t* row, std::int32_t sn, std::int32_t dn, std::int32_t cas, std::int32_t* tmp) noexcept
{
    h53<true>(row, sn, dn, cas, tmp);
}

void idwt53_v_avx2(std::int32_t* col, std::size_t stride, std::int32_t sn, std::int32_t dn, std::int32_t cas,
                   std::int32_t cols, std::int32_t* tmp) noexcept
{
    v53<true>(col, stride, sn, dn, cas, cols, tmp);
}

void v8dwt_decode_avx2(float* wavelet, std::int32_t sn, std::int32_t dn, std::int32_t cas) noexcept
{
    v8_decode<true>(V8{wavelet, dn, sn, cas});
}
#endif

}  // namespace kernels

}  // namespace nj2k::detail

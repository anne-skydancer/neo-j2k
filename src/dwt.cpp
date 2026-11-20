// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The inverse wavelet transforms of a whole tile-component, after OpenJPEG 2.5.4's dwt.c
// (opj_dwt_decode_tile, opj_idwt53_h, opj_idwt53_v and their kernels; opj_dwt_decode_tile_97,
// opj_v8dwt_interleave_h/_v, opj_v8dwt_decode and its steps): the same lifting steps, the same
// operation order per sample, the same special cases for one and two samples, and the same split
// of the 5/3 vertical pass into groups of 16 columns (OpenJPEG's AVX2 build) with the scalar
// formulas for the rest. Every int32 sum wraps (OpenJPEG's SIMD lanes do; its scalar code leaves
// overflow undefined). Large levels take a row-wise structure (stage 3), small ones OpenJPEG's;
// both give every sample the same operations. OpenJPEG's notice:
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

// One level of opj_dwt_decode_tile as OpenJPEG structures it (stage 1c): every row, then the
// columns 16 at a time (rw x rh, its first row and column on parities vcas and hcas)
template <bool kAvx2>
void level53_columns(std::int32_t* data, std::uint32_t w, std::uint32_t rw, std::uint32_t rh, std::int32_t vsn,
                     std::int32_t vdn, std::int32_t vcas, std::int32_t hsn, std::int32_t hdn, std::int32_t hcas,
                     std::int32_t* mem) noexcept
{
    for (std::uint32_t j = 0; j < rh; ++j) {
        h53<kAvx2>(&data[static_cast<std::size_t>(j) * w], hsn, hdn, hcas, mem);
    }
    std::uint32_t j = 0;
    for (; j + kCols53 <= rw; j += kCols53) {
        v53<kAvx2>(&data[j], w, vsn, vdn, vcas, kCols53, mem);
    }
    if (j < rw) {
        v53<kAvx2>(&data[j], w, vsn, vdn, vcas, static_cast<std::int32_t>(rw - j), mem);
    }
}

// opj_dwt_decode_tile, one thread, as OpenJPEG structures it (stage 1c; the reference)
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
        const std::int32_t vdn = static_cast<std::int32_t>(rh - static_cast<std::uint32_t>(vsn));
        level53_columns<kAvx2>(data, w, rw, rh, vsn, vdn, tr->y0 % 2, hsn, hdn, tr->x0 % 2, mem);
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

// One level of opj_dwt_decode_tile_97 as OpenJPEG structures it (stage 1c): eight rows at a time
// gathered into opj_v8dwt_t, then eight columns at a time
template <bool kAvx2>
void level97_columns(std::int32_t* data, std::uint32_t w, std::uint32_t rw, std::uint32_t rh, std::int32_t vsn,
                     std::int32_t vdn, std::int32_t vcas, std::int32_t hsn, std::int32_t hdn, std::int32_t hcas,
                     float* wavelet) noexcept
{
    float* aj = reinterpret_cast<float*>(data);  // the samples hold float bits
    V8 h{wavelet, hdn, hsn, hcas};
    V8 v{wavelet, vdn, vsn, vcas};
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

// opj_dwt_decode_tile_97, one thread, as OpenJPEG structures it (stage 1c; the reference)
void decode_97_reference(std::int32_t* data, std::uint32_t w, const ResRect* res, std::uint32_t numres, float* wavelet) noexcept
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
    while (--numres) {
        const std::int32_t hsn = static_cast<std::int32_t>(rw);
        const std::int32_t vsn = static_cast<std::int32_t>(rh);
        ++res;
        rw = static_cast<std::uint32_t>(res->x1 - res->x0);
        rh = static_cast<std::uint32_t>(res->y1 - res->y0);
        const std::int32_t hdn = static_cast<std::int32_t>(rw - static_cast<std::uint32_t>(hsn));
        const std::int32_t vdn = static_cast<std::int32_t>(rh - static_cast<std::uint32_t>(vsn));
        level97_columns<kAvx2>(data, w, rw, rh, vsn, vdn, res->y0 % 2, hsn, hdn, res->x0 % 2, wavelet);
    }
}

// ---- stage 3: row-wise --------------------------------------------------------------------------------
//
// A level whose samples fit in the first caches takes OpenJPEG's structure (the gathers are cheap
// there, and the row-wise passes cost more per row than they save); from these areas on (samples
// of the level), the row-wise transform. Both give the same samples.
// (measured on the kernel bench, nj2k_kernels: the row-wise 9/7 gains from 128^2, the 5/3 from 512^2)
constexpr std::size_t kRowWise97 = 128 * 128;
constexpr std::size_t kRowWise53 = 512 * 512;

// ---- 9/7, stage 3: row-wise ---------------------------------------------------------------------------
//
// The lifting steps of opj_v8dwt_decode, each sample taking the same operations in the same order,
// on whole rows of the buffer where they lie, so every access is contiguous:
// - the horizontal pass lifts each row's low and high halves in place, eight samples at a time, then
//   interleaves them (OpenJPEG gathers eight rows into opj_v8dwt_t and scatters them back);
// - the vertical pass lifts the low and the high rows in place, a row at a time, the four steps
//   advancing together down the rows so a row is in the cache from its first step to its last
//   (OpenJPEG gathers eight-column strips, half a cache line from each row, rows a stride apart),
//   then puts the rows in their interleaved order by following the permutation's cycles with one
//   row of scratch. Memory beyond the tile's buffer: a row and a bit a row.

// opj_v8dwt_decode_step2 on one row's halves where they lie: target t[i], i < min(end, m), takes
// (s[i - 1 + off] + s[i + off]) * c with s[-1] read as s[0] (the mirror at the first sample, off =
// 0); when m < end, t[m] takes (c + c) * s[m - 1 + off] (the mirror at the last)
void row_step2_scalar(float* t, const float* s, std::uint32_t end, std::uint32_t m, float c, std::uint32_t off) noexcept
{
    const std::uint32_t imax = end < m ? end : m;
    std::uint32_t i = 0;
    if (imax >= 1) {
        t[0] = t[0] + ((s[0] + s[off]) * c);
        i = 1;
    }
    for (; i < imax; ++i) {
        t[i] = t[i] + ((s[i - 1 + off] + s[i + off]) * c);
    }
    if (m < end) {
        c = c + c;
        t[imax] = t[imax] + (c * s[imax + off - 1]);
    }
}

#if defined(__AVX2__)
void row_step2_avx2(float* t, const float* s, std::uint32_t end, std::uint32_t m, float c, std::uint32_t off) noexcept
{
    const std::uint32_t imax = end < m ? end : m;
    std::uint32_t i = 0;
    if (imax >= 1) {
        t[0] = t[0] + ((s[0] + s[off]) * c);
        i = 1;
    }
    const __m256 vc = _mm256_set1_ps(c);
    for (; i + 8 <= imax; i += 8) {
        const __m256 sum = _mm256_add_ps(_mm256_loadu_ps(s + i - 1 + off), _mm256_loadu_ps(s + i + off));
        _mm256_storeu_ps(t + i, _mm256_add_ps(_mm256_loadu_ps(t + i), _mm256_mul_ps(sum, vc)));
    }
    for (; i < imax; ++i) {
        t[i] = t[i] + ((s[i - 1 + off] + s[i + off]) * c);
    }
    if (m < end) {
        c = c + c;
        t[imax] = t[imax] + (c * s[imax + off - 1]);
    }
}
#endif

// One row of the horizontal pass: sn low then dn high samples in row, the first of the
// resolution's row on parity cas; the row receives them lifted and interleaved (tmp: sn + dn floats)
template <bool kAvx2>
void h97_row(float* row, std::int32_t sn, std::int32_t dn, std::int32_t cas, float* tmp) noexcept
{
    float* lo = row;
    float* hi = row + sn;
    const std::uint32_t usn = static_cast<std::uint32_t>(sn), udn = static_cast<std::uint32_t>(dn);
    const bool lift = cas == 0 ? (dn > 0 || sn > 1) : (sn > 0 || dn > 1);
    if (lift) {
        const std::int32_t a = cas == 0 ? 0 : 1, b = 1 - a;
        const std::uint32_t ml = static_cast<std::uint32_t>(imin(sn, dn - a));
        const std::uint32_t mh = static_cast<std::uint32_t>(imin(dn, sn - b));
        const std::uint32_t la = static_cast<std::uint32_t>(a), lb = static_cast<std::uint32_t>(b);
#if defined(__AVX2__)
        if constexpr (kAvx2) {
            const __m256 vk = _mm256_set1_ps(kK), vt = _mm256_set1_ps(kTwoInvK);
            std::uint32_t i = 0;
            for (; i + 8 <= usn; i += 8) {
                _mm256_storeu_ps(lo + i, _mm256_mul_ps(_mm256_loadu_ps(lo + i), vk));
            }
            for (; i < usn; ++i) {
                lo[i] = lo[i] * kK;
            }
            for (i = 0; i + 8 <= udn; i += 8) {
                _mm256_storeu_ps(hi + i, _mm256_mul_ps(_mm256_loadu_ps(hi + i), vt));
            }
            for (; i < udn; ++i) {
                hi[i] = hi[i] * kTwoInvK;
            }
            row_step2_avx2(lo, hi, usn, ml, -kDelta, la);
            row_step2_avx2(hi, lo, udn, mh, -kGamma, lb);
            row_step2_avx2(lo, hi, usn, ml, -kBeta, la);
            row_step2_avx2(hi, lo, udn, mh, -kAlpha, lb);
        } else
#endif
        {
            for (std::uint32_t i = 0; i < usn; ++i) {
                lo[i] = lo[i] * kK;
            }
            for (std::uint32_t i = 0; i < udn; ++i) {
                hi[i] = hi[i] * kTwoInvK;
            }
            row_step2_scalar(lo, hi, usn, ml, -kDelta, la);
            row_step2_scalar(hi, lo, udn, mh, -kGamma, lb);
            row_step2_scalar(lo, hi, usn, ml, -kBeta, la);
            row_step2_scalar(hi, lo, udn, mh, -kAlpha, lb);
        }
    }
    // interleave: the row's first sample is a low one on cas 0, a high one on cas 1
    const float* first = cas == 0 ? lo : hi;
    const float* second = cas == 0 ? hi : lo;
    const std::uint32_t nfirst = cas == 0 ? usn : udn, nsecond = cas == 0 ? udn : usn;
    const std::uint32_t pairs = nsecond < nfirst ? nsecond : nfirst;
    std::uint32_t i = 0;
#if defined(__AVX2__)
    if constexpr (kAvx2) {
        for (; i + 8 <= pairs; i += 8) {
            const __m256 f = _mm256_loadu_ps(first + i), s = _mm256_loadu_ps(second + i);
            const __m256 l = _mm256_unpacklo_ps(f, s), h = _mm256_unpackhi_ps(f, s);
            _mm256_storeu_ps(tmp + 2 * i, _mm256_permute2f128_ps(l, h, 0x20));
            _mm256_storeu_ps(tmp + 2 * i + 8, _mm256_permute2f128_ps(l, h, 0x31));
        }
    }
#endif
    for (; i < pairs; ++i) {
        tmp[2 * i] = first[i];
        tmp[2 * i + 1] = second[i];
    }
    if (nfirst > pairs) {
        tmp[2 * pairs] = first[pairs];
    } else if (nsecond > pairs) {
        tmp[2 * pairs + 1] = second[pairs];
    }
    std::memcpy(row, tmp, std::size_t(usn + udn) * sizeof(float));
}

// Row operations of the vertical pass over n floats: opj_v8dwt_decode_step1's t * c, step2's
// t + ((s1 + s2) * c), and its last sample's t + (c * s) (c doubled by the caller)
template <bool kAvx2>
void row_scale(float* t, std::uint32_t n, float c) noexcept
{
    std::uint32_t i = 0;
#if defined(__AVX2__)
    if constexpr (kAvx2) {
        const __m256 vc = _mm256_set1_ps(c);
        for (; i + 8 <= n; i += 8) {
            _mm256_storeu_ps(t + i, _mm256_mul_ps(_mm256_loadu_ps(t + i), vc));
        }
    }
#endif
    for (; i < n; ++i) {
        t[i] = t[i] * c;
    }
}

template <bool kAvx2>
void row_lift(float* t, const float* s1, const float* s2, std::uint32_t n, float c) noexcept
{
    std::uint32_t i = 0;
#if defined(__AVX2__)
    if constexpr (kAvx2) {
        const __m256 vc = _mm256_set1_ps(c);
        for (; i + 8 <= n; i += 8) {
            const __m256 sum = _mm256_add_ps(_mm256_loadu_ps(s1 + i), _mm256_loadu_ps(s2 + i));
            _mm256_storeu_ps(t + i, _mm256_add_ps(_mm256_loadu_ps(t + i), _mm256_mul_ps(sum, vc)));
        }
    }
#endif
    for (; i < n; ++i) {
        t[i] = t[i] + ((s1[i] + s2[i]) * c);
    }
}

template <bool kAvx2>
void row_lift_edge(float* t, const float* s, std::uint32_t n, float c2) noexcept
{
    std::uint32_t i = 0;
#if defined(__AVX2__)
    if constexpr (kAvx2) {
        const __m256 vc = _mm256_set1_ps(c2);
        for (; i + 8 <= n; i += 8) {
            _mm256_storeu_ps(t + i, _mm256_add_ps(_mm256_loadu_ps(t + i), _mm256_mul_ps(vc, _mm256_loadu_ps(s + i))));
        }
    }
#endif
    for (; i < n; ++i) {
        t[i] = t[i] + (c2 * s[i]);
    }
}

// The vertical lifting of cols columns: sn low rows from base, then dn high ones, rows stride
// floats apart, the resolution's first row on parity cas. The steps are those of opj_v8dwt_decode
// (row_step2's indices, a row for a sample); step s updates its target rows in order, each as soon
// as the rows it reads have had step s - 1 (or the scaling) and the row itself its previous step,
// which also keeps every row from being updated before the steps that read its older value are
// done with it (step s + 1's targets wait for step s beyond the rows they overwrite).
// Each row first takes the level's horizontal pass (h97_row: hsn, hdn, hcas, tmp), just before its
// first vertical step, while it is in the cache.
template <bool kAvx2>
void v97_lift_rows(float* base, std::size_t stride, std::uint32_t cols, std::int32_t sn, std::int32_t dn,
                   std::int32_t cas, std::int32_t hsn, std::int32_t hdn, std::int32_t hcas, float* tmp) noexcept
{
    const bool lift = cas == 0 ? (dn > 0 || sn > 1) : (sn > 0 || dn > 1);
    if (!lift) {
        for (std::int32_t j = 0; j < sn + dn; ++j) {
            h97_row<kAvx2>(base + std::size_t(j) * stride, hsn, hdn, hcas, tmp);
        }
        return;
    }
    const std::int32_t a = cas == 0 ? 0 : 1, b = 1 - a;
    const std::uint32_t usn = static_cast<std::uint32_t>(sn), udn = static_cast<std::uint32_t>(dn);
    const std::uint32_t ml = static_cast<std::uint32_t>(imin(sn, dn - a));
    const std::uint32_t mh = static_cast<std::uint32_t>(imin(dn, sn - b));
    struct Step {
        bool low;  // its targets are the low rows (sources the high ones)
        std::uint32_t imax, count, off;
        float c;
    };
    auto make = [](bool low, std::uint32_t end, std::uint32_t m, std::uint32_t off, float c) {
        const std::uint32_t imax = end < m ? end : m;
        return Step{low, imax, imax + (m < end ? 1u : 0u), off, c};
    };
    const Step steps[4] = {make(true, usn, ml, std::uint32_t(a), -kDelta), make(false, udn, mh, std::uint32_t(b), -kGamma),
                           make(true, usn, ml, std::uint32_t(a), -kBeta), make(false, udn, mh, std::uint32_t(b), -kAlpha)};
    auto low = [&](std::uint32_t i) { return base + std::size_t(i) * stride; };
    auto high = [&](std::uint32_t i) { return base + std::size_t(usn + i) * stride; };
    std::uint32_t next[4] = {0, 0, 0, 0};
    std::uint32_t scaled_low = 0, scaled_high = 0;
    const std::uint32_t rows = usn > udn ? usn : udn;
    for (std::uint32_t k = 0; k < rows; ++k) {
        if (k < usn) {
            h97_row<kAvx2>(low(k), hsn, hdn, hcas, tmp);
            row_scale<kAvx2>(low(k), cols, kK);
            scaled_low = k + 1;
        }
        if (k < udn) {
            h97_row<kAvx2>(high(k), hsn, hdn, hcas, tmp);
            row_scale<kAvx2>(high(k), cols, kTwoInvK);
            scaled_high = k + 1;
        }
        for (std::uint32_t s = 0; s < 4; ++s) {
            const Step& p = steps[s];
            const std::uint32_t sources = s == 0 ? scaled_high : next[s - 1];
            const std::uint32_t targets = s == 0 ? scaled_low : (s == 1 ? scaled_high : next[s - 2]);
            while (next[s] < p.count) {
                const std::uint32_t i = next[s];
                // the last source row the target reads (imax + off >= 1 whenever a step has an edge
                // target: sn and dn differ by at most one)
                const std::uint32_t need = i < p.imax ? i + p.off : p.imax + p.off - 1;
                if (sources <= need || targets <= i) {
                    break;
                }
                float* t = p.low ? low(i) : high(i);
                if (i < p.imax) {
                    const std::uint32_t j1 = i + p.off == 0 ? 0 : i + p.off - 1;
                    row_lift<kAvx2>(t, p.low ? high(j1) : low(j1), p.low ? high(i + p.off) : low(i + p.off), cols, p.c);
                } else {
                    row_lift_edge<kAvx2>(t, p.low ? high(need) : low(need), cols, p.c + p.c);
                }
                ++next[s];
            }
        }
    }
}

// The rows of a vertical pass from their order in the buffer (sn low, then dn high) into the
// resolution's (the low ones on parity cas): each cycle of the permutation followed with one row of
// scratch (tmp, row_bytes), every row copied once; visited: a bit a row
void interleave_rows(void* base_v, std::size_t stride, std::size_t row_bytes, std::uint32_t sn, std::uint32_t dn,
                     std::uint32_t cas, void* tmp, std::uint8_t* visited) noexcept
{
    auto* base = static_cast<std::uint8_t*>(base_v);
    const std::uint32_t len = sn + dn;
    std::memset(visited, 0, (len + 7) / 8);
    // the buffer row whose samples go to row q of the resolution
    auto source = [&](std::uint32_t q) { return (q & 1) == cas ? (q - cas) / 2 : sn + (q + cas - 1) / 2; };
    for (std::uint32_t start = 0; start < len; ++start) {
        if (visited[start >> 3] & (1u << (start & 7))) {
            continue;
        }
        visited[start >> 3] = static_cast<std::uint8_t>(visited[start >> 3] | (1u << (start & 7)));
        std::uint32_t from = source(start);
        if (from == start) {
            continue;
        }
        std::memcpy(tmp, base + std::size_t(start) * stride, row_bytes);
        std::uint32_t cur = start;
        while (from != start) {
            std::memcpy(base + std::size_t(cur) * stride, base + std::size_t(from) * stride, row_bytes);
            cur = from;
            visited[cur >> 3] = static_cast<std::uint8_t>(visited[cur >> 3] | (1u << (cur & 7)));
            from = source(cur);
        }
        std::memcpy(base + std::size_t(cur) * stride, tmp, row_bytes);
    }
}

// opj_dwt_decode_tile_97, one thread, row-wise
template <bool kAvx2>
void decode_97_t(std::int32_t* data, std::uint32_t w, const ResRect* res, std::uint32_t numres, float* wavelet,
                 std::size_t row_wise = kRowWise97) noexcept
{
    std::uint32_t rw = static_cast<std::uint32_t>(res->x1 - res->x0);
    std::uint32_t rh = static_cast<std::uint32_t>(res->y1 - res->y0);
    if (numres == 1 || w == 0) {
        return;
    }
    while (--numres) {
        float* aj = reinterpret_cast<float*>(data);  // the samples hold float bits
        const std::int32_t hsn = static_cast<std::int32_t>(rw);
        const std::int32_t vsn = static_cast<std::int32_t>(rh);
        ++res;
        rw = static_cast<std::uint32_t>(res->x1 - res->x0);
        rh = static_cast<std::uint32_t>(res->y1 - res->y0);
        const std::int32_t hdn = static_cast<std::int32_t>(rw - static_cast<std::uint32_t>(hsn));
        const std::int32_t hcas = res->x0 % 2;
        const std::int32_t vdn = static_cast<std::int32_t>(rh - static_cast<std::uint32_t>(vsn));
        const std::int32_t vcas = res->y0 % 2;
        if (std::size_t(rw) * rh < row_wise) {
            level97_columns<kAvx2>(data, w, rw, rh, vsn, vdn, vcas, hsn, hdn, hcas, wavelet);
            continue;
        }
        v97_lift_rows<kAvx2>(aj, w, rw, vsn, vdn, vcas, hsn, hdn, hcas, wavelet);
        interleave_rows(aj, std::size_t(w) * sizeof(float), std::size_t(rw) * sizeof(float),
                        static_cast<std::uint32_t>(vsn), static_cast<std::uint32_t>(vdn), static_cast<std::uint32_t>(vcas),
                        wavelet, reinterpret_cast<std::uint8_t*>(wavelet + rw));
    }
}

// ---- 5/3, stage 3: row-wise -------------------------------------------------------------------------
//
// The vertical pass of opj_idwt53_v on whole rows where they lie (OpenJPEG gathers 16 columns,
// one cache line from each row, rows a stride apart): the low rows' predict step and the high rows'
// update step advancing together down the rows, each row taking the level's horizontal pass just
// before its first vertical use, then the rows put in their interleaved order (v97's permutation).
// Every sample takes OpenJPEG's formulas; at the two ends, the columns of a full group of 16 take its
// SIMD build's (d + d + 2) >> 2 and the others its scalar (d + 1) >> 1, which differ where the
// int32 sums wrap.

// t - ((a + b + 2) >> 2)
template <bool kAvx2>
void row53_predict(std::int32_t* t, const std::int32_t* a, const std::int32_t* b, std::uint32_t n) noexcept
{
    std::uint32_t i = 0;
#if defined(__AVX2__)
    if constexpr (kAvx2) {
        const __m256i two = _mm256_set1_epi32(2);
        for (; i + 8 <= n; i += 8) {
            const __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i));
            const __m256i vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i));
            const __m256i vt = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(t + i));
            const __m256i d = _mm256_srai_epi32(_mm256_add_epi32(_mm256_add_epi32(va, vb), two), 2);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(t + i), _mm256_sub_epi32(vt, d));
        }
    }
#endif
    for (; i < n; ++i) {
        t[i] = sub(t[i], add(add(a[i], b[i]), 2) >> 2);
    }
}

// the end rows' predict: t - ((h + h + 2) >> 2) in columns [0, g), t - ((h + 1) >> 1) beyond
template <bool kAvx2>
void row53_predict_edge(std::int32_t* t, const std::int32_t* h, std::uint32_t n, std::uint32_t g) noexcept
{
    std::uint32_t i = 0;
#if defined(__AVX2__)
    if constexpr (kAvx2) {
        const __m256i two = _mm256_set1_epi32(2);
        for (; i + 8 <= g; i += 8) {
            const __m256i vh = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(h + i));
            const __m256i vt = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(t + i));
            const __m256i d = _mm256_srai_epi32(_mm256_add_epi32(_mm256_add_epi32(vh, vh), two), 2);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(t + i), _mm256_sub_epi32(vt, d));
        }
    }
#endif
    for (; i < g; ++i) {
        t[i] = sub(t[i], add(add(h[i], h[i]), 2) >> 2);
    }
    for (; i < n; ++i) {
        t[i] = sub(t[i], add(h[i], 1) >> 1);
    }
}

// t + ((a + b) >> 1)
template <bool kAvx2>
void row53_update(std::int32_t* t, const std::int32_t* a, const std::int32_t* b, std::uint32_t n) noexcept
{
    std::uint32_t i = 0;
#if defined(__AVX2__)
    if constexpr (kAvx2) {
        for (; i + 8 <= n; i += 8) {
            const __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i));
            const __m256i vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i));
            const __m256i vt = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(t + i));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(t + i),
                                _mm256_add_epi32(vt, _mm256_srai_epi32(_mm256_add_epi32(va, vb), 1)));
        }
    }
#endif
    for (; i < n; ++i) {
        t[i] = add(t[i], add(a[i], b[i]) >> 1);
    }
}

// t + a
template <bool kAvx2>
void row53_update_edge(std::int32_t* t, const std::int32_t* a, std::uint32_t n) noexcept
{
    std::uint32_t i = 0;
#if defined(__AVX2__)
    if constexpr (kAvx2) {
        for (; i + 8 <= n; i += 8) {
            const __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i));
            const __m256i vt = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(t + i));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(t + i), _mm256_add_epi32(vt, va));
        }
    }
#endif
    for (; i < n; ++i) {
        t[i] = add(t[i], a[i]);
    }
}

// One level of the 5/3 transform from rows in the buffer (rh rows of rw samples, w apart): sn low
// then dn high rows, the first on parity cas; hsn, hdn, hcas: the horizontal pass's split.
template <bool kAvx2>
void level53_rows(std::int32_t* base, std::uint32_t w, std::uint32_t rw, std::int32_t sn, std::int32_t dn,
                  std::int32_t cas, std::int32_t hsn, std::int32_t hdn, std::int32_t hcas, std::int32_t* tmp) noexcept
{
    const std::uint32_t usn = static_cast<std::uint32_t>(sn), udn = static_cast<std::uint32_t>(dn);
    const std::uint32_t len = usn + udn;
    const std::uint32_t g = (rw / kCols53) * kCols53;  // the columns of full groups of 16
    auto low = [&](std::uint32_t i) { return base + std::size_t(i) * w; };
    auto high = [&](std::uint32_t i) { return base + std::size_t(usn + i) * w; };
    // the horizontal pass, a row just before the vertical pass reads it
    std::uint32_t h_low = 0, h_high = 0;
    auto horizontal = [&](std::uint32_t nlow, std::uint32_t nhigh) {
        for (; h_low < nlow && h_low < usn; ++h_low) {
            h53<kAvx2>(low(h_low), hsn, hdn, hcas, tmp);
        }
        for (; h_high < nhigh && h_high < udn; ++h_high) {
            h53<kAvx2>(high(h_high), hsn, hdn, hcas, tmp);
        }
    };
    if (cas == 0) {
        if (len > 1) {
            // L'0 = L0 - E(H0); L'i = Li - ((H(i-1) + Hi + 2) >> 2); an odd length's last L takes
            // E(H(dn-1)); H'j = Hj + ((L'j + L'(j+1)) >> 1), an even length's last Hj + L'(sn-1)
            auto predict = [&](std::uint32_t i) {
                horizontal(i + 1, i + 1);
                if (i == 0) {
                    row53_predict_edge<kAvx2>(low(0), high(0), rw, g);
                } else if (i < udn) {
                    row53_predict<kAvx2>(low(i), high(i - 1), high(i), rw);
                } else {
                    row53_predict_edge<kAvx2>(low(i), high(udn - 1), rw, g);
                }
            };
            auto update = [&](std::uint32_t j) {
                if (j + 1 < usn) {
                    row53_update<kAvx2>(high(j), low(j), low(j + 1), rw);
                } else {
                    row53_update_edge<kAvx2>(high(j), low(usn - 1), rw);
                }
            };
            for (std::uint32_t k = 0; k < usn; ++k) {
                predict(k);
                if (k >= 1) {
                    update(k - 1);
                }
            }
            for (std::uint32_t j = usn - 1; j < udn; ++j) {
                update(j);
            }
        }
        horizontal(usn, udn);
    } else if (len == 1) {
        horizontal(usn, udn);
        std::int32_t* r = base;
        for (std::uint32_t c = 0; c < rw; ++c) {
            r[c] /= 2;
        }
    } else if (len == 2) {
        horizontal(usn, udn);
        std::int32_t* l = low(0);
        std::int32_t* h = high(0);
        for (std::uint32_t c = 0; c < rw; ++c) {
            const std::int32_t o1 = sub(l[c], add(h[c], 1) >> 1);
            l[c] = o1;
            h[c] = add(h[c], o1);
        }
    } else if (len > 2) {
        // L'i = Li - ((Hi + H(i+1) + 2) >> 2), an even length's last L takes E(H(dn-1));
        // H'0 = H0 + L'0, H'j = Hj + ((L'j + L'(j-1)) >> 1), an odd length's last Hj + L'(sn-1)
        auto predict = [&](std::uint32_t i) {
            horizontal(i + 1, i + 2);
            if (i + 1 < udn) {
                row53_predict<kAvx2>(low(i), high(i), high(i + 1), rw);
            } else {
                row53_predict_edge<kAvx2>(low(i), high(udn - 1), rw, g);
            }
        };
        auto update = [&](std::uint32_t j) {
            if (j == 0) {
                row53_update_edge<kAvx2>(high(0), low(0), rw);
            } else if (j < usn) {
                row53_update<kAvx2>(high(j), low(j), low(j - 1), rw);
            } else {
                row53_update_edge<kAvx2>(high(j), low(usn - 1), rw);
            }
        };
        for (std::uint32_t k = 0; k < usn; ++k) {
            predict(k);
            update(k);
        }
        horizontal(usn, udn);
        for (std::uint32_t j = usn; j < udn; ++j) {
            update(j);
        }
    } else {
        horizontal(usn, udn);
    }
    interleave_rows(base, std::size_t(w) * sizeof(std::int32_t), std::size_t(rw) * sizeof(std::int32_t), usn, udn,
                    static_cast<std::uint32_t>(cas), tmp, reinterpret_cast<std::uint8_t*>(tmp + rw));
}

// opj_dwt_decode_tile, one thread, row-wise
template <bool kAvx2>
void decode_53_rows(std::int32_t* data, std::uint32_t w, const ResRect* tr, std::uint32_t numres, std::int32_t* mem,
                    std::size_t row_wise = kRowWise53) noexcept
{
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
        const std::int32_t vdn = static_cast<std::int32_t>(rh - static_cast<std::uint32_t>(vsn));
        if (std::size_t(rw) * rh < row_wise) {
            level53_columns<kAvx2>(data, w, rw, rh, vsn, vdn, tr->y0 % 2, hsn, hdn, tr->x0 % 2, mem);
        } else {
            level53_rows<kAvx2>(data, w, rw, vsn, vdn, tr->y0 % 2, hsn, hdn, tr->x0 % 2, mem);
        }
    }
}

void decode_97(std::int32_t* data, std::uint32_t w, const ResRect* res, std::uint32_t numres, float* wavelet) noexcept
{
#if defined(__AVX2__)
    decode_97_t<true>(data, w, res, numres, wavelet);
#else
    decode_97_t<false>(data, w, res, numres, wavelet);
#endif
}

}  // namespace

std::size_t dwt_scratch_bytes(const ResRect* res, std::uint32_t numres, bool reversible) noexcept
{
    if (numres <= 1) {
        return 0;
    }
    const std::size_t mr = max_resolution(res, numres);
    // 5/3: kCols53 columns of the longest side; 9/7: a row and a bit a row (the row-wise transform
    // needs less than the eight floats a sample OpenJPEG's took)
    return reversible ? mr * kCols53 * sizeof(std::int32_t) : mr * kV8 * sizeof(float);
}

void dwt_decode(std::int32_t* data, std::uint32_t w, const ResRect* res, std::uint32_t numres, bool reversible,
                void* scratch) noexcept
{
    if (reversible) {
#if defined(__AVX2__)
        decode_53_rows<true>(data, w, res, numres, static_cast<std::int32_t*>(scratch));
#else
        decode_53_rows<false>(data, w, res, numres, static_cast<std::int32_t*>(scratch));
#endif
    } else {
        decode_97(data, w, res, numres, static_cast<float*>(scratch));
    }
}

namespace kernels {

std::size_t dwt_scratch_bytes_reference(const ResRect* res, std::uint32_t numres, bool reversible) noexcept
{
    if (numres <= 1) {
        return 0;
    }
    const std::size_t mr = max_resolution(res, numres);
    return reversible ? mr * kCols53 * sizeof(std::int32_t) : mr * kV8 * sizeof(float);
}

void dwt_decode_reference(std::int32_t* data, std::uint32_t w, const ResRect* res, std::uint32_t numres,
                          bool reversible, void* scratch) noexcept
{
    if (reversible) {
        decode_53(data, w, res, numres, static_cast<std::int32_t*>(scratch));
    } else {
        decode_97_reference(data, w, res, numres, static_cast<float*>(scratch));
    }
}

void dwt_decode_scalar(std::int32_t* data, std::uint32_t w, const ResRect* res, std::uint32_t numres, bool reversible,
                       void* scratch) noexcept
{
    if (reversible) {
        decode_53_rows<false>(data, w, res, numres, static_cast<std::int32_t*>(scratch));
    } else {
        decode_97_t<false>(data, w, res, numres, static_cast<float*>(scratch));
    }
}

void dwt_decode_row_wise(std::int32_t* data, std::uint32_t w, const ResRect* res, std::uint32_t numres,
                         bool reversible, bool scalar, void* scratch) noexcept
{
#if defined(__AVX2__)
    if (!scalar) {
        if (reversible) {
            decode_53_rows<true>(data, w, res, numres, static_cast<std::int32_t*>(scratch), 0);
        } else {
            decode_97_t<true>(data, w, res, numres, static_cast<float*>(scratch), 0);
        }
        return;
    }
#endif
    (void)scalar;
    if (reversible) {
        decode_53_rows<false>(data, w, res, numres, static_cast<std::int32_t*>(scratch), 0);
    } else {
        decode_97_t<false>(data, w, res, numres, static_cast<float*>(scratch), 0);
    }
}

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

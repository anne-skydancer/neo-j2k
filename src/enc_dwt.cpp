// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The encoder's forward transforms (stage 6a), ported from OpenJPEG 2.5.4 at the oracle's pin: the 5/3
// and 9/7 wavelets (dwt.c opj_dwt_encode, opj_dwt_encode_real: the lifting of each line and each group
// of eight columns in OpenJPEG's order, which the SSE paths share), the step sizes of the quantization
// (opj_dwt_calc_explicit_stepsizes), the norms, and the colour transforms (mct.c). The 9/7 lifting is
// float arithmetic without contraction (D2), as the decoder's. OpenJPEG's notice:
//
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
 * Copyright (c) 2007, Callum Lerwick <seg@haxxed.com>
 * Copyright (c) 2008, Jerome Fimes, Communications & Systemes <jerome.fimes@c-s.fr>
 * Copyright (c) 2012, Carl Hetherington
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
#include <cmath>
#include <cstring>
#include <type_traits>

#include "enc_tile.h"

namespace nj2k::detail {

namespace {

// From table F.4 from the standard
constexpr float kAlpha = -1.586134342f;
constexpr float kBeta = -0.052980118f;
constexpr float kGamma = 0.882911075f;
constexpr float kDelta = 0.443506852f;
constexpr float kK = 1.230174105f;
constexpr float kInvK = static_cast<float>(1.0 / 1.230174105);

constexpr std::uint32_t kLanes = 8;  // NB_ELTS_V8: the columns a vertical pass takes at once

// opj_dwt_norms: the norms of the 5-3 wavelets for different bands
const double kNorms[4][10] = {{1.000, 1.500, 2.750, 5.375, 10.68, 21.34, 42.67, 85.33, 170.7, 341.3},
                              {1.038, 1.592, 2.919, 5.703, 11.33, 22.64, 45.25, 90.48, 180.9},
                              {1.038, 1.592, 2.919, 5.703, 11.33, 22.64, 45.25, 90.48, 180.9},
                              {.7186, .9218, 1.586, 3.043, 6.019, 12.01, 24.00, 47.97, 95.93}};

// opj_dwt_norms_real: the norms of the 9-7 wavelets for different bands
const double kNormsReal[4][10] = {{1.000, 1.965, 4.177, 8.403, 16.90, 33.84, 67.69, 135.3, 270.6, 540.9},
                                  {2.022, 3.989, 8.355, 17.04, 34.27, 68.63, 137.3, 274.6, 549.0},
                                  {2.022, 3.989, 8.355, 17.04, 34.27, 68.63, 137.3, 274.6, 549.0},
                                  {2.080, 3.865, 8.307, 17.18, 34.71, 69.59, 139.3, 278.6, 557.2}};

int floorlog2(std::int32_t a) noexcept
{
    int l;
    for (l = 0; a > 1; ++l) {
        a >>= 1;
    }
    return l;
}

// ---- the 5-3 lifting of one line, or of up to eight columns held side by side ----
// Element k of lane c is t[k * stride + c]; nl lanes are computed.
void lift53(std::int32_t* t, std::uint32_t n, bool even, std::uint32_t stride, std::uint32_t nl) noexcept
{
    const std::uint32_t sn = (n + (even ? 1 : 0)) >> 1;
    const std::uint32_t dn = n - sn;
    auto S = [&](std::uint32_t i) { return t + std::size_t(2 * i) * stride; };
    auto D = [&](std::uint32_t i) { return t + std::size_t(1 + 2 * i) * stride; };
    std::uint32_t i;
    if (even) {
        if (n > 1) {
            for (i = 0; i + 1 < sn; i++) {
                for (std::uint32_t c = 0; c < nl; c++) {
                    D(i)[c] -= (S(i)[c] + S(i + 1)[c]) >> 1;
                }
            }
            if ((n % 2) == 0) {
                for (std::uint32_t c = 0; c < nl; c++) {
                    D(i)[c] -= S(i)[c];
                }
            }
            for (std::uint32_t c = 0; c < nl; c++) {
                S(0)[c] += (D(0)[c] + D(0)[c] + 2) >> 2;
            }
            for (i = 1; i < dn; i++) {
                for (std::uint32_t c = 0; c < nl; c++) {
                    S(i)[c] += (D(i - 1)[c] + D(i)[c] + 2) >> 2;
                }
            }
            if ((n % 2) == 1) {
                for (std::uint32_t c = 0; c < nl; c++) {
                    S(i)[c] += (D(i - 1)[c] + D(i - 1)[c] + 2) >> 2;
                }
            }
        }
    } else {
        if (n == 1) {
            for (std::uint32_t c = 0; c < nl; c++) {
                S(0)[c] *= 2;
            }
        } else {
            for (std::uint32_t c = 0; c < nl; c++) {
                S(0)[c] -= D(0)[c];
            }
            for (i = 1; i < sn; i++) {
                for (std::uint32_t c = 0; c < nl; c++) {
                    S(i)[c] -= (D(i)[c] + D(i - 1)[c]) >> 1;
                }
            }
            if ((n % 2) == 1) {
                for (std::uint32_t c = 0; c < nl; c++) {
                    S(i)[c] -= D(i - 1)[c];
                }
            }
            for (i = 0; i + 1 < dn; i++) {
                for (std::uint32_t c = 0; c < nl; c++) {
                    D(i)[c] += (S(i)[c] + S(i + 1)[c] + 2) >> 2;
                }
            }
            if ((n % 2) == 0) {
                for (std::uint32_t c = 0; c < nl; c++) {
                    D(i)[c] += (S(i)[c] + S(i)[c] + 2) >> 2;
                }
            }
        }
    }
}

// opj_dwt_encode_step2 and opj_v8dwt_encode_step2: the lifting step of one parity from the
// neighbours of the other. a and b are the offsets of the two parities (0 and 1, or 1 and 0).
void step2(float* w, std::uint32_t stride, std::uint32_t nl, std::uint32_t a, std::uint32_t b, std::uint32_t end,
           std::uint32_t m, float cst) noexcept
{
    const std::uint32_t imax = end < m ? end : m;
    for (std::uint32_t i = 0; i < imax; ++i) {
        const std::uint32_t p = b + 2 * i;
        const float* left = w + std::size_t(i == 0 ? a : p - 1) * stride;
        const float* right = w + std::size_t(p + 1) * stride;
        float* dst = w + std::size_t(p) * stride;
        for (std::uint32_t c = 0; c < nl; c++) {
            dst[c] += (left[c] + right[c]) * cst;
        }
    }
    if (m < end) {
        const std::uint32_t p = b + 2 * imax;
        const float* prev = w + std::size_t(p - 1) * stride;
        float* dst = w + std::size_t(p) * stride;
        for (std::uint32_t c = 0; c < nl; c++) {
            dst[c] += (2 * prev[c]) * cst;
        }
    }
}

// opj_dwt_encode_1_real, for n > 1: the four lifting steps and the scaling
void lift97(float* w, std::uint32_t n, bool even, std::uint32_t stride, std::uint32_t nl) noexcept
{
    const std::int32_t sn = static_cast<std::int32_t>((n + (even ? 1 : 0)) >> 1);
    const std::int32_t dn = static_cast<std::int32_t>(n) - sn;
    const std::uint32_t a = even ? 0 : 1;
    const std::uint32_t b = even ? 1 : 0;
    auto imin = [](std::int32_t x, std::int32_t y) { return x < y ? x : y; };
    step2(w, stride, nl, a, b, static_cast<std::uint32_t>(dn), static_cast<std::uint32_t>(imin(dn, sn - std::int32_t(b))),
          kAlpha);
    step2(w, stride, nl, b, a, static_cast<std::uint32_t>(sn), static_cast<std::uint32_t>(imin(sn, dn - std::int32_t(a))),
          kBeta);
    step2(w, stride, nl, a, b, static_cast<std::uint32_t>(dn), static_cast<std::uint32_t>(imin(dn, sn - std::int32_t(b))),
          kGamma);
    step2(w, stride, nl, b, a, static_cast<std::uint32_t>(sn), static_cast<std::uint32_t>(imin(sn, dn - std::int32_t(a))),
          kDelta);
    // the low-pass samples by 1/K, the high-pass ones by K
    for (std::int32_t i = 0; i < sn; ++i) {
        float* x = w + std::size_t(a + 2 * static_cast<std::uint32_t>(i)) * stride;
        for (std::uint32_t c = 0; c < nl; c++) {
            x[c] *= kInvK;
        }
    }
    for (std::int32_t i = 0; i < dn; ++i) {
        float* x = w + std::size_t(b + 2 * static_cast<std::uint32_t>(i)) * stride;
        for (std::uint32_t c = 0; c < nl; c++) {
            x[c] *= kK;
        }
    }
}

// The wavelet of one resolution step, 5-3 and 9-7 alike (the type decides the lifting).
template <class T>
void encode_level(T* data, std::uint32_t w, std::uint32_t rw, std::uint32_t rh, std::uint32_t rw1, std::uint32_t rh1,
                  T* tmp) noexcept
{
    constexpr bool is_float = std::is_same_v<T, float>;
    // OpenJPEG's cas_row and cas_col are the parities of the resolution's x0 and y0: zero, since
    // the encoder takes an image and a tile at the origin
    const bool even = true;

    // vertical pass, eight columns at a time
    {
        const std::uint32_t sn = rh1;
        for (std::uint32_t j = 0; j < rw; j += kLanes) {
            const std::uint32_t cols = rw - j < kLanes ? rw - j : kLanes;
            if (is_float && rh == 1) {
                continue;
            }
            for (std::uint32_t k = 0; k < rh; ++k) {
                for (std::uint32_t c = 0; c < cols; ++c) {
                    tmp[std::size_t(k) * kLanes + c] = data[std::size_t(k) * w + j + c];
                }
            }
            if constexpr (is_float) {
                lift97(tmp, rh, even, kLanes, cols);
            } else {
                lift53(tmp, rh, even, kLanes, cols);
            }
            // deinterleave: the low-pass lines first, then the high-pass ones
            for (std::uint32_t k = 0; k < rh; ++k) {
                const std::uint32_t src = k < sn ? (even ? 0 : 1) + 2 * k : (even ? 1 : 0) + 2 * (k - sn);
                for (std::uint32_t c = 0; c < cols; ++c) {
                    data[std::size_t(k) * w + j + c] = tmp[std::size_t(src) * kLanes + c];
                }
            }
        }
    }

    // horizontal pass, one row at a time
    for (std::uint32_t j = 0; j < rh; ++j) {
        T* row = data + std::size_t(j) * w;
        const std::uint32_t sn = rw1;
        if (rw == 1) {
            if constexpr (is_float) {
                continue;
            } else {
                if (!even) {
                    row[0] *= 2;
                }
                continue;
            }
        }
        std::memcpy(tmp, row, std::size_t(rw) * sizeof(T));
        if constexpr (is_float) {
            lift97(tmp, rw, even, 1, 1);
        } else {
            lift53(tmp, rw, even, 1, 1);
        }
        for (std::uint32_t k = 0; k < rw; ++k) {
            const std::uint32_t src = k < sn ? (even ? 0 : 1) + 2 * k : (even ? 1 : 0) + 2 * (k - sn);
            row[k] = tmp[src];
        }
    }
}

template <class T>
void encode_tile_comp(T* data, std::uint32_t w, const ResRect* res, std::uint32_t numres, void* scratch) noexcept
{
    T* tmp = static_cast<T*>(scratch);
    for (std::uint32_t l = numres - 1; l > 0; --l) {
        const ResRect& cur = res[l];
        const ResRect& last = res[l - 1];
        encode_level<T>(data, w, static_cast<std::uint32_t>(cur.x1 - cur.x0), static_cast<std::uint32_t>(cur.y1 - cur.y0),
                        static_cast<std::uint32_t>(last.x1 - last.x0), static_cast<std::uint32_t>(last.y1 - last.y0),
                        tmp);
    }
}

}  // namespace

const double kEncMctNorms[3] = {1.732, .8292, .8292};
const double kEncMctNormsReal[3] = {1.732, 1.805, 1.573};

double enc_dwt_norm(std::uint32_t level, std::uint32_t orient) noexcept
{
    /* the band-aid of OpenJPEG's opj_dwt_getnorm against an overflow of its table (issue 493) */
    if (orient == 0 && level >= 10) {
        level = 9;
    } else if (orient > 0 && level >= 9) {
        level = 8;
    }
    return kNorms[orient][level];
}

double enc_dwt_norm_real(std::uint32_t level, std::uint32_t orient) noexcept
{
    if (orient == 0 && level >= 10) {
        level = 9;
    } else if (orient > 0 && level >= 9) {
        level = 8;
    }
    return kNormsReal[orient][level];
}

void enc_resolution_rects(std::uint32_t w, std::uint32_t h, std::uint32_t numres, ResRect* out) noexcept
{
    for (std::uint32_t r = 0; r < numres; ++r) {
        const std::uint32_t level = numres - 1 - r;
        const std::uint64_t d = std::uint64_t(1) << level;
        out[r].x0 = 0;
        out[r].y0 = 0;
        out[r].x1 = static_cast<std::int32_t>((std::uint64_t(w) + d - 1) / d);
        out[r].y1 = static_cast<std::int32_t>((std::uint64_t(h) + d - 1) / d);
    }
}

std::size_t enc_dwt_scratch_bytes(const ResRect* res, std::uint32_t numres) noexcept
{
    std::uint32_t mr = 0;
    for (std::uint32_t r = 1; r < numres; ++r) {
        const std::uint32_t w = static_cast<std::uint32_t>(res[r].x1 - res[r].x0);
        const std::uint32_t h = static_cast<std::uint32_t>(res[r].y1 - res[r].y0);
        mr = mr < w ? w : mr;
        mr = mr < h ? h : mr;
    }
    return std::size_t(mr) * kLanes * sizeof(std::int32_t);
}

void enc_dwt_53(std::int32_t* data, std::uint32_t w, const ResRect* res, std::uint32_t numres, void* scratch) noexcept
{
    encode_tile_comp<std::int32_t>(data, w, res, numres, scratch);
}

void enc_dwt_97(float* data, std::uint32_t w, const ResRect* res, std::uint32_t numres, void* scratch) noexcept
{
    encode_tile_comp<float>(data, w, res, numres, scratch);
}

void enc_calc_stepsizes(std::uint32_t numres, std::uint32_t prec, bool reversible, EncStep* out) noexcept
{
    const std::uint32_t numbands = 3 * numres - 2;
    for (std::uint32_t bandno = 0; bandno < numbands; bandno++) {
        double stepsize;
        const std::uint32_t resno = (bandno == 0) ? 0 : ((bandno - 1) / 3 + 1);
        const std::uint32_t orient = (bandno == 0) ? 0 : ((bandno - 1) % 3 + 1);
        const std::uint32_t level = numres - 1 - resno;
        const std::uint32_t gain = !reversible ? 0 : ((orient == 0) ? 0 : (((orient == 1) || (orient == 2)) ? 1 : 2));
        if (reversible) {
            stepsize = 1.0;
        } else {
            const double norm = enc_dwt_norm_real(level, orient);
            stepsize = (1 << (gain)) / norm;
        }
        // opj_dwt_encode_stepsize
        const std::int32_t step = static_cast<std::int32_t>(std::floor(stepsize * 8192.0));
        const std::int32_t numbps = static_cast<std::int32_t>(prec + gain);
        const std::int32_t p = floorlog2(step) - 13;
        const std::int32_t n = 11 - floorlog2(step);
        out[bandno].mant = (n < 0 ? step >> -n : step << n) & 0x7ff;
        out[bandno].expn = numbps - p;
    }
}

void enc_rct(std::int32_t* c0, std::int32_t* c1, std::int32_t* c2, std::size_t n) noexcept
{
    for (std::size_t i = 0; i < n; ++i) {
        std::int32_t r = c0[i];
        std::int32_t g = c1[i];
        std::int32_t b = c2[i];
        std::int32_t y = (r + (g * 2) + b) >> 2;
        std::int32_t u = b - g;
        std::int32_t v = r - g;
        c0[i] = y;
        c1[i] = u;
        c2[i] = v;
    }
}

void enc_ict(float* c0, float* c1, float* c2, std::size_t n) noexcept
{
    for (std::size_t i = 0; i < n; ++i) {
        float r = c0[i];
        float g = c1[i];
        float b = c2[i];
        float y = 0.299f * r + 0.587f * g + 0.114f * b;
        float u = -0.16875f * r - 0.331260f * g + 0.5f * b;
        float v = 0.5f * r - 0.41869f * g - 0.08131f * b;
        c0[i] = y;
        c1[i] = u;
        c2[i] = v;
    }
}

}  // namespace nj2k::detail

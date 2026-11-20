// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The encoder (stage 6a, owner decision D9: byte-identical to OpenJPEG for good): OpenJPEG's encoder
// API (nj2k_create_compress, nj2k_setup_encoder, nj2k_start_compress, nj2k_encode, nj2k_end_compress)
// for the settings the Second Life viewer uses, and a refusal (NJ2K_E_UNSUPPORTED) of the rest, which
// belongs to the j2k-extra module. The main header and the tile-part follow j2k.c
// (opj_j2k_setup_encoder, the header writers, opj_j2k_update_rates, opj_j2k_encode) and tcd.c
// (opj_tcd_init_tile, opj_tcd_encode_tile); their notice:
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
#include "encode.h"

#include <cmath>
#include <climits>
#include <cstdarg>
#include <cstring>
#include <new>

#if defined(__SSE__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

#include "memory.h"

using nj2k::detail::Allocator;
using nj2k::detail::EncBand;
using nj2k::detail::EncCblk;
using nj2k::detail::EncComp;
using nj2k::detail::Encoder;
using nj2k::detail::EncParams;
using nj2k::detail::EncRes;
using nj2k::detail::EncTile;
using nj2k::detail::Messages;
using nj2k::detail::Stream;

namespace {

NJ2K_BOOL finish(Encoder* e, nj2k_status_t status) noexcept
{
    e->status = status;
    if (status < 0) {
        e->state = Encoder::State::failed;
    }
    return status >= 0 ? NJ2K_TRUE : NJ2K_FALSE;
}

NJ2K_BOOL fail(Encoder* e, nj2k_status_t status, const char* format, ...) noexcept NJ2K_PRINTF_LIKE(3, 4);

NJ2K_BOOL fail(Encoder* e, nj2k_status_t status, const char* format, ...) noexcept
{
    va_list args;
    va_start(args, format);
    e->messages.vemit(Messages::Error, format, args);
    va_end(args);
    return finish(e, status);
}

constexpr const char* kExtra = "the j2k-extra module";
// OpenJPEG's own comment when the caller gives none, kept so that the bytes are the same
constexpr const char* kDefaultComment = "Created by OpenJPEG version 2.5.4";

// A refusal is a mistake of the caller's, which it may fix and retry: the encoder stays where it was
// (refuse); only what the caller cannot mend (memory, the output stream, a tile that does not fit) ends
// the encoder (fail). The _st forms return the status, for the checks.
nj2k_status_t refuse_st(Encoder* e, nj2k_status_t status, const char* format, ...) noexcept NJ2K_PRINTF_LIKE(3, 4);

nj2k_status_t refuse_st(Encoder* e, nj2k_status_t status, const char* format, ...) noexcept
{
    va_list args;
    va_start(args, format);
    e->messages.vemit(Messages::Error, format, args);
    va_end(args);
    e->status = status;
    return status;
}

nj2k_status_t unsupported_st(Encoder* e, const char* who, const char* what) noexcept
{
    return refuse_st(e, NJ2K_E_UNSUPPORTED, "%s: %s is not supported by the core encoder (it belongs to %s)\n", who,
                     what, kExtra);
}

NJ2K_BOOL refuse(Encoder* e, nj2k_status_t status, const char* format, ...) noexcept NJ2K_PRINTF_LIKE(3, 4);

NJ2K_BOOL refuse(Encoder* e, nj2k_status_t status, const char* format, ...) noexcept
{
    va_list args;
    va_start(args, format);
    e->messages.vemit(Messages::Error, format, args);
    va_end(args);
    e->status = status;
    return NJ2K_FALSE;
}

NJ2K_BOOL unsupported(Encoder* e, const char* who, const char* what) noexcept
{
    unsupported_st(e, who, what);
    return NJ2K_FALSE;
}

// what a refused start_compress leaves behind: nothing, so that the caller can mend the image and call again
void discard_planes(Encoder* e) noexcept
{
    e->arena.release_all();
    for (auto& pl : e->planes) {
        pl = nullptr;
    }
}

inline void put16(std::uint8_t*& p, std::uint32_t v) noexcept
{
    *p++ = static_cast<std::uint8_t>(v >> 8);
    *p++ = static_cast<std::uint8_t>(v);
}

inline void put32(std::uint8_t*& p, std::uint32_t v) noexcept
{
    *p++ = static_cast<std::uint8_t>(v >> 24);
    *p++ = static_cast<std::uint8_t>(v >> 16);
    *p++ = static_cast<std::uint8_t>(v >> 8);
    *p++ = static_cast<std::uint8_t>(v);
}

// opj_lrintf: round to nearest even, as the oracle's x86-64 builds do
inline std::int32_t lrintf_even(float f) noexcept
{
#if defined(__SSE__) || defined(_M_X64)
    return _mm_cvtss_si32(_mm_set_ss(f));
#else
    return static_cast<std::int32_t>(std::lrintf(f));
#endif
}

constexpr std::int32_t kFracBits = 6;  // T1_NMSEDEC_FRACBITS

// The Second Life viewer's settings (llimagej2coj.cpp, JPEG2KEncode), which the core encoder takes: a J2K
// codestream of one tile at the origin, one tile-part; 8-bit unsigned components of the same size
// (1 to 8, the colour transform on the first three when there are three or more), RLCP, 64 x 64
// code-blocks, no precincts, no mode switches, no SOP, EPH, PLT or TLM; reversible (5/3) or
// irreversible (9/7); one or more quality layers by rate; the comment of the caller.
struct Settings {
    std::uint32_t numres;
    std::uint32_t numlayers;
    bool reversible;
    bool mct;
};

nj2k_status_t check_params(Encoder* e, const nj2k_cparameters_t* p, const nj2k_image_t* image, Settings& s) noexcept
{
    const char* who = "nj2k_setup_encoder";
    if (p->tile_size_on) {
        return unsupported_st(e, who, "tiling");
    }
    if (p->cp_tx0 != 0 || p->cp_ty0 != 0) {
        return unsupported_st(e, who, "a tile offset");
    }
    if (p->cp_fixed_alloc || p->cp_fixed_quality) {
        return unsupported_st(e, who, "fixed layer or fixed quality allocation");
    }
    if (p->csty != 0) {
        return unsupported_st(e, who, "SOP, EPH or precinct sizes");
    }
    if (p->prog_order != NJ2K_RLCP) {
        return unsupported_st(e, who, "a progression order other than RLCP");
    }
    if (p->numpocs != 0) {
        return unsupported_st(e, who, "progression order changes");
    }
    if (p->cblockw_init != 64 || p->cblockh_init != 64) {
        return unsupported_st(e, who, "code-blocks other than 64 x 64");
    }
    if (p->mode != 0) {
        return unsupported_st(e, who, "code-block mode switches");
    }
    if (p->roi_compno >= 0 && static_cast<std::uint32_t>(p->roi_compno) < image->numcomps && p->roi_shift != 0) {
        return unsupported_st(e, who, "a region of interest");
    }
    if (p->cp_cinema != NJ2K_OFF || p->cp_rsiz != NJ2K_STD_RSIZ || p->rsiz != NJ2K_PROFILE_NONE) {
        return unsupported_st(e, who, "a profile");
    }
    if (p->max_comp_size != 0) {
        return unsupported_st(e, who, "max_comp_size");
    }
    if (p->tp_on) {
        return unsupported_st(e, who, "several tile-parts");
    }
    if (p->mct_data) {
        return unsupported_st(e, who, "a custom colour transform");
    }
    if (p->tcp_mct != 0 && p->tcp_mct != 1) {
        return unsupported_st(e, who, "a colour transform other than none or the standard one");
    }
    if (p->tcp_mct == 1 && image->numcomps < 3) {
        return refuse_st(e, NJ2K_E_ARGUMENT, "%s: the colour transform needs three components\n", who);
    }
    if (p->numresolution <= 0 || p->numresolution > NJ2K_J2K_MAXRLVLS) {
        return refuse_st(e, NJ2K_E_ARGUMENT, "Invalid number of resolutions : %d not in range [1,%d]\n", p->numresolution,
                      NJ2K_J2K_MAXRLVLS);
    }
    if (p->tcp_numlayers < 0) {
        return refuse_st(e, NJ2K_E_ARGUMENT, "%s: negative number of layers\n", who);
    }
    if (static_cast<std::uint32_t>(p->tcp_numlayers) > Encoder::kMaxLayers) {
        return refuse_st(e, NJ2K_E_LIMIT, "%s: %d layers, at most %u\n", who, p->tcp_numlayers, Encoder::kMaxLayers);
    }
    if (p->cp_comment && std::strlen(p->cp_comment) > Encoder::kMaxComment) {
        return refuse_st(e, NJ2K_E_ARGUMENT, "%s: the comment is longer than a marker segment holds\n", who);
    }
    s.numres = static_cast<std::uint32_t>(p->numresolution);
    s.numlayers = static_cast<std::uint32_t>(p->tcp_numlayers);
    s.reversible = p->irreversible == 0;  // any other value is the 9/7, as in OpenJPEG
    s.mct = p->tcp_mct == 1;
    return NJ2K_OK;
}

nj2k_status_t check_image(Encoder* e, const nj2k_image_t* image, const char* who) noexcept
{
    if (!image || !image->comps || image->numcomps == 0) {
        return refuse_st(e, NJ2K_E_ARGUMENT, "%s: no image\n", who);
    }
    if (image->numcomps > Encoder::kMaxComponents) {
        return refuse_st(e, NJ2K_E_LIMIT, "%s: %u components, at most %u\n", who, image->numcomps,
                      Encoder::kMaxComponents);
    }
    if (image->x0 != 0 || image->y0 != 0) {
        return unsupported_st(e, who, "an image offset");
    }
    if (image->x1 == 0 || image->y1 == 0) {
        return refuse_st(e, NJ2K_E_ARGUMENT, "%s: an empty image\n", who);
    }
    if (image->x1 > Encoder::kMaxSide || image->y1 > Encoder::kMaxSide ||
        std::uint64_t(image->x1) * image->y1 * image->numcomps > Encoder::kMaxSamples) {
        return refuse_st(e, NJ2K_E_LIMIT, "%s: a %u x %u image of %u components is over the encoder's limits (%u per side, %llu samples)\n",
                      who, image->x1, image->y1, image->numcomps, Encoder::kMaxSide,
                      static_cast<unsigned long long>(Encoder::kMaxSamples));
    }
    for (std::uint32_t c = 0; c < image->numcomps; ++c) {
        const nj2k_image_comp_t& comp = image->comps[c];
        if (comp.dx != 1 || comp.dy != 1) {
            return unsupported_st(e, who, "subsampled components");
        }
        if (comp.x0 != 0 || comp.y0 != 0) {
            return unsupported_st(e, who, "a component offset");
        }
        if (comp.w != image->x1 || comp.h != image->y1) {
            return unsupported_st(e, who, "components of different sizes");
        }
        if (comp.prec != 8 || comp.sgnd != 0) {
            return unsupported_st(e, who, "components other than 8-bit unsigned");
        }
    }
    return NJ2K_OK;
}

// ---- the main header (opj_j2k_write_soc, siz, cod, qcd, com) ----

std::size_t header_size(const Encoder& e) noexcept
{
    const std::uint32_t numbands = 3 * e.params.numres - 2;
    const std::size_t qcd = 4 + (e.params.reversible ? 1 + numbands : 1 + 2 * numbands);
    return 2 + (40 + 3 * std::size_t(e.params.numcomps)) + 14 + qcd + (6 + e.comment_len);
}

void write_header(const Encoder& e, std::uint8_t* out) noexcept
{
    const EncParams& p = e.params;
    std::uint8_t* c = out;

    put16(c, 0xff4f);  // SOC

    put16(c, 0xff51);  // SIZ
    put16(c, 38 + 3 * p.numcomps);
    put16(c, 0);  // Rsiz
    put32(c, e.width);
    put32(c, e.height);
    put32(c, 0);  // X0siz
    put32(c, 0);  // Y0siz
    put32(c, e.width);   // XTsiz
    put32(c, e.height);  // YTsiz
    put32(c, 0);  // XT0siz
    put32(c, 0);  // YT0siz
    put16(c, p.numcomps);
    for (std::uint32_t i = 0; i < p.numcomps; ++i) {
        *c++ = static_cast<std::uint8_t>(p.prec - 1);  // Ssiz
        *c++ = 1;                                      // XRsiz
        *c++ = 1;                                      // YRsiz
    }

    put16(c, 0xff52);  // COD
    put16(c, 12);
    *c++ = 0;                                   // Scod
    *c++ = 1;                                   // SGcod (A): RLCP
    put16(c, p.numlayers);                      // SGcod (B)
    *c++ = static_cast<std::uint8_t>(p.mct);    // SGcod (C)
    *c++ = static_cast<std::uint8_t>(p.numres - 1);  // SPcod
    *c++ = 4;                                   // 64 wide
    *c++ = 4;                                   // 64 high
    *c++ = 0;                                   // no mode switches
    *c++ = p.reversible ? 1 : 0;                // the wavelet

    const std::uint32_t numbands = 3 * p.numres - 2;
    put16(c, 0xff5c);  // QCD
    if (p.reversible) {
        put16(c, 4 + 1 + numbands - 2);
        *c++ = static_cast<std::uint8_t>(0 + (p.numgbits << 5));  // no quantization
        for (std::uint32_t b = 0; b < numbands; ++b) {
            *c++ = static_cast<std::uint8_t>(static_cast<std::uint32_t>(p.steps[b].expn) << 3);
        }
    } else {
        put16(c, 4 + 1 + 2 * numbands - 2);
        *c++ = static_cast<std::uint8_t>(2 + (p.numgbits << 5));  // scalar expounded
        for (std::uint32_t b = 0; b < numbands; ++b) {
            put16(c, (static_cast<std::uint32_t>(p.steps[b].expn) << 11) + static_cast<std::uint32_t>(p.steps[b].mant));
        }
    }

    put16(c, 0xff64);  // COM
    put16(c, static_cast<std::uint32_t>(e.comment_len + 4));
    put16(c, 1);  // general use (IS 8859-15:1999 (Latin) values)
    std::memcpy(c, e.comment, e.comment_len);
}

// opj_j2k_update_rates: the layers' rates in bytes of the tile from the compression ratios asked
void update_rates(Encoder& e, std::uint64_t main_header_bytes) noexcept
{
    EncParams& p = e.params;
    const std::uint32_t l_bits_empty = 8 * 1 * 1;
    const std::uint32_t l_size_pixel = p.numcomps * p.prec;
    const float l_sot_remove = static_cast<float>(main_header_bytes) / static_cast<float>(1u);
    const float l_offset = static_cast<float>(0.0f) / static_cast<float>(p.numlayers);
    const std::int32_t l_x0 = 0, l_y0 = 0;
    const std::int32_t l_x1 = static_cast<std::int32_t>(e.width), l_y1 = static_cast<std::int32_t>(e.height);
    float* l_rates = p.rates;

    for (std::uint32_t k = 0; k < p.numlayers; ++k) {
        if (*l_rates > 0.0f) {
            *l_rates = static_cast<float>((static_cast<double>(l_size_pixel) * static_cast<std::uint32_t>(l_x1 - l_x0) *
                                           static_cast<std::uint32_t>(l_y1 - l_y0)) /
                                          ((*l_rates) * static_cast<float>(l_bits_empty))) -
                       l_offset;
        }
        ++l_rates;
    }

    l_rates = p.rates;
    if (*l_rates > 0.0f) {
        *l_rates -= l_sot_remove;
        if (*l_rates < 30.0f) {
            *l_rates = 30.0f;
        }
    }
    ++l_rates;

    const std::uint32_t l_last_res = p.numlayers - 1;
    for (std::uint32_t k = 1; k < l_last_res; ++k) {
        if (*l_rates > 0.0f) {
            *l_rates -= l_sot_remove;
            if (*l_rates < *(l_rates - 1) + 10.0f) {
                *l_rates = (*(l_rates - 1)) + 20.0f;
            }
        }
        ++l_rates;
    }

    if (*l_rates > 0.0f) {
        *l_rates -= (l_sot_remove + 2.f);
        if (*l_rates < *(l_rates - 1) + 10.0f) {
            *l_rates = (*(l_rates - 1)) + 20.0f;
        }
    }
}

// The tile buffer's size: OpenJPEG's guess of the codestream (opj_j2k_update_rates), which the rate
// allocation and Tier-2 are given as their room.
std::uint32_t tile_buffer_size(const Encoder& e) noexcept
{
    const EncParams& p = e.params;
    std::uint64_t l_tile_size = 0;
    for (std::uint32_t i = 0; i < p.numcomps; ++i) {
        l_tile_size += std::uint64_t(e.width) * e.height * p.prec;
    }
    l_tile_size = static_cast<std::uint64_t>(static_cast<double>(l_tile_size) * 1.4 / 8);
    l_tile_size += 500;
    // opj_j2k_get_specific_header_sizes: the tile-part's SOT (12), the COC and QCC room of the
    // other components (11 + 11 each) and a POC marker (13)
    l_tile_size += 12 + std::uint64_t(p.numcomps - 1) * (11 + 11) + 13;
    return static_cast<std::uint32_t>(l_tile_size);
}

// ---- the tile (opj_tcd_init_tile for an encoder, and the stages of opj_tcd_encode_tile) ----

inline std::int32_t ceildivpow2(std::int32_t a, std::int32_t b) noexcept
{
    return static_cast<std::int32_t>((std::int64_t(a) + (std::int64_t(1) << b) - 1) >> b);
}

inline std::int32_t int64_ceildivpow2(std::int64_t a, std::int32_t b) noexcept
{
    return static_cast<std::int32_t>((a + (std::int64_t(1) << b) - 1) >> b);
}

bool build_tile(Encoder& e, EncTile& tile) noexcept
{
    const EncParams& p = e.params;
    nj2k::detail::Arena& arena = e.arena;
    tile.numcomps = p.numcomps;
    tile.comps = arena.make_array<EncComp>(p.numcomps);
    if (!tile.comps) {
        return false;
    }
    for (std::uint32_t compno = 0; compno < p.numcomps; ++compno) {
        EncComp& tilec = tile.comps[compno];
        tilec.x0 = 0;
        tilec.y0 = 0;
        tilec.x1 = static_cast<std::int32_t>(e.width);
        tilec.y1 = static_cast<std::int32_t>(e.height);
        tilec.numres = p.numres;
        tilec.data = e.planes[compno];
        tilec.res = arena.make_array<EncRes>(p.numres);
        if (!tilec.res) {
            return false;
        }
        std::uint32_t step_index = 0;
        std::int32_t level_no = static_cast<std::int32_t>(p.numres);
        for (std::uint32_t resno = 0; resno < p.numres; ++resno) {
            EncRes& res = tilec.res[resno];
            --level_no;
            res.x0 = ceildivpow2(tilec.x0, level_no);
            res.y0 = ceildivpow2(tilec.y0, level_no);
            res.x1 = ceildivpow2(tilec.x1, level_no);
            res.y1 = ceildivpow2(tilec.y1, level_no);
            res.numbands = resno == 0 ? 1 : 3;
            // every precinct is 2^15 wide and high (2^14 in the bands of a resolution above the
            // first), which holds every side the encoder takes: one precinct a resolution
            for (std::uint32_t bandno = 0; bandno < res.numbands; ++bandno, ++step_index) {
                EncBand& band = res.bands[bandno];
                if (resno == 0) {
                    band.bandno = 0;
                    band.x0 = ceildivpow2(tilec.x0, level_no);
                    band.y0 = ceildivpow2(tilec.y0, level_no);
                    band.x1 = ceildivpow2(tilec.x1, level_no);
                    band.y1 = ceildivpow2(tilec.y1, level_no);
                } else {
                    band.bandno = bandno + 1;
                    const std::int32_t x0b = static_cast<std::int32_t>(band.bandno & 1);
                    const std::int32_t y0b = static_cast<std::int32_t>(band.bandno >> 1);
                    band.x0 = int64_ceildivpow2(tilec.x0 - (std::int64_t(x0b) << level_no), level_no + 1);
                    band.y0 = int64_ceildivpow2(tilec.y0 - (std::int64_t(y0b) << level_no), level_no + 1);
                    band.x1 = int64_ceildivpow2(tilec.x1 - (std::int64_t(x0b) << level_no), level_no + 1);
                    band.y1 = int64_ceildivpow2(tilec.y1 - (std::int64_t(y0b) << level_no), level_no + 1);
                }
                if (band.empty()) {
                    continue;
                }
                {
                    // Table E-1 - Sub-band gains; Equation E-4; Delta_b of Equation E-3
                    const std::int32_t log2_gain = (band.bandno == 0) ? 0 : (band.bandno == 3) ? 2 : 1;
                    const std::int32_t Rb = static_cast<std::int32_t>(p.prec) + log2_gain;
                    const nj2k::detail::EncStep step = p.steps[step_index];
                    band.stepsize = static_cast<float>(
                        ((1.0 + step.mant / 2048.0) * std::ldexp(1.0, static_cast<int>(Rb - step.expn))));
                    // Mb of Equation E-2
                    band.numbps = step.expn + static_cast<std::int32_t>(p.numgbits) - 1;
                }
                // the one precinct: its code-blocks, from the band's origin
                band.cw = static_cast<std::uint32_t>(ceildivpow2(band.x1, 6));
                band.ch = static_cast<std::uint32_t>(ceildivpow2(band.y1, 6));
                const std::uint32_t n = band.cw * band.ch;
                band.cblks = arena.make_array<EncCblk>(n);
                if (!band.cblks || !nj2k::detail::enc_tgt_create(arena, band.incltree, band.cw, band.ch) ||
                    !nj2k::detail::enc_tgt_create(arena, band.imsbtree, band.cw, band.ch)) {
                    return false;
                }
                for (std::uint32_t cblkno = 0; cblkno < n; ++cblkno) {
                    EncCblk& cb = band.cblks[cblkno];
                    const std::int32_t cx = static_cast<std::int32_t>(cblkno % band.cw) * 64;
                    const std::int32_t cy = static_cast<std::int32_t>(cblkno / band.cw) * 64;
                    cb.x0 = cx;
                    cb.y0 = cy;
                    cb.x1 = cx + 64 < band.x1 ? cx + 64 : band.x1;
                    cb.y1 = cy + 64 < band.y1 ? cy + 64 : band.y1;
                    cb.layers = arena.make_array<nj2k::detail::EncLayer>(p.numlayers);
                    if (!cb.layers) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

using nj2k::detail::ResRect;

// Jobs in order on the calling thread; the host's executor takes their place in stage 6b.
void run_jobs(std::uint32_t count, void (*job)(void* ctx, std::uint32_t index), void* ctx) noexcept
{
    for (std::uint32_t i = 0; i < count; ++i) {
        job(ctx, i);
    }
}

struct DwtJobs {
    Encoder* e;
    const ResRect* rects;
    std::uint8_t* scratch;
    std::size_t slice;
};

void dwt_job(void* ctx, std::uint32_t compno) noexcept
{
    auto* j = static_cast<DwtJobs*>(ctx);
    const EncParams& p = j->e->params;
    void* scratch = j->scratch ? j->scratch + j->slice * compno : nullptr;
    if (p.reversible) {
        nj2k::detail::enc_dwt_53(static_cast<std::int32_t*>(j->e->planes[compno]), j->e->width, j->rects, p.numres,
                                 scratch);
    } else {
        nj2k::detail::enc_dwt_97(static_cast<float*>(j->e->planes[compno]), j->e->width, j->rects, p.numres, scratch);
    }
}

struct T1Job {
    std::uint32_t compno, resno, bandno, cblkno;
};

struct T1Jobs {
    Encoder* e;
    EncTile* tile;
    T1Job* list;
    nj2k::detail::EncT1* t1;
    const double* mct_norms;
    std::uint32_t mct_numcomps;
    nj2k_status_t status;
};

// one code-block: its samples in the order of the coding passes, then its passes (opj_t1_cblk_encode_processor)
void t1_job(void* ctx, std::uint32_t index) noexcept
{
    using namespace nj2k::detail;
    auto* j = static_cast<T1Jobs*>(ctx);
    if (j->status != NJ2K_OK) {
        return;
    }
    const Encoder& e = *j->e;
    const EncParams& p = e.params;
    const T1Job& job = j->list[index];
    EncComp& tilec = j->tile->comps[job.compno];
    EncRes& res = tilec.res[job.resno];
    EncBand& band = res.bands[job.bandno];
    EncCblk& cblk = band.cblks[job.cblkno];
    const std::uint32_t tile_w = e.width;
    std::int32_t x = cblk.x0 - band.x0;
    std::int32_t y = cblk.y0 - band.y0;
    if (band.bandno & 1) {
        const EncRes& pres = tilec.res[job.resno - 1];
        x += pres.x1 - pres.x0;
    }
    if (band.bandno & 2) {
        const EncRes& pres = tilec.res[job.resno - 1];
        y += pres.y1 - pres.y0;
    }
    const std::uint32_t cblk_w = static_cast<std::uint32_t>(cblk.x1 - cblk.x0);
    const std::uint32_t cblk_h = static_cast<std::uint32_t>(cblk.y1 - cblk.y0);
    std::int32_t* t1data = enc_t1_data(j->t1);
    std::uint32_t i, k;
    std::uint32_t row;
    if (p.reversible) {
        // change from "natural" order to "zigzag" order of T1 passes; the shift on the unsigned
        // type, as OpenJPEG does
        const std::uint32_t* tiledp_u =
            static_cast<const std::uint32_t*>(tilec.data) + std::size_t(y) * tile_w + static_cast<std::size_t>(x);
        for (row = 0; row < (cblk_h & ~3U); row += 4) {
            for (i = 0; i < cblk_w; ++i) {
                t1data[0] = static_cast<std::int32_t>(tiledp_u[(row + 0) * tile_w + i] << kFracBits);
                t1data[1] = static_cast<std::int32_t>(tiledp_u[(row + 1) * tile_w + i] << kFracBits);
                t1data[2] = static_cast<std::int32_t>(tiledp_u[(row + 2) * tile_w + i] << kFracBits);
                t1data[3] = static_cast<std::int32_t>(tiledp_u[(row + 3) * tile_w + i] << kFracBits);
                t1data += 4;
            }
        }
        if (row < cblk_h) {
            for (i = 0; i < cblk_w; ++i) {
                for (k = row; k < cblk_h; k++) {
                    t1data[0] = static_cast<std::int32_t>(tiledp_u[k * tile_w + i] << kFracBits);
                    t1data++;
                }
            }
        }
    } else {
        const float* tiledp_f =
            static_cast<const float*>(tilec.data) + std::size_t(y) * tile_w + static_cast<std::size_t>(x);
        for (row = 0; row < (cblk_h & ~3U); row += 4) {
            for (i = 0; i < cblk_w; ++i) {
                t1data[0] = lrintf_even((tiledp_f[(row + 0) * tile_w + i] / band.stepsize) * (1 << kFracBits));
                t1data[1] = lrintf_even((tiledp_f[(row + 1) * tile_w + i] / band.stepsize) * (1 << kFracBits));
                t1data[2] = lrintf_even((tiledp_f[(row + 2) * tile_w + i] / band.stepsize) * (1 << kFracBits));
                t1data[3] = lrintf_even((tiledp_f[(row + 3) * tile_w + i] / band.stepsize) * (1 << kFracBits));
                t1data += 4;
            }
        }
        if (row < cblk_h) {
            for (i = 0; i < cblk_w; ++i) {
                for (k = row; k < cblk_h; k++) {
                    t1data[0] = lrintf_even((tiledp_f[k * tile_w + i] / band.stepsize) * (1 << kFracBits));
                    t1data++;
                }
            }
        }
    }
    const bool coded = enc_t1_encode(j->t1, j->e->arena, cblk, band.bandno, job.compno, tilec.numres - 1 - job.resno,
                                        p.reversible, static_cast<double>(band.stepsize), j->mct_norms, j->mct_numcomps);
    if (!coded) {
        j->status = j->e->arena.failure() == nj2k::detail::Arena::Failure::none ? NJ2K_E_LIMIT : NJ2K_E_NO_MEMORY;
        return;
    }
}

// Everything of the tile but the stream: the colour and wavelet transforms, Tier-1, the rate
// allocation and Tier-2, into e.tile_buf (SOT, SOD and the packets); *total is the tile-part's size.
nj2k_status_t encode_tile(Encoder& e, std::uint32_t* total) noexcept
{
    using namespace nj2k::detail;
    const EncParams& p = e.params;
    Arena& arena = e.arena;
    const std::size_t n = std::size_t(e.width) * e.height;

    EncTile tile;
    if (!build_tile(e, tile)) {
        return NJ2K_E_NO_MEMORY;
    }

    // the DC level shift (opj_tcd_dc_level_shift_encode), to floats for the 9/7 transform
    for (std::uint32_t compno = 0; compno < p.numcomps; ++compno) {
        std::uint32_t* d = static_cast<std::uint32_t*>(e.planes[compno]);
        if (p.reversible) {
            for (std::size_t i = 0; i < n; ++i) {
                d[i] -= p.dc_shift;
            }
        } else {
            for (std::size_t i = 0; i < n; ++i) {
                const float f = static_cast<float>(static_cast<std::int32_t>(d[i] - p.dc_shift));
                std::memcpy(&d[i], &f, sizeof(float));
            }
        }
    }

    // the colour transform of the first three components
    if (p.mct) {
        if (p.reversible) {
            enc_rct(static_cast<std::int32_t*>(e.planes[0]), static_cast<std::int32_t*>(e.planes[1]),
                    static_cast<std::int32_t*>(e.planes[2]), n);
        } else {
            enc_ict(static_cast<float*>(e.planes[0]), static_cast<float*>(e.planes[1]),
                    static_cast<float*>(e.planes[2]), n);
        }
    }

    // The stages that are independent per component (the wavelet) and per code-block (Tier-1) are
    // jobs, run in order on the calling thread by run_jobs(): the shape stage 6b gives to a host's
    // executor (stage 5's parallel_for over an index). They are not thread-safe yet: the Tier-1
    // scratch, the arena Tier-1 commits to and the jobs' status are shared (doc/api.md, 6b's to-do).
    ResRect* rects = arena.make_array<ResRect>(p.numres);
    if (!rects) {
        return NJ2K_E_NO_MEMORY;
    }
    enc_resolution_rects(e.width, e.height, p.numres, rects);

    // the forward wavelet, one job a component
    {
        const std::size_t bytes = enc_dwt_scratch_bytes(rects, p.numres);
        const std::size_t slice = (bytes + 63) & ~std::size_t(63);
        std::uint8_t* scratch = slice ? static_cast<std::uint8_t*>(arena.allocate(slice * p.numcomps, 64)) : nullptr;
        if (slice && !scratch) {
            return NJ2K_E_NO_MEMORY;
        }
        DwtJobs jobs = {&e, rects, scratch, slice};
        run_jobs(p.numcomps, dwt_job, &jobs);
    }

    // Tier-1, one job a code-block (opj_tcd_t1_encode)
    {
        std::uint32_t count = 0;
        for (std::uint32_t compno = 0; compno < p.numcomps; ++compno) {
            const EncComp& tilec = tile.comps[compno];
            for (std::uint32_t resno = 0; resno < tilec.numres; ++resno) {
                for (std::uint32_t bandno = 0; bandno < tilec.res[resno].numbands; ++bandno) {
                    const EncBand& band = tilec.res[resno].bands[bandno];
                    count += band.empty() ? 0 : band.cw * band.ch;
                }
            }
        }
        T1Jobs jobs = {};
        jobs.e = &e;
        jobs.tile = &tile;
        jobs.list = arena.make_array<T1Job>(count);
        jobs.t1 = enc_t1_create(e.allocator);
        if ((count && !jobs.list) || !jobs.t1) {
            enc_t1_destroy(jobs.t1, e.allocator);
            return NJ2K_E_NO_MEMORY;
        }
        if (p.mct) {
            jobs.mct_numcomps = 3U;
            jobs.mct_norms = p.reversible ? kEncMctNorms : kEncMctNormsReal;
        } else {
            jobs.mct_numcomps = p.numcomps;
        }
        std::uint32_t at = 0;
        for (std::uint32_t compno = 0; compno < p.numcomps; ++compno) {
            EncComp& tilec = tile.comps[compno];
            for (std::uint32_t resno = 0; resno < tilec.numres; ++resno) {
                for (std::uint32_t bandno = 0; bandno < tilec.res[resno].numbands; ++bandno) {
                    EncBand& band = tilec.res[resno].bands[bandno];
                    if (band.empty()) {
                        continue;
                    }
                    for (std::uint32_t cblkno = 0; cblkno < band.cw * band.ch; ++cblkno) {
                        jobs.list[at++] = {compno, resno, bandno, cblkno};
                    }
                }
            }
        }
        run_jobs(count, t1_job, &jobs);
        enc_t1_destroy(jobs.t1, e.allocator);
        if (jobs.status != NJ2K_OK) {
            return jobs.status;
        }
    }

    // the rate allocation and Tier-2, behind the tile-part's SOT and SOD
    std::uint8_t* const buf = e.tile_buf;
    const std::uint32_t room = e.tile_size - 12 - 4;
    std::uint32_t written = 0;
    if (!enc_rate_allocate_and_packets(e.messages, tile, p, buf + 12 + 2, room, &written)) {
        e.messages.emit(Messages::Error, "Cannot encode tile\n");
        return NJ2K_E_LIMIT;
    }
    {
        std::uint8_t* c = buf;
        put16(c, 0xff90);  // SOT
        put16(c, 10);      // Lsot
        put16(c, 0);       // Isot
        put32(c, 12 + 2 + written);  // Psot
        *c++ = 0;          // TPsot
        *c++ = 1;          // TNsot
        put16(c, 0xff93);  // SOD
    }
    *total = 12 + 2 + written;
    return NJ2K_OK;
}

}  // namespace

namespace nj2k::detail {

void Encoder::release_all() noexcept
{
    arena.release_all();
    tile_buf = nullptr;
    for (auto& pl : planes) {
        pl = nullptr;
    }
    if (comment) {
        allocator.release(comment, comment_len + 1);
        comment = nullptr;
    }
}

}  // namespace nj2k::detail

extern "C" {

NJ2K_API nj2k_codec_t* NJ2K_CALLCONV nj2k_create_compress(NJ2K_CODEC_FORMAT format)
{
    if (format != NJ2K_CODEC_J2K) {
        // JP2 and JPT output belong to the j2k-codestream module
        return nullptr;
    }
    const Allocator allocator = Allocator::system();
    void* mem = allocator.allocate(sizeof(Encoder), alignof(Encoder) < 16 ? 16 : alignof(Encoder));
    if (!mem) {
        return nullptr;
    }
    auto* e = new (mem) Encoder();
    e->allocator = allocator;
    e->arena.init(&e->allocator);
    return reinterpret_cast<nj2k_codec_t*>(e);
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_setup_encoder(nj2k_codec_t* p_codec, nj2k_cparameters_t* parameters,
                                                    nj2k_image_t* image)
{
    Encoder* e = Encoder::from(p_codec);
    if (!e) {
        return NJ2K_FALSE;
    }
    if (!parameters || !image) {
        return refuse(e, NJ2K_E_ARGUMENT, "nj2k_setup_encoder: no parameters or no image\n");
    }
    if (e->state != Encoder::State::created) {
        return refuse(e, NJ2K_E_STATE, "nj2k_setup_encoder: call it once, on a new encoder\n");
    }
    nj2k_status_t st = check_image(e, image, "nj2k_setup_encoder");
    if (st != NJ2K_OK) {
        return NJ2K_FALSE;
    }
    Settings s = {};
    st = check_params(e, parameters, image, s);
    if (st != NJ2K_OK) {
        return NJ2K_FALSE;
    }

    // opj_j2k_setup_encoder: no explicit layers means one lossless layer, written back into the caller's
    // parameters, as is the codestream size the last rate gives (which no marker or rate reads)
    if (parameters->tcp_numlayers == 0) {
        parameters->tcp_numlayers = 1;
        parameters->cp_disto_alloc = 1;
        parameters->tcp_rates[0] = 0;
        s.numlayers = 1;
    }
    if (parameters->max_cs_size <= 0) {
        if (parameters->tcp_rates[parameters->tcp_numlayers - 1] > 0) {
            float temp_size;
            temp_size = static_cast<float>((static_cast<double>(image->numcomps) * image->comps[0].w *
                                            image->comps[0].h * image->comps[0].prec) /
                                           (static_cast<double>(parameters->tcp_rates[parameters->tcp_numlayers - 1]) *
                                            8 * image->comps[0].dx * image->comps[0].dy));
            if (temp_size >= static_cast<float>(INT_MAX)  /* 2^31: the cast would overflow */) {
                parameters->max_cs_size = INT_MAX;
            } else {
                parameters->max_cs_size = static_cast<int>(std::floor(temp_size));
            }
        } else {
            parameters->max_cs_size = 0;
        }
    } else {
        // a codestream size the caller asks for (or the one an earlier setup wrote back into the same
        // parameters) lifts the layers' rates up to the ratio it implies
        const float temp_rate = static_cast<float>((static_cast<double>(image->numcomps) * image->comps[0].w *
                                                    image->comps[0].h * image->comps[0].prec) /
                                                   ((static_cast<double>(parameters->max_cs_size)) * 8 *
                                                    image->comps[0].dx * image->comps[0].dy));
        bool cap = false;
        for (int i = 0; i < parameters->tcp_numlayers; i++) {
            if (parameters->tcp_rates[i] < temp_rate) {
                parameters->tcp_rates[i] = temp_rate;
                cap = true;
            }
        }
        if (cap) {
            e->messages.emit(Messages::Warning,
                             "The desired maximum codestream size has limited\n"
                             "at least one of the desired quality layers\n");
        }
    }

    EncParams& p = e->params;
    p = EncParams();
    p.numcomps = image->numcomps;
    p.prec = 8;
    p.numres = s.numres;
    p.numlayers = s.numlayers;
    p.numgbits = 2;
    p.reversible = s.reversible;
    p.mct = s.mct ? 1 : 0;
    p.dc_shift = 1u << (p.prec - 1);
    for (std::uint32_t j = 0; j < p.numlayers; ++j) {
        p.rates[j] = parameters->tcp_rates[j];
        if (p.rates[j] <= 1.0) {
            p.rates[j] = 0.0;  // force lossless
        }
    }
    nj2k::detail::enc_calc_stepsizes(p.numres, p.prec, p.reversible, p.steps);
    e->width = image->x1;
    e->height = image->y1;

    const char* text = parameters->cp_comment;
    if (!text) {
        text = kDefaultComment;
    }
    e->comment_len = std::strlen(text);
    e->comment = static_cast<char*>(e->allocator.allocate(e->comment_len + 1, 16));
    if (!e->comment) {
        return fail(e, NJ2K_E_NO_MEMORY, "Not enough memory to allocate copy of comment string\n");
    }
    std::memcpy(e->comment, text, e->comment_len + 1);

    e->state = Encoder::State::setup;
    return finish(e, NJ2K_OK);
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_encoder_set_extra_options(nj2k_codec_t* p_codec, const char* const* p_options)
{
    Encoder* e = Encoder::from(p_codec);
    if (!e) {
        return NJ2K_FALSE;
    }
    if (e->state != Encoder::State::setup) {
        return refuse(e, NJ2K_E_STATE, "nj2k_encoder_set_extra_options: call it after nj2k_setup_encoder\n");
    }
    if (!p_options) {
        return finish(e, NJ2K_OK);
    }
    for (const char* const* it = p_options; *it != nullptr; ++it) {
        const bool plt_tlm = std::strncmp(*it, "PLT=", 4) == 0 || std::strncmp(*it, "TLM=", 4) == 0;
        if (std::strcmp(*it, "PLT=NO") == 0 || std::strcmp(*it, "TLM=NO") == 0 || std::strcmp(*it, "GUARD_BITS=2") == 0) {
            continue;  // what the encoder does anyway
        }
        if (plt_tlm && (std::strcmp(*it + 4, "YES") == 0)) {
            return unsupported(e, "nj2k_encoder_set_extra_options", *it);
        }
        if (std::strncmp(*it, "GUARD_BITS=", 11) == 0) {
            const int bits = std::atoi(*it + 11);
            if (bits >= 0 && bits <= 7) {
                return unsupported(e, "nj2k_encoder_set_extra_options", *it);  // other guard bits: j2k-extra
            }
        }
        return refuse(e, NJ2K_E_ARGUMENT, "Invalid option: %s.\n", *it);
    }
    return finish(e, NJ2K_OK);
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_start_compress(nj2k_codec_t* p_codec, nj2k_image_t* image, nj2k_stream_t* p_stream)
{
    Encoder* e = Encoder::from(p_codec);
    if (!e) {
        return NJ2K_FALSE;
    }
    Stream* s = Stream::from(p_stream);
    if (!s || s->input || !s->write_fn || !image) {
        return refuse(e, NJ2K_E_ARGUMENT, "nj2k_start_compress: no image or no output stream\n");
    }
    if (e->state != Encoder::State::setup) {
        return refuse(e, NJ2K_E_STATE, "nj2k_start_compress: call it after nj2k_setup_encoder\n");
    }
    if (check_image(e, image, "nj2k_start_compress") != NJ2K_OK) {
        return NJ2K_FALSE;
    }
    const EncParams& p = e->params;
    if (image->numcomps != p.numcomps || image->x1 != e->width || image->y1 != e->height) {
        return refuse(e, NJ2K_E_ARGUMENT, "nj2k_start_compress: the image is not the one nj2k_setup_encoder took\n");
    }

    // opj_j2k_encoding_validation: the resolutions must fit the tile
    if (p.numres > 32 || e->width < (std::uint64_t(1) << (p.numres - 1)) ||
        e->height < (std::uint64_t(1) << (p.numres - 1))) {
        return fail(e, NJ2K_E_ARGUMENT, "Number of resolutions is too high in comparison to the size of tiles\n");
    }

    // the samples, copied (OpenJPEG takes the image's planes; the caller keeps its own here)
    const std::size_t n = std::size_t(e->width) * e->height;
    for (std::uint32_t c = 0; c < p.numcomps; ++c) {
        const std::int32_t* src = image->comps[c].data;
        if (!src) {
            discard_planes(e);
            return refuse(e, NJ2K_E_ARGUMENT, "nj2k_start_compress: component %u has no samples\n", c);
        }
        std::uint32_t* dst = static_cast<std::uint32_t*>(e->arena.allocate(n * sizeof(std::uint32_t), 64));
        if (!dst) {
            discard_planes(e);
            return fail(e, NJ2K_E_NO_MEMORY, "Not enough memory to copy the samples\n");
        }
        // 8-bit unsigned samples: OpenJPEG leaves others to arithmetic that wraps (DP-16); one test a plane
        std::uint32_t over = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t v = static_cast<std::uint32_t>(src[i]);
            over |= v;
            dst[i] = v;
        }
        if (over > 255u) {
            discard_planes(e);
            return refuse(e, NJ2K_E_ARGUMENT, "nj2k_start_compress: a sample of component %u is not in [0, 255]\n", c);
        }
        e->planes[c] = dst;
    }

    // the main header
    const std::size_t hsize = header_size(*e);
    std::uint8_t* header = static_cast<std::uint8_t*>(e->arena.allocate(hsize, 16));
    if (!header) {
        return fail(e, NJ2K_E_NO_MEMORY, "Not enough memory for the main header\n");
    }
    write_header(*e, header);
    if (!s->write(header, hsize)) {
        return fail(e, NJ2K_E_OUTPUT, "nj2k_start_compress: the output stream refused the main header\n");
    }

    update_rates(*e, s->offset);

    e->tile_size = tile_buffer_size(*e);
    e->tile_buf = static_cast<std::uint8_t*>(e->arena.allocate(e->tile_size, 16));
    if (!e->tile_buf) {
        return fail(e, NJ2K_E_NO_MEMORY, "Not enough memory to allocate the encoded tile data. %u MB required\n",
                    e->tile_size / 1024 / 1024);
    }
    e->state = Encoder::State::started;
    return finish(e, NJ2K_OK);
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_encode(nj2k_codec_t* p_codec, nj2k_stream_t* p_stream)
{
    Encoder* e = Encoder::from(p_codec);
    if (!e) {
        return NJ2K_FALSE;
    }
    Stream* s = Stream::from(p_stream);
    if (!s || s->input || !s->write_fn) {
        return refuse(e, NJ2K_E_ARGUMENT, "nj2k_encode: no output stream\n");
    }
    if (e->state != Encoder::State::started) {
        return refuse(e, NJ2K_E_STATE, "nj2k_encode: call it after nj2k_start_compress, once\n");
    }
    std::uint32_t total = 0;
    const nj2k_status_t st = encode_tile(*e, &total);
    if (st != NJ2K_OK) {
        return fail(e, st, "nj2k_encode: the tile could not be encoded\n");
    }
    if (!s->write(e->tile_buf, total)) {
        return fail(e, NJ2K_E_OUTPUT, "nj2k_encode: the output stream refused the tile\n");
    }
    // the tile's memory goes back; the encode is done but for the end of the codestream
    e->arena.release_all();
    e->tile_buf = nullptr;
    for (auto& pl : e->planes) {
        pl = nullptr;
    }
    e->state = Encoder::State::encoded;
    return finish(e, NJ2K_OK);
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_end_compress(nj2k_codec_t* p_codec, nj2k_stream_t* p_stream)
{
    Encoder* e = Encoder::from(p_codec);
    if (!e) {
        return NJ2K_FALSE;
    }
    Stream* s = Stream::from(p_stream);
    if (!s || s->input || !s->write_fn) {
        return refuse(e, NJ2K_E_ARGUMENT, "nj2k_end_compress: no output stream\n");
    }
    if (e->state != Encoder::State::encoded) {
        return refuse(e, NJ2K_E_STATE, "nj2k_end_compress: call it after nj2k_encode\n");
    }
    static const std::uint8_t eoc[2] = {0xff, 0xd9};
    if (!s->write(eoc, 2) || !s->flush()) {
        return fail(e, NJ2K_E_OUTPUT, "nj2k_end_compress: the output stream refused the end of the codestream\n");
    }
    e->state = Encoder::State::finished;
    return finish(e, NJ2K_OK);
}

}  // extern "C"

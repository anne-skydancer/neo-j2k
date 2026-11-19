// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// A tile's layout and its samples, after OpenJPEG 2.5.4's tcd.c: opj_tcd_init_tile for decoding
// (the geometry of every tile-component, resolution, band, precinct and code-block, the step sizes
// and the checks that refuse a tile) and opj_tcd_decode_tile after Tier-2 (opj_t1_decode_cblks for
// a whole tile, opj_tcd_dwt_decode, opj_tcd_mct_decode, opj_tcd_dc_level_shift_decode), on flat
// arrays in the work arena. Code-blocks without data are not run through Tier-1: their samples
// are written as zeros, which is what Tier-1 gives them, after OpenJPEG's bit-plane check.
// OpenJPEG's notice:
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
#include "tile.h"

#include <climits>
#include <cmath>
#include <cstring>

#include "mct.h"

namespace nj2k::detail {

namespace {

// OpenJPEG's struct sizes on 64-bit platforms, which its "system limits" checks divide by
constexpr std::uint32_t kOpjPrecinctSize = 56;    // sizeof(opj_tcd_precinct_t)
constexpr std::uint32_t kOpjCblkDecSize = 88;     // sizeof(opj_tcd_cblk_dec_t)

inline std::int32_t int_ceildiv(std::int32_t a, std::int32_t b) noexcept
{
    return static_cast<std::int32_t>((static_cast<std::int64_t>(a) + b - 1) / b);
}
inline std::int32_t int_ceildivpow2(std::int32_t a, std::int32_t b) noexcept
{
    return static_cast<std::int32_t>((a + (static_cast<std::int64_t>(1) << b) - 1) >> b);
}
inline std::int32_t int64_ceildivpow2(std::int64_t a, std::int32_t b) noexcept
{
    return static_cast<std::int32_t>((a + (static_cast<std::int64_t>(1) << b) - 1) >> b);
}
inline std::int32_t int_floordivpow2(std::int32_t a, std::int32_t b) noexcept
{
    return a >> b;
}
inline std::int32_t imax(std::int32_t a, std::int32_t b) noexcept
{
    return a > b ? a : b;
}
inline std::int32_t imin(std::int32_t a, std::int32_t b) noexcept
{
    return a < b ? a : b;
}
inline std::uint32_t uint_adds(std::uint32_t a, std::uint32_t b) noexcept
{
    const std::uint64_t sum = std::uint64_t(a) + b;
    return static_cast<std::uint32_t>(-static_cast<std::int32_t>(sum >> 32)) | static_cast<std::uint32_t>(sum);
}
inline std::uint32_t uint_ceildivpow2(std::uint32_t a, std::uint32_t b) noexcept
{
    return static_cast<std::uint32_t>((a + (std::uint64_t(1) << b) - 1U) >> b);
}

nj2k_status_t arena_status(const WorkArena& arena) noexcept
{
    return arena.budget_failed() ? NJ2K_E_LIMIT : NJ2K_E_NO_MEMORY;
}

// a tag tree of w x h leaves: the nodes of every level, or none when a side is 0 (opj_tgt_create)
bool make_tree(WorkArena& arena, TagTree& t, std::uint32_t w, std::uint32_t h) noexcept
{
    t.w = w;
    t.h = h;
    t.nodes = nullptr;
    if (std::uint64_t(w) * h == 0) {
        return true;
    }
    std::uint64_t total = 0;
    for (std::uint32_t lw = w, lh = h;;) {
        const std::uint64_t n = std::uint64_t(lw) * lh;
        total += n;
        if (n <= 1) {
            break;
        }
        lw = (lw + 1) / 2;
        lh = (lh + 1) / 2;
    }
    t.nodes = arena.raw_array<TagNode>(std::size_t(total));
    return t.nodes != nullptr;
}

// opj_tcd_is_subband_area_of_interest for a whole-tile decode (see t2.cpp)
bool area_of_interest(const TileComp& tilec, std::uint32_t resno, std::uint32_t bandno, std::uint32_t band_x0,
                      std::uint32_t band_y0, std::uint32_t band_x1, std::uint32_t band_y1) noexcept
{
    const std::uint32_t filter_margin = (tilec.params.style->qmfbid == 1) ? 2 : 3;
    const std::uint32_t tcx0 = static_cast<std::uint32_t>(tilec.x0), tcy0 = static_cast<std::uint32_t>(tilec.y0);
    const std::uint32_t tcx1 = static_cast<std::uint32_t>(tilec.x1), tcy1 = static_cast<std::uint32_t>(tilec.y1);
    const std::uint32_t nb = (resno == 0) ? tilec.numresolutions - 1 : tilec.numresolutions - resno;
    const std::uint32_t x0b = bandno & 1;
    const std::uint32_t y0b = bandno >> 1;
    auto map = [nb](std::uint32_t t, std::uint32_t b) {
        return (nb == 0) ? t : (t <= (1U << (nb - 1)) * b) ? 0 : uint_ceildivpow2(t - (1U << (nb - 1)) * b, nb);
    };
    std::uint32_t tbx0 = map(tcx0, x0b), tby0 = map(tcy0, y0b), tbx1 = map(tcx1, x0b), tby1 = map(tcy1, y0b);
    tbx0 = tbx0 < filter_margin ? 0 : tbx0 - filter_margin;
    tby0 = tby0 < filter_margin ? 0 : tby0 - filter_margin;
    tbx1 = uint_adds(tbx1, filter_margin);
    tby1 = uint_adds(tby1, filter_margin);
    return band_x0 < tbx1 && band_y0 < tby1 && band_x1 > tbx0 && band_y1 > tby0;
}

}  // namespace

nj2k_status_t Tile::init(const TileInput& input, WorkArena& arena, const Messages& msg) noexcept
{
    in = &input;
    ppt = input.ppt;
    const MainHeader& mh = *input.mh;
    numcomps = mh.numcomps;
    const std::uint32_t p = input.tile % mh.tw, q = input.tile / mh.tw;

    // the four borders of the tile, clipped to the image
    const std::uint32_t l_tx0 = mh.tx0 + p * mh.tdx;
    x0 = static_cast<std::int32_t>(l_tx0 > mh.x0 ? l_tx0 : mh.x0);
    {
        const std::uint32_t e = uint_adds(l_tx0, mh.tdx);
        x1 = static_cast<std::int32_t>(e < mh.x1 ? e : mh.x1);
    }
    if ((x0 < 0) || (x1 <= x0)) {
        msg.emit(Messages::Error, "Tile X coordinates are not supported\n");
        return NJ2K_E_UNSUPPORTED;
    }
    const std::uint32_t l_ty0 = mh.ty0 + q * mh.tdy;
    y0 = static_cast<std::int32_t>(l_ty0 > mh.y0 ? l_ty0 : mh.y0);
    {
        const std::uint32_t e = uint_adds(l_ty0, mh.tdy);
        y1 = static_cast<std::int32_t>(e < mh.y1 ? e : mh.y1);
    }
    if ((y0 < 0) || (y1 <= y0)) {
        msg.emit(Messages::Error, "Tile Y coordinates are not supported\n");
        return NJ2K_E_UNSUPPORTED;
    }
    for (std::uint32_t c = 0; c < numcomps; ++c) {
        if (input.params[c].style->cblksty & kCblkHT) {
            msg.emit(Messages::Error, "HT code-blocks (JPEG 2000 Part 15) are not supported\n");
            return NJ2K_E_UNSUPPORTED;
        }
    }

    comps = arena.make_array<TileComp>(numcomps);
    if (!comps) {
        return arena_status(arena);
    }
    for (std::uint32_t compno = 0; compno < numcomps; ++compno) {
        TileComp& tc = comps[compno];
        const SizComponent& sc = mh.comps[compno];
        const TileComponentParams& params = input.params[compno];
        const CodingStyle& style = *params.style;
        tc.params = params;
        tc.x0 = int_ceildiv(x0, sc.dx);
        tc.y0 = int_ceildiv(y0, sc.dy);
        tc.x1 = int_ceildiv(x1, sc.dx);
        tc.y1 = int_ceildiv(y1, sc.dy);
        tc.numresolutions = style.numresolutions;
        tc.minimum_num_resolutions =
            style.numresolutions <= input.reduce ? 1 : style.numresolutions - input.reduce;
        tc.dc_shift = sc.sgnd ? 0 : static_cast<std::int32_t>(1u << (sc.prec - 1));
        tc.res = arena.make_array<Resolution>(tc.numresolutions);
        if (!tc.res) {
            return arena_status(arena);
        }
        std::uint32_t level_no = tc.numresolutions;
        for (std::uint32_t resno = 0; resno < tc.numresolutions; ++resno) {
            Resolution& r = tc.res[resno];
            --level_no;
            const std::int32_t lv = static_cast<std::int32_t>(level_no);
            r.x0 = int_ceildivpow2(tc.x0, lv);
            r.y0 = int_ceildivpow2(tc.y0, lv);
            r.x1 = int_ceildivpow2(tc.x1, lv);
            r.y1 = int_ceildivpow2(tc.y1, lv);
            const std::uint32_t pdx = style.precincts[resno] & 0xFu, pdy = style.precincts[resno] >> 4;
            r.pdx = pdx;
            r.pdy = pdy;
            const std::int32_t tl_prc_x_start = int_floordivpow2(r.x0, static_cast<std::int32_t>(pdx)) << pdx;
            const std::int32_t tl_prc_y_start = int_floordivpow2(r.y0, static_cast<std::int32_t>(pdy)) << pdy;
            std::int32_t br_prc_x_end, br_prc_y_end;
            {
                const std::uint32_t tmp = static_cast<std::uint32_t>(int_ceildivpow2(r.x1, static_cast<std::int32_t>(pdx)))
                                          << pdx;
                if (tmp > static_cast<std::uint32_t>(INT_MAX)) {
                    msg.emit(Messages::Error, "Integer overflow\n");
                    return NJ2K_E_UNSUPPORTED;
                }
                br_prc_x_end = static_cast<std::int32_t>(tmp);
            }
            {
                const std::uint32_t tmp = static_cast<std::uint32_t>(int_ceildivpow2(r.y1, static_cast<std::int32_t>(pdy)))
                                          << pdy;
                if (tmp > static_cast<std::uint32_t>(INT_MAX)) {
                    msg.emit(Messages::Error, "Integer overflow\n");
                    return NJ2K_E_UNSUPPORTED;
                }
                br_prc_y_end = static_cast<std::int32_t>(tmp);
            }
            r.pw = (r.x0 == r.x1) ? 0U : static_cast<std::uint32_t>((br_prc_x_end - tl_prc_x_start) >> pdx);
            r.ph = (r.y0 == r.y1) ? 0U : static_cast<std::uint32_t>((br_prc_y_end - tl_prc_y_start) >> pdy);
            if ((r.pw != 0U) && ((UINT32_MAX / r.pw) < r.ph)) {
                msg.emit(Messages::Error, "Size of tile data exceeds system limits\n");
                return NJ2K_E_LIMIT;
            }
            const std::uint32_t nb_precincts = r.pw * r.ph;
            if ((UINT32_MAX / kOpjPrecinctSize) < nb_precincts) {
                msg.emit(Messages::Error, "Size of tile data exceeds system limits\n");
                return NJ2K_E_LIMIT;
            }
            std::int32_t tlcbgxstart, tlcbgystart;
            std::uint32_t cbgwidthexpn, cbgheightexpn;
            if (resno == 0) {
                tlcbgxstart = tl_prc_x_start;
                tlcbgystart = tl_prc_y_start;
                cbgwidthexpn = pdx;
                cbgheightexpn = pdy;
                r.numbands = 1;
            } else {
                tlcbgxstart = int_ceildivpow2(tl_prc_x_start, 1);
                tlcbgystart = int_ceildivpow2(tl_prc_y_start, 1);
                cbgwidthexpn = pdx - 1;
                cbgheightexpn = pdy - 1;
                r.numbands = 3;
            }
            const std::uint32_t cblkwidthexpn = style.cblkw < cbgwidthexpn ? style.cblkw : cbgwidthexpn;
            const std::uint32_t cblkheightexpn = style.cblkh < cbgheightexpn ? style.cblkh : cbgheightexpn;
            r.cblkw_expn = cblkwidthexpn;
            r.cblkh_expn = cblkheightexpn;
            for (std::uint32_t bandno = 0; bandno < r.numbands; ++bandno) {
                Band& band = r.bands[bandno];
                if (resno == 0) {
                    band.bandno = 0;
                    band.x0 = int_ceildivpow2(tc.x0, lv);
                    band.y0 = int_ceildivpow2(tc.y0, lv);
                    band.x1 = int_ceildivpow2(tc.x1, lv);
                    band.y1 = int_ceildivpow2(tc.y1, lv);
                } else {
                    band.bandno = bandno + 1;
                    const std::int32_t l_x0b = static_cast<std::int32_t>(band.bandno & 1);
                    const std::int32_t l_y0b = static_cast<std::int32_t>(band.bandno >> 1);
                    band.x0 = int64_ceildivpow2(tc.x0 - (static_cast<std::int64_t>(l_x0b) << level_no), lv + 1);
                    band.y0 = int64_ceildivpow2(tc.y0 - (static_cast<std::int64_t>(l_y0b) << level_no), lv + 1);
                    band.x1 = int64_ceildivpow2(tc.x1 - (static_cast<std::int64_t>(l_x0b) << level_no), lv + 1);
                    band.y1 = int64_ceildivpow2(tc.y1 - (static_cast<std::int64_t>(l_y0b) << level_no), lv + 1);
                }
                {
                    // Table E-1, sub-band gains; BUG_WEIRD_TWO_INVK: 0 for irreversible bands, which
                    // the 9/7 inverse compensates with 2 / invK (dwt.cpp)
                    const std::int32_t log2_gain = (style.qmfbid == 0) ? 0
                                                   : (band.bandno == 0) ? 0
                                                   : (band.bandno == 3) ? 2
                                                                        : 1;
                    const std::int32_t Rb = static_cast<std::int32_t>(sc.prec) + log2_gain;
                    const std::uint32_t step_index = resno == 0 ? 0 : 3 * resno - 2 + bandno;
                    std::uint32_t expn = 0, mant = 0;
                    params.quant->step(step_index, expn, mant);
                    // Delta_b of equation E-3: pow(2.0, n) is exact, as ldexp is
                    band.stepsize = static_cast<float>((1.0 + mant / 2048.0) *
                                                       std::ldexp(1.0, Rb - static_cast<std::int32_t>(expn)));
                    // Mb of equation E-2
                    band.numbps = static_cast<std::int32_t>(expn) + static_cast<std::int32_t>(params.quant->numgbits) - 1;
                }
                band.precincts = nullptr;
                if (nb_precincts > 0U) {
                    band.precincts = arena.make_array<Precinct>(nb_precincts);
                    if (!band.precincts) {
                        return arena_status(arena);
                    }
                }
                for (std::uint32_t precno = 0; precno < nb_precincts; ++precno) {
                    Precinct& prc = band.precincts[precno];
                    const std::int32_t cbgxstart =
                        tlcbgxstart + static_cast<std::int32_t>(precno % r.pw) * (1 << cbgwidthexpn);
                    const std::int32_t cbgystart =
                        tlcbgystart + static_cast<std::int32_t>(precno / r.pw) * (1 << cbgheightexpn);
                    const std::int32_t cbgxend = cbgxstart + (1 << cbgwidthexpn);
                    const std::int32_t cbgyend = cbgystart + (1 << cbgheightexpn);
                    prc.x0 = imax(cbgxstart, band.x0);
                    prc.y0 = imax(cbgystart, band.y0);
                    prc.x1 = imin(cbgxend, band.x1);
                    prc.y1 = imin(cbgyend, band.y1);
                    const std::int32_t tlcblkxstart =
                        int_floordivpow2(prc.x0, static_cast<std::int32_t>(cblkwidthexpn)) << cblkwidthexpn;
                    const std::int32_t tlcblkystart =
                        int_floordivpow2(prc.y0, static_cast<std::int32_t>(cblkheightexpn)) << cblkheightexpn;
                    const std::int32_t brcblkxend =
                        int_ceildivpow2(prc.x1, static_cast<std::int32_t>(cblkwidthexpn)) << cblkwidthexpn;
                    const std::int32_t brcblkyend =
                        int_ceildivpow2(prc.y1, static_cast<std::int32_t>(cblkheightexpn)) << cblkheightexpn;
                    prc.cw = static_cast<std::uint32_t>((brcblkxend - tlcblkxstart) >> cblkwidthexpn);
                    prc.ch = static_cast<std::uint32_t>((brcblkyend - tlcblkystart) >> cblkheightexpn);
                    prc.cblk_x0 = tlcblkxstart;
                    prc.cblk_y0 = tlcblkystart;
                    const std::uint32_t nb_code_blocks = prc.cw * prc.ch;
                    if ((UINT32_MAX / kOpjCblkDecSize) < nb_code_blocks) {
                        msg.emit(Messages::Error, "Size of code block data exceeds system limits\n");
                        return NJ2K_E_LIMIT;
                    }
                    if (!make_tree(arena, prc.incl, prc.cw, prc.ch) || !make_tree(arena, prc.imsb, prc.cw, prc.ch)) {
                        return arena_status(arena);
                    }
                    prc.cblks = nullptr;
                    if (nb_code_blocks) {
                        prc.cblks = arena.make_array<CodeBlock>(nb_code_blocks);
                        if (!prc.cblks) {
                            return arena_status(arena);
                        }
                    }
                }
            }
        }
        const Resolution& dec = tc.res[tc.minimum_num_resolutions - 1];
        tc.data_w = static_cast<std::uint32_t>(dec.x1 - dec.x0);
        tc.data_h = static_cast<std::uint32_t>(dec.y1 - dec.y0);
        tc.data = nullptr;
    }
    return NJ2K_OK;
}

std::size_t Tile::samples_bytes(std::uint32_t c) const noexcept
{
    const TileComp& tc = comps[c];
    return std::size_t(tc.data_w) * tc.data_h * sizeof(std::int32_t);
}

std::size_t Tile::dwt_scratch_bytes_needed() const noexcept
{
    std::size_t most = 0;
    for (std::uint32_t c = 0; c < numcomps; ++c) {
        if (in->selected && !in->selected[c]) {
            continue;
        }
        const TileComp& tc = comps[c];
        ResRect rects[kMaxResolutions];
        for (std::uint32_t r = 0; r < tc.minimum_num_resolutions; ++r) {
            rects[r] = ResRect{tc.res[r].x0, tc.res[r].y0, tc.res[r].x1, tc.res[r].y1};
        }
        const std::size_t n = dwt_scratch_bytes(rects, tc.minimum_num_resolutions, tc.params.style->qmfbid == 1);
        most = n > most ? n : most;
    }
    return most;
}

std::size_t Tile::largest_cblk_bytes() const noexcept
{
    std::size_t most = 0;
    for (std::uint32_t c = 0; c < numcomps; ++c) {
        const TileComp& tc = comps[c];
        for (std::uint32_t r = 0; r < tc.minimum_num_resolutions; ++r) {
            const Resolution& res = tc.res[r];
            for (std::uint32_t b = 0; b < res.numbands; ++b) {
                const Band& band = res.bands[b];
                const std::uint32_t np = res.pw * res.ph;
                for (std::uint32_t pn = 0; band.precincts && pn < np; ++pn) {
                    const Precinct& prc = band.precincts[pn];
                    const std::uint32_t nb = prc.cw * prc.ch;
                    for (std::uint32_t k = 0; k < nb; ++k) {
                        const CodeBlock& cb = prc.cblks[k];
                        std::size_t total = 0;
                        for (std::uint32_t i = 0; i < cb.nchunks; ++i) {
                            total += cb.chunks[i].len;
                        }
                        most = total > most ? total : most;
                    }
                }
            }
        }
    }
    return most;
}

nj2k_status_t Tile::decode_samples(T1& t1, const Allocator& alloc, void* dwt_scratch, const std::uint32_t* resno_decoded,
                                   const Messages& msg) noexcept
{
    (void)alloc;
    // Tier-1 (opj_t1_decode_cblks, whole tile), component by component
    for (std::uint32_t compno = 0; compno < numcomps; ++compno) {
        if (in->selected && !in->selected[compno]) {
            continue;
        }
        TileComp& tc = comps[compno];
        const CodingStyle& style = *tc.params.style;
        const std::uint32_t tile_w = tc.data_w;
        const bool reversible = style.qmfbid == 1;
        for (std::uint32_t resno = 0; resno < tc.minimum_num_resolutions; ++resno) {
            const Resolution& res = tc.res[resno];
            for (std::uint32_t bandno = 0; bandno < res.numbands; ++bandno) {
                const Band& band = res.bands[bandno];
                const std::uint32_t np = res.pw * res.ph;
                for (std::uint32_t precno = 0; precno < np; ++precno) {
                    const Precinct& prc = band.precincts[precno];
                    if (!area_of_interest(tc, resno, band.bandno, std::uint32_t(prc.x0), std::uint32_t(prc.y0),
                                          std::uint32_t(prc.x1), std::uint32_t(prc.y1))) {
                        continue;
                    }
                    const std::uint32_t nb = prc.cw * prc.ch;
                    for (std::uint32_t cblkno = 0; cblkno < nb; ++cblkno) {
                        const CodeBlock& cb = prc.cblks[cblkno];
                        const std::int32_t cbx = prc.cblk_x0 + static_cast<std::int32_t>(cblkno % prc.cw) * (1 << res.cblkw_expn);
                        const std::int32_t cby = prc.cblk_y0 + static_cast<std::int32_t>(cblkno / prc.cw) * (1 << res.cblkh_expn);
                        const std::int32_t cx0 = imax(cbx, prc.x0), cy0 = imax(cby, prc.y0);
                        const std::int32_t cx1 = imin(cbx + (1 << res.cblkw_expn), prc.x1);
                        const std::int32_t cy1 = imin(cby + (1 << res.cblkh_expn), prc.y1);
                        if (!area_of_interest(tc, resno, band.bandno, std::uint32_t(cx0), std::uint32_t(cy0),
                                              std::uint32_t(cx1), std::uint32_t(cy1))) {
                            continue;
                        }
                        T1Block b{};
                        b.w = static_cast<std::uint32_t>(cx1 - cx0);
                        b.h = static_cast<std::uint32_t>(cy1 - cy0);
                        b.numbps = cb.numbps;
                        b.orient = band.bandno;
                        b.roishift = tc.params.roishift;
                        b.cblksty = style.cblksty;
                        b.chunks = cb.chunks;
                        b.nchunks = cb.nchunks;
                        b.segs = cb.segs;
                        b.nsegs = cb.real_num_segs;
                        std::int32_t x = cx0 - band.x0;
                        std::int32_t y = cy0 - band.y0;
                        if (band.bandno & 1) {
                            const Resolution& pres = tc.res[resno - 1];
                            x += pres.x1 - pres.x0;
                        }
                        if (band.bandno & 2) {
                            const Resolution& pres = tc.res[resno - 1];
                            y += pres.y1 - pres.y0;
                        }
                        std::int32_t* dst = tc.data + static_cast<std::size_t>(y) * tile_w + static_cast<std::size_t>(x);
                        if (cb.nchunks == 0) {
                            if (!t1_empty_cblk_ok(b, msg)) {
                                return NJ2K_E_DATA;
                            }
                            t1_store(nullptr, b.w, b.h, 0, reversible, band.stepsize, dst, tile_w);
                            continue;
                        }
                        if (!t1_decode_cblk(t1, b, msg)) {
                            return NJ2K_E_DATA;
                        }
                        t1_store(t1.data, b.w, b.h, b.roishift, reversible, band.stepsize, dst, tile_w);
                    }
                }
            }
        }
    }

    // the inverse wavelet up to the resolution decoded (opj_tcd_dwt_decode)
    for (std::uint32_t compno = 0; compno < numcomps; ++compno) {
        if (in->selected && !in->selected[compno]) {
            continue;
        }
        TileComp& tc = comps[compno];
        ResRect rects[kMaxResolutions];
        const std::uint32_t numres = resno_decoded[compno] + 1;
        for (std::uint32_t r = 0; r < numres && r < tc.numresolutions; ++r) {
            rects[r] = ResRect{tc.res[r].x0, tc.res[r].y0, tc.res[r].x1, tc.res[r].y1};
        }
        dwt_decode(tc.data, tc.data_w, rects, numres, tc.params.style->qmfbid == 1, dwt_scratch);
    }

    // the colour transform (opj_tcd_mct_decode)
    const TileCoding& coding = *in->params[0].coding;
    if (coding.mct != 0 && !in->selected) {
        const TileComp& c0 = comps[0];
        const Resolution& r0 = c0.res[c0.minimum_num_resolutions - 1];
        const std::size_t l_samples = std::size_t(std::uint32_t(r0.x1 - r0.x0)) * std::size_t(std::uint32_t(r0.y1 - r0.y0));
        if (numcomps >= 3) {
            if (c0.minimum_num_resolutions != comps[1].minimum_num_resolutions ||
                c0.minimum_num_resolutions != comps[2].minimum_num_resolutions) {
                msg.emit(Messages::Error, "Tiles don't all have the same dimension. Skip the MCT step.\n");
                return NJ2K_E_DATA;
            }
            const Resolution& r1 = comps[1].res[c0.minimum_num_resolutions - 1];
            const Resolution& r2 = comps[2].res[c0.minimum_num_resolutions - 1];
            if (resno_decoded[0] != resno_decoded[1] || resno_decoded[0] != resno_decoded[2] ||
                std::size_t(std::uint32_t(r1.x1 - r1.x0)) * std::size_t(std::uint32_t(r1.y1 - r1.y0)) != l_samples ||
                std::size_t(std::uint32_t(r2.x1 - r2.x0)) * std::size_t(std::uint32_t(r2.y1 - r2.y0)) != l_samples) {
                msg.emit(Messages::Error, "Tiles don't all have the same dimension. Skip the MCT step.\n");
                return NJ2K_E_DATA;
            }
            if (in->params[0].style->qmfbid == 1) {
                mct_decode_rct(comps[0].data, comps[1].data, comps[2].data, l_samples);
            } else {
                mct_decode_ict(comps[0].data, comps[1].data, comps[2].data, l_samples);
            }
        } else {
            msg.emit(Messages::Error, "Number of components (%u) is inconsistent with a MCT. Skip the MCT step.\n",
                     numcomps);
        }
    }

    // the DC level shift (opj_tcd_dc_level_shift_decode)
    for (std::uint32_t compno = 0; compno < numcomps; ++compno) {
        if (in->selected && !in->selected[compno]) {
            continue;
        }
        TileComp& tc = comps[compno];
        const SizComponent& sc = in->mh->comps[compno];
        const Resolution& res = tc.res[resno_decoded[compno]];
        const std::uint32_t w = static_cast<std::uint32_t>(res.x1 - res.x0);
        const std::uint32_t h = static_cast<std::uint32_t>(res.y1 - res.y0);
        std::int32_t lo, hi;
        if (sc.sgnd) {
            lo = -static_cast<std::int32_t>(1u << (sc.prec - 1));
            hi = static_cast<std::int32_t>((1u << (sc.prec - 1)) - 1);
        } else {
            lo = 0;
            hi = static_cast<std::int32_t>((1u << sc.prec) - 1);
        }
        if (w == 0 || h == 0) {
            continue;
        }
        if (tc.params.style->qmfbid == 1) {
            dc_shift_53(tc.data, w, h, tc.data_w, tc.dc_shift, lo, hi);
        } else {
            dc_shift_97(tc.data, w, h, tc.data_w, tc.dc_shift, lo, hi);
        }
    }
    return NJ2K_OK;
}

}  // namespace nj2k::detail

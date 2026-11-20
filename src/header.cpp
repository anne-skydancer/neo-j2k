// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The codestream header reader. It follows OpenJPEG 2.5.4's checks (j2k.c, read_siz, read_cod and
// their siblings) on every marker it shares with it, so it accepts and refuses what OpenJPEG does on
// valid streams; where OpenJPEG accepts what the standard forbids, or reads a valid stream in a way
// that depends on marker order, this reader follows the standard (doc/departures.md). Messages carry
// OpenJPEG's wording where the check is OpenJPEG's. The checks and messages derive from OpenJPEG's
// j2k.c, whose notice follows.
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
 * Copyright (c) 2008, Jerome Fimes, Communications & Systemes <jerome.fimes@c-s.fr>
 * Copyright (c) 2006-2007, Parvatha Elangovan
 * Copyright (c) 2010-2011, Kaori Hagihara
 * Copyright (c) 2011-2012, Centre National d'Etudes Spatiales (CNES), France
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
#include "header.h"

#include <cstdarg>
#include <cstring>

#include "stream.h"

namespace nj2k::detail {

namespace {

std::uint16_t be16(const std::uint8_t* p) noexcept
{
    return std::uint16_t(std::uint32_t(p[0]) << 8 | p[1]);
}

std::uint32_t be32(const std::uint8_t* p) noexcept
{
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3];
}

nj2k_status_t fail(const Messages& msg, nj2k_status_t status, const char* format, ...) noexcept NJ2K_PRINTF_LIKE(3, 4);

nj2k_status_t fail(const Messages& msg, nj2k_status_t status, const char* format, ...) noexcept
{
    va_list args;
    va_start(args, format);
    msg.vemit(Messages::Error, format, args);
    va_end(args);
    return status;
}

nj2k_status_t need_data(const Messages& msg, const char* where) noexcept
{
    return fail(msg, NJ2K_E_NEED_DATA, "Stream too short: the bytes end inside %s\n", where);
}

nj2k_status_t arena_failure(const Messages& msg, const Arena& arena) noexcept
{
    if (arena.failure() == Arena::Failure::budget) {
        return fail(msg, NJ2K_E_LIMIT, "The headers need more memory than the decoder's limit\n");
    }
    return fail(msg, NJ2K_E_NO_MEMORY, "Not enough memory to read the headers\n");
}

// JPEG 2000 Part 2 markers (DCO, VMS, DFS, ADS, MCT, MCC, NLT, MCO, CBD, ATK, ...).
bool part2_marker(std::uint16_t m) noexcept
{
    return m >= 0xFF70 && m <= 0xFF7F;
}

// The body of a marker segment, after its marker code and length, copied into the arena (the model
// owns its bytes). No allocation for bytes the host says are not there.
nj2k_status_t read_body(Source& src, Arena& arena, const Messages& msg, std::uint32_t size, std::uint8_t*& body,
                        const char* where) noexcept
{
    body = nullptr;
    if (size == 0) {
        return NJ2K_OK;
    }
    const std::int64_t left = src.bytes_left();
    if (left >= 0 && std::uint64_t(left) < size) {
        src.skip(std::uint64_t(left));
        return need_data(msg, where);
    }
    body = static_cast<std::uint8_t*>(arena.allocate(size, 1));
    if (!body) {
        return arena_failure(msg, arena);
    }
    if (src.read(body, size) != size) {
        return need_data(msg, where);
    }
    return NJ2K_OK;
}

// SPcod / SPcoc: the coding style of a tile-component. cs.csty is set by the caller. used is what
// was read; the caller checks the segment ends there.
nj2k_status_t read_spcod(const Messages& msg, const std::uint8_t* p, std::uint32_t n, CodingStyle& cs,
                         std::uint32_t& used) noexcept
{
    if (n < 5) {
        return fail(msg, NJ2K_E_HEADER, "Error reading SPCod SPCoc element\n");
    }
    const std::uint32_t numres = std::uint32_t(p[0]) + 1;
    if (numres > kMaxResolutions) {
        return fail(msg, NJ2K_E_HEADER, "Invalid value for numresolutions : %u, max value is set in openjpeg.h at %u\n",
                    numres, kMaxResolutions);
    }
    const std::uint32_t cblkw = std::uint32_t(p[1]) + 2, cblkh = std::uint32_t(p[2]) + 2;
    if (cblkw > 10 || cblkh > 10 || cblkw + cblkh > 12) {
        return fail(msg, NJ2K_E_HEADER, "Error reading SPCod SPCoc element, Invalid cblkw/cblkh combination\n");
    }
    if (p[3] & kCblkHTMixed) {
        return fail(msg, NJ2K_E_UNSUPPORTED,
                    "Error reading SPCod SPCoc element. Unsupported Mixed HT code-block style found\n");
    }
    if (p[4] > 1) {
        return fail(msg, NJ2K_E_HEADER, "Error reading SPCod SPCoc element, Invalid transformation found\n");
    }
    cs.numresolutions = std::uint8_t(numres);
    cs.cblkw = std::uint8_t(cblkw);
    cs.cblkh = std::uint8_t(cblkh);
    cs.cblksty = p[3];
    cs.qmfbid = p[4];
    used = 5;
    if (cs.csty & kCstyPrecincts) {
        if (n - 5 < numres) {
            return fail(msg, NJ2K_E_HEADER, "Error reading SPCod SPCoc element\n");
        }
        for (std::uint32_t r = 0; r < numres; ++r) {
            const std::uint8_t v = p[5 + r];
            // precinct exponent 0 is allowed at the lowest resolution only (ISO 15444-1 Table A.21)
            if (r != 0 && ((v & 0xF) == 0 || (v >> 4) == 0)) {
                return fail(msg, NJ2K_E_HEADER, "Invalid precinct size\n");
            }
            cs.precincts[r] = v;
        }
        used += numres;
    } else {
        std::memset(cs.precincts, 0xFF, sizeof(cs.precincts));
    }
    return NJ2K_OK;
}

// COD: the tile-wide coding style and the default style of every component.
nj2k_status_t read_cod(const Messages& msg, const HeaderSettings& set, const std::uint8_t* p, std::uint32_t n,
                       TileCoding& tc, CodingStyle& cs) noexcept
{
    if (n < 5) {
        return fail(msg, NJ2K_E_HEADER, "Error reading COD marker\n");
    }
    if (p[0] & ~std::uint32_t(kCstyPrecincts | kCstySop | kCstyEph)) {
        return fail(msg, NJ2K_E_HEADER, "Unknown Scod value in COD marker\n");
    }
    if (p[1] > 4) {
        return fail(msg, NJ2K_E_HEADER, "Unknown progression order in COD marker\n");
    }
    const std::uint32_t layers = be16(p + 2);
    if (layers == 0) {
        return fail(msg, NJ2K_E_HEADER, "Invalid number of layers in COD marker : %u not in range [1-65535]\n", layers);
    }
    if (set.limits.max_layers != 0 && layers > set.limits.max_layers) {
        return fail(msg, NJ2K_E_LIMIT, "Error with COD marker: %u layers exceed the decoder's limit of %u\n", layers,
                    set.limits.max_layers);
    }
    if (p[4] > 1) {
        return fail(msg, NJ2K_E_HEADER, "Invalid multiple component transformation\n");
    }
    TileCoding t{};
    t.csty = p[0];
    t.prg = p[1];
    t.numlayers = std::uint16_t(layers);
    t.mct = p[4];
    CodingStyle s{};
    s.csty = std::uint8_t(t.csty & kCstyPrecincts);
    std::uint32_t used = 0;
    if (const nj2k_status_t st = read_spcod(msg, p + 5, n - 5, s, used); st != NJ2K_OK) {
        return fail(msg, st, "Error reading COD marker\n");
    }
    if (5 + used != n) {
        return fail(msg, NJ2K_E_HEADER, "Error reading COD marker\n");
    }
    tc = t;
    cs = s;
    return NJ2K_OK;
}

std::uint32_t component_room(std::uint32_t numcomps) noexcept
{
    return numcomps <= 256 ? 1 : 2;
}

std::uint32_t read_component(const std::uint8_t* p, std::uint32_t room) noexcept
{
    return room == 1 ? p[0] : be16(p);
}

// COC: one component's coding style.
nj2k_status_t read_coc(const Messages& msg, std::uint32_t numcomps, const std::uint8_t* p, std::uint32_t n,
                       std::uint32_t& comp, CodingStyle& cs) noexcept
{
    const std::uint32_t room = component_room(numcomps);
    if (n < room + 1) {
        return fail(msg, NJ2K_E_HEADER, "Error reading COC marker\n");
    }
    comp = read_component(p, room);
    if (comp >= numcomps) {
        return fail(msg, NJ2K_E_HEADER, "Error reading COC marker (bad number of components)\n");
    }
    CodingStyle s{};
    s.csty = p[room];
    std::uint32_t used = 0;
    if (const nj2k_status_t st = read_spcod(msg, p + room + 1, n - room - 1, s, used); st != NJ2K_OK) {
        return fail(msg, st, "Error reading COC marker\n");
    }
    if (room + 1 + used != n) {
        return fail(msg, NJ2K_E_HEADER, "Error reading COC marker\n");
    }
    cs = s;
    return NJ2K_OK;
}

// SQcd / SQcc. The whole remainder of the segment must be used.
nj2k_status_t read_sqcd(const Messages& msg, const std::uint8_t* p, std::uint32_t n, Quantization& q,
                        const char* marker) noexcept
{
    if (n < 1) {
        return fail(msg, NJ2K_E_HEADER, "Error reading SQcd or SQcc element\n");
    }
    Quantization out{};
    out.qntsty = p[0] & 0x1F;
    out.numgbits = p[0] >> 5;
    const std::uint32_t rest = n - 1;
    if (out.qntsty == kQntDerived) {
        if (rest != 2) {
            return fail(msg, NJ2K_E_HEADER, "Error reading %s marker\n", marker);
        }
        out.nbands = 1;
        out.steps[0] = be16(p + 1);
    } else if (out.qntsty == kQntNone || out.qntsty == kQntExpounded) {
        const std::uint32_t width = out.qntsty == kQntNone ? 1 : 2;
        if (rest % width != 0) {
            return fail(msg, NJ2K_E_HEADER, "Error reading %s marker\n", marker);
        }
        const std::uint32_t bands = rest / width;
        if (bands > kMaxBands) {
            msg.emit(Messages::Warning,
                     "While reading CCP_QNTSTY element inside QCD or QCC marker segment, number of subbands (%u) is "
                     "greater to OPJ_J2K_MAXBANDS (%u). So we limit the number of elements stored to OPJ_J2K_MAXBANDS "
                     "(%u) and skip the rest. \n",
                     bands, kMaxBands, kMaxBands);
        }
        out.nbands = std::uint8_t(bands < kMaxBands ? bands : kMaxBands);
        for (std::uint32_t b = 0; b < out.nbands; ++b) {
            out.steps[b] = width == 1 ? std::uint16_t(std::uint32_t(p[1 + b] >> 3) << 11) : be16(p + 1 + 2 * b);
        }
    } else {
        // reserved styles; OpenJPEG reads them as expounded (doc/departures.md)
        return fail(msg, NJ2K_E_HEADER, "Unknown quantization style %u in %s marker\n", unsigned(out.qntsty), marker);
    }
    q = out;
    return NJ2K_OK;
}

nj2k_status_t read_qcc(const Messages& msg, std::uint32_t numcomps, const std::uint8_t* p, std::uint32_t n,
                       std::uint32_t& comp, Quantization& q) noexcept
{
    const std::uint32_t room = component_room(numcomps);
    if (n < room) {
        return fail(msg, NJ2K_E_HEADER, "Error reading QCC marker\n");
    }
    comp = read_component(p, room);
    if (comp >= numcomps) {
        return fail(msg, NJ2K_E_HEADER, "Invalid component number: %u, regarding the number of components %u\n", comp,
                    numcomps);
    }
    return read_sqcd(msg, p + room, n - room, q, "QCC");
}

nj2k_status_t read_rgn(const Messages& msg, std::uint32_t numcomps, const std::uint8_t* p, std::uint32_t n,
                       std::uint32_t& comp, std::uint8_t& roishift) noexcept
{
    const std::uint32_t room = component_room(numcomps);
    if (n != 2 + room) {
        return fail(msg, NJ2K_E_HEADER, "Error reading RGN marker\n");
    }
    comp = read_component(p, room);
    if (comp >= numcomps) {
        return fail(msg, NJ2K_E_HEADER, "bad component number in RGN (%u when there are only %u)\n", comp, numcomps);
    }
    if (p[room] != 0) {
        msg.emit(Messages::Warning, "RGN marker: region of interest style %u read as implicit\n", unsigned(p[room]));
    }
    roishift = p[room + 1];
    return NJ2K_OK;
}

// POC entries, appended to list. existing is what OpenJPEG counts against its limit of 31 changes.
nj2k_status_t read_poc(const Messages& msg, Arena& arena, std::uint32_t numcomps, const std::uint8_t* p,
                       std::uint32_t n, std::uint32_t existing, ArenaList<PocEntry>& list) noexcept
{
    const std::uint32_t room = component_room(numcomps);
    const std::uint32_t chunk = 5 + 2 * room;
    const std::uint32_t count = n / chunk;
    if (count == 0 || n % chunk != 0) {
        return fail(msg, NJ2K_E_HEADER, "Error reading POC marker\n");
    }
    if (existing + count >= kMaxPocs) {
        return fail(msg, NJ2K_E_HEADER, "Too many POCs %u\n", existing + count);
    }
    for (std::uint32_t i = 0; i < count; ++i, p += chunk) {
        PocEntry e{};
        e.resno0 = p[0];
        e.compno0 = std::uint16_t(read_component(p + 1, room));
        e.layno1 = be16(p + 1 + room);
        e.resno1 = p[3 + room];
        e.compno1 = std::uint16_t(read_component(p + 4 + room, room));
        e.prg = p[4 + 2 * room];
        if (!list.push(arena, e)) {
            return arena_failure(msg, arena);
        }
    }
    return NJ2K_OK;
}

// A Zppm / Zppt chunk. Each index may come once (OpenJPEG: "Zppm %u already read").
nj2k_status_t add_packed(const Messages& msg, Arena& arena, const std::uint8_t* p, std::uint32_t n,
                         ArenaList<PackedChunk>& list, bool main) noexcept
{
    // the index and at least one byte of packed headers
    if (n < 2) {
        return fail(msg, NJ2K_E_HEADER, "Error reading %s marker\n", main ? "PPM" : "PPT");
    }
    for (const PackedChunk& c : list) {
        if (c.z == p[0]) {
            return fail(msg, NJ2K_E_HEADER, "%s %u already read\n", main ? "Zppm" : "Zppt", unsigned(p[0]));
        }
    }
    PackedChunk c{p[0], n - 1, p + 1};
    if (!list.push(arena, c)) {
        return arena_failure(msg, arena);
    }
    return NJ2K_OK;
}

void sort_packed(ArenaList<PackedChunk>& list) noexcept
{
    for (std::uint32_t i = 1; i < list.size; ++i) {
        const PackedChunk c = list.data[i];
        std::uint32_t j = i;
        while (j > 0 && list.data[j - 1].z > c.z) {
            list.data[j] = list.data[j - 1];
            --j;
        }
        list.data[j] = c;
    }
}

// TLM, as OpenJPEG reads it: an invalid one is ignored with a warning, never an error.
nj2k_status_t read_tlm(const Messages& msg, Arena& arena, MainHeader& mh, const std::uint8_t* p,
                       std::uint32_t n) noexcept
{
    if (n < 2) {
        return fail(msg, NJ2K_E_HEADER, "Error reading TLM marker.\n");
    }
    if (mh.tlm_invalid) {
        return NJ2K_OK;
    }
    const std::uint32_t st = (p[1] >> 4) & 0x3, sp = (p[1] >> 6) & 0x1;
    if (st == 3) {
        mh.tlm_invalid = true;
        msg.emit(Messages::Warning, "opj_j2k_read_tlm(): ST = 3 is invalid.\n");
        return NJ2K_OK;
    }
    const std::uint32_t psize = (sp + 1) * 2, entry = psize + st, body = n - 2;
    if (body % entry != 0) {
        mh.tlm_invalid = true;
        msg.emit(Messages::Warning, "opj_j2k_read_tlm(): TLM marker not of expected size.\n");
        return NJ2K_OK;
    }
    const std::uint8_t* q = p + 2;
    for (std::uint32_t i = 0; i < body / entry; ++i) {
        std::uint32_t tile = 0;
        if (st == 0) {
            tile = mh.tlm.size;
        } else {
            tile = st == 1 ? q[0] : be16(q);
            q += st;
        }
        if (tile >= mh.tile_count()) {
            mh.tlm_invalid = true;
            msg.emit(Messages::Warning, "opj_j2k_read_tlm(): invalid tile number %u\n", tile);
            return NJ2K_OK;
        }
        const std::uint32_t length = psize == 2 ? be16(q) : be32(q);
        q += psize;
        if (!mh.tlm.push(arena, TlmEntry{std::uint16_t(tile), length})) {
            return arena_failure(msg, arena);
        }
    }
    return NJ2K_OK;
}

// SIZ: the image, the tile grid and the components.
nj2k_status_t read_siz(const Messages& msg, Arena& arena, const HeaderSettings& set, const std::uint8_t* p,
                       std::uint32_t n, MainHeader& mh) noexcept
{
    if (n < 36 || (n - 36) % 3 != 0) {
        return fail(msg, NJ2K_E_HEADER, "Error with SIZ marker size\n");
    }
    mh.rsiz = be16(p);
    mh.x1 = be32(p + 2);
    mh.y1 = be32(p + 6);
    mh.x0 = be32(p + 10);
    mh.y0 = be32(p + 14);
    mh.tdx = be32(p + 18);
    mh.tdy = be32(p + 22);
    mh.tx0 = be32(p + 26);
    mh.ty0 = be32(p + 30);
    const std::uint32_t csiz = be16(p + 34);
    if (csiz == 0 || csiz > kMaxComponents) {
        return fail(msg, NJ2K_E_HEADER, "Error with SIZ marker: number of component is illegal -> %u\n", csiz);
    }
    if (csiz != (n - 36) / 3) {
        return fail(msg, NJ2K_E_HEADER,
                    "Error with SIZ marker: number of component is not compatible with the remaining number of "
                    "parameters ( %u vs %u)\n",
                    csiz, (n - 36) / 3);
    }
    if (set.limits.max_components != 0 && csiz > set.limits.max_components) {
        return fail(msg, NJ2K_E_LIMIT, "Error with SIZ marker: %u components exceed the decoder's limit of %u\n", csiz,
                    set.limits.max_components);
    }
    if (mh.x0 >= mh.x1 || mh.y0 >= mh.y1) {
        return fail(msg, NJ2K_E_HEADER, "Error with SIZ marker: negative or zero image size (%lld x %lld)\n",
                    static_cast<long long>(mh.x1) - mh.x0, static_cast<long long>(mh.y1) - mh.y0);
    }
    if (mh.tdx == 0 || mh.tdy == 0) {
        return fail(msg, NJ2K_E_HEADER, "Error with SIZ marker: invalid tile size (tdx: %u, tdy: %u)\n", mh.tdx, mh.tdy);
    }
    if (mh.tx0 > mh.x0 || mh.ty0 > mh.y0 || std::uint64_t(mh.tx0) + mh.tdx <= mh.x0 ||
        std::uint64_t(mh.ty0) + mh.tdy <= mh.y0) {
        return fail(msg, NJ2K_E_HEADER, "Error with SIZ marker: illegal tile offset\n");
    }
    if (!set.dump && set.ihdr_w > 0 && set.ihdr_h > 0 &&
        (set.ihdr_w != mh.x1 - mh.x0 || set.ihdr_h != mh.y1 - mh.y0)) {
        return fail(msg, NJ2K_E_HEADER, "Error with SIZ marker: IHDR w(%u) h(%u) vs. SIZ w(%u) h(%u)\n", set.ihdr_w,
                    set.ihdr_h, mh.x1 - mh.x0, mh.y1 - mh.y0);
    }
    for (std::uint32_t i = 0; i < csiz; ++i) {
        const std::uint8_t* c = p + 36 + 3 * i;
        const std::uint32_t prec = (c[0] & 0x7Fu) + 1, sgnd = c[0] >> 7, dx = c[1], dy = c[2];
        if (!set.dump && !set.allow_varying_depth && i > 0 && (c[0] != p[36])) {
            msg.emit(Messages::Warning,
                     "Despite JP2 BPC!=255, precision and/or sgnd values for comp[%u] is different than comp[0]:\n"
                     "        [0] prec(%u) sgnd(%u) [%u] prec(%u) sgnd(%u)\n",
                     i, (p[36] & 0x7Fu) + 1, unsigned(p[36] >> 7), i, prec, sgnd);
        }
        if (dx < 1 || dy < 1) {
            return fail(msg, NJ2K_E_HEADER,
                        "Invalid values for comp = %u : dx=%u dy=%u (should be between 1 and 255 according to the "
                        "JPEG2000 norm)\n",
                        i, dx, dy);
        }
        if (prec > 31) {
            return fail(msg, NJ2K_E_HEADER,
                        "Invalid values for comp = %u : prec=%u (should be between 1 and 38 according to the JPEG2000 "
                        "norm. OpenJpeg only supports up to 31)\n",
                        i, prec);
        }
    }
    const std::uint64_t tw = (std::uint64_t(mh.x1) - mh.tx0 + mh.tdx - 1) / mh.tdx;
    const std::uint64_t th = (std::uint64_t(mh.y1) - mh.ty0 + mh.tdy - 1) / mh.tdy;
    if (tw == 0 || th == 0 || tw > kMaxTiles / th) {
        return fail(msg, NJ2K_E_HEADER,
                    "Invalid number of tiles : %llu x %llu (maximum fixed by jpeg2000 norm is 65535 tiles)\n",
                    static_cast<unsigned long long>(tw), static_cast<unsigned long long>(th));
    }
    mh.tw = std::uint32_t(tw);
    mh.th = std::uint32_t(th);
    if (set.limits.max_tiles != 0 && mh.tw * mh.th > set.limits.max_tiles) {
        return fail(msg, NJ2K_E_LIMIT, "Error with SIZ marker: %u tiles exceed the decoder's limit of %u\n", mh.tw * mh.th,
                    set.limits.max_tiles);
    }
    // per component: what SIZ says, and room for the main header's COC, QCC and RGN
    mh.numcomps = csiz;
    mh.comps = arena.make_array<SizComponent>(csiz);
    mh.coc = arena.make_array<const CodingStyle*>(csiz);
    mh.qcc = arena.make_array<const Quantization*>(csiz);
    mh.rgn = arena.make_array<std::int16_t>(csiz);
    if (!mh.comps || !mh.coc || !mh.qcc || !mh.rgn) {
        return arena_failure(msg, arena);
    }
    for (std::uint32_t i = 0; i < csiz; ++i) {
        const std::uint8_t* c = p + 36 + 3 * i;
        mh.comps[i] = SizComponent{std::uint8_t((c[0] & 0x7F) + 1), std::uint8_t(c[0] >> 7), c[1], c[2]};
        mh.rgn[i] = -1;
    }
    return NJ2K_OK;
}

// Nppm: the packed headers of each tile-part in turn, over the PPM markers in Zppm order
// (OpenJPEG's opj_j2k_merge_ppm, with its checks).
nj2k_status_t merge_ppm(const Messages& msg, Arena& arena, MainHeader& mh) noexcept
{
    sort_packed(mh.ppm_chunks);
    std::uint8_t* merged = nullptr;
    for (int pass = 0; pass < 2; ++pass) {
        std::uint64_t total = 0, remaining = 0;
        std::uint8_t* out = merged;
        for (const PackedChunk& c : mh.ppm_chunks) {
            const std::uint8_t* d = c.data;
            std::uint64_t size = c.size;
            const std::uint64_t carry = remaining < size ? remaining : size;
            if (out && carry) {
                std::memcpy(out + total, d, std::size_t(carry));
            }
            total += pass == 1 ? carry : 0;
            remaining -= carry;
            d += carry;
            size -= carry;
            while (size > 0) {
                if (size < 4) {
                    return fail(msg, NJ2K_E_HEADER, "Not enough bytes to read Nppm\n");
                }
                const std::uint32_t nppm = be32(d);
                d += 4;
                size -= 4;
                if (pass == 0) {
                    if (total > 0xFFFFFFFFull - nppm) {
                        return fail(msg, NJ2K_E_HEADER, "Too large value for Nppm\n");
                    }
                    total += nppm;
                    if (!mh.ppm_lengths.push(arena, nppm)) {
                        return arena_failure(msg, arena);
                    }
                }
                const std::uint64_t take = nppm < size ? nppm : size;
                if (out && take) {
                    std::memcpy(out + total, d, std::size_t(take));
                }
                total += pass == 1 ? take : 0;
                d += take;
                size -= take;
                remaining = nppm - take;
            }
        }
        if (remaining != 0) {
            return fail(msg, NJ2K_E_HEADER, "Corrupted PPM markers\n");
        }
        if (pass == 0) {
            // Packed packet headers that hold no byte at all: OpenJPEG refuses them too (its
            // opj_malloc(0) gives NULL, "Not enough memory to read PPM marker"), here for what it is
            if (total == 0) {
                return fail(msg, NJ2K_E_HEADER, "Corrupted PPM markers: the packet headers hold no bytes\n");
            }
            // every byte counted comes from the markers, so this is no larger than they are
            mh.ppm_size = total;
            merged = static_cast<std::uint8_t*>(arena.allocate(std::size_t(total), 1));
            if (!merged) {
                return arena_failure(msg, arena);
            }
            mh.ppm_data = merged;
        }
    }
    return NJ2K_OK;
}

// The tile's entry in the directory, made (with its page, and the directory with the first page) when
// it has none; null when the arena refuses. tile is below mh.tile_count().
TileOverrides* tile_overrides(Arena& arena, MainHeader& mh, std::uint16_t tile) noexcept
{
    TileDirectory& dir = mh.tiles;
    constexpr std::uint32_t kPage = TileDirectory::kPageTiles;
    if (!dir.pages) {
        const std::uint32_t count = (mh.tile_count() + kPage - 1) / kPage;
        dir.pages = arena.make_array<TileDirectory::Page*>(count);
        if (!dir.pages) {
            return nullptr;
        }
        dir.page_count = count;
    }
    TileDirectory::Page*& page = dir.pages[tile / kPage];
    if (!page) {
        page = arena.make<TileDirectory::Page>();
        if (!page) {
            return nullptr;
        }
    }
    TileOverrides*& t = page->tile[tile % kPage];
    if (!t) {
        t = arena.make<TileOverrides>();
        if (!t) {
            return nullptr;
        }
        t->tile = tile;
        ++dir.size;
    }
    return t;
}

// The last marker for the component (the one that counts), or null.
template <class T>
const CompValue<T>* last_comp_value(const ArenaList<CompValue<T>>& list, std::uint32_t comp) noexcept
{
    for (std::uint32_t i = list.size; i-- > 0;) {
        if (list.data[i].comp == comp) {
            return &list.data[i];
        }
    }
    return nullptr;
}

// A component's parameters from the main header (main COC > main COD, main QCC > main QCD), then
// the tile's own COD and QCD over them (tile-part COD > main COC, tile-part QCD > main QCC).
TileComponentParams component_defaults(const MainHeader& mh, const TileOverrides* tile, std::uint32_t comp) noexcept
{
    TileComponentParams p{&mh.cod, &mh.main_style(comp), &mh.main_quant(comp), mh.main_roishift(comp)};
    if (tile && tile->has_cod) {
        p.coding = &tile->cod;
        p.style = &tile->cod_style;
    }
    if (tile && tile->qcd) {
        p.quant = tile->qcd;
    }
    return p;
}

// W8: every sub-band of every resolution needs its step size; derived quantization has one.
nj2k_status_t check_step_count(const TileComponentParams& p, const TileOverrides* tile, std::uint32_t comp,
                               const Messages& msg) noexcept
{
    if (p.quant->qntsty == kQntDerived) {
        return NJ2K_OK;
    }
    const std::uint32_t bands = 3u * p.style->numresolutions - 2;
    if (p.quant->nbands >= bands) {
        return NJ2K_OK;
    }
    if (tile) {
        return fail(msg, NJ2K_E_HEADER, "Tile %u, component %u: the quantization gives %u step sizes for %u sub-bands\n",
                    unsigned(tile->tile), comp, unsigned(p.quant->nbands), bands);
    }
    return fail(msg, NJ2K_E_HEADER, "Component %u: the quantization gives %u step sizes for %u sub-bands\n", comp,
                unsigned(p.quant->nbands), bands);
}

// PLT, as OpenJPEG checks it: the packet lengths must end with a complete length.
nj2k_status_t check_plt(const Messages& msg, const std::uint8_t* p, std::uint32_t n) noexcept
{
    if (n < 1) {
        return fail(msg, NJ2K_E_HEADER, "Error reading PLT marker\n");
    }
    std::uint32_t length = 0;
    for (std::uint32_t i = 1; i < n; ++i) {
        length |= p[i] & 0x7Fu;
        if (p[i] & 0x80) {
            length <<= 7;
        } else {
            length = 0;
        }
    }
    if (length != 0) {
        return fail(msg, NJ2K_E_HEADER, "Error reading PLT marker\n");
    }
    return NJ2K_OK;
}

}  // namespace

std::size_t Source::read(std::uint8_t* dst, std::size_t n) noexcept
{
    if (stream_) {
        return stream_->read(dst, n);
    }
    const std::size_t take = n < size_ - at_ ? n : size_ - at_;
    if (take) {
        std::memcpy(dst, data_ + at_, take);
    }
    at_ += take;
    return take;
}

bool Source::skip(std::uint64_t n) noexcept
{
    if (stream_) {
        return stream_->skip(n);
    }
    if (n > size_ - at_) {
        at_ = size_;
        return false;
    }
    at_ += std::size_t(n);
    return true;
}

std::uint64_t Source::tell() const noexcept
{
    return stream_ ? stream_->offset : at_;
}

std::int64_t Source::bytes_left() const noexcept
{
    return stream_ ? stream_->bytes_left() : std::int64_t(size_ - at_);
}

void Quantization::step(std::uint32_t band, std::uint32_t& expn, std::uint32_t& mant) const noexcept
{
    if (qntsty == kQntDerived) {
        const std::int32_t e0 = steps[0] >> 11;
        const std::int32_t e = band == 0 ? e0 : e0 - std::int32_t((band - 1) / 3);
        expn = e > 0 ? std::uint32_t(e) : 0;
        mant = steps[0] & 0x7FFu;
        return;
    }
    if (band < nbands) {
        expn = steps[band] >> 11;
        mant = steps[band] & 0x7FFu;
    } else {
        expn = mant = 0;
    }
}

nj2k_status_t read_main_header(Source& src, Arena& arena, const Messages& msg, const HeaderSettings& set,
                               MainHeader*& out) noexcept
{
    const std::uint64_t start = src.tell();
    std::uint8_t b[2] = {};
    const std::size_t got = src.read(b, 2);
    if (got == 0) {
        return need_data(msg, "the main header (no bytes)");
    }
    if (b[0] != 0xFF || (got == 2 && b[1] != 0x4F)) {
        return fail(msg, NJ2K_E_NOT_JPEG2000, "Expected a SOC marker \n");
    }
    if (got == 1) {
        return need_data(msg, "the SOC marker");
    }
    MainHeader* mh = arena.make<MainHeader>();
    if (!mh) {
        return arena_failure(msg, arena);
    }
    mh->start = start;
    bool has_siz = false, has_cod = false, has_qcd = false;
    for (;;) {
        const std::uint64_t at = src.tell();
        if (src.read(b, 2) != 2) {
            return need_data(msg, "the main header");
        }
        const std::uint16_t m = be16(b);
        if (m < 0xFF00) {
            return fail(msg, NJ2K_E_HEADER, "A marker ID was expected (0xff--) instead of %.8x\n", unsigned(m));
        }
        if (!has_siz && m != mk::SIZ) {
            // ISO 15444-1 A.5: SIZ follows SOC (OpenJPEG skips unknown markers to find it)
            return fail(msg, NJ2K_E_HEADER, "Expected a SIZ marker after SOC, found 0x%04X\n", unsigned(m));
        }
        if (m == mk::SOT) {
            mh->end = at;
            break;
        }
        if (segmentless(m)) {
            if (m >= 0xFF30 && m <= 0xFF3F) {
                msg.emit(Messages::Warning, "Marker 0x%04X without a segment skipped in the main header\n", unsigned(m));
                continue;
            }
            return fail(msg, NJ2K_E_HEADER, "Marker 0x%04X is not compliant with its position (main header)\n",
                        unsigned(m));
        }
        if (src.read(b, 2) != 2) {
            return need_data(msg, "the main header");
        }
        const std::uint32_t length = be16(b);
        if (length < 2) {
            return fail(msg, NJ2K_E_HEADER, "Invalid marker size\n");
        }
        if (part2_marker(m)) {
            return fail(msg, NJ2K_E_UNSUPPORTED, "Marker 0x%04X is a JPEG 2000 Part 2 extension, not supported\n",
                        unsigned(m));
        }
        std::uint8_t* body = nullptr;
        const std::uint32_t n = length - 2;
        if (const nj2k_status_t st = read_body(src, arena, msg, n, body, "the main header"); st != NJ2K_OK) {
            return st;
        }
        nj2k_status_t st = NJ2K_OK;
        switch (m) {
        case mk::SIZ:
            if (has_siz) {
                return fail(msg, NJ2K_E_HEADER, "A second SIZ marker in the main header\n");
            }
            st = read_siz(msg, arena, set, body, n, *mh);
            has_siz = true;
            break;
        case mk::COD:
            st = read_cod(msg, set, body, n, mh->cod, mh->cod_style);
            has_cod = true;
            break;
        case mk::COC: {
            std::uint32_t comp = 0;
            CodingStyle cs{};
            st = read_coc(msg, mh->numcomps, body, n, comp, cs);
            if (st == NJ2K_OK) {
                auto* keep = arena.make<CodingStyle>();
                if (!keep) {
                    return arena_failure(msg, arena);
                }
                *keep = cs;
                mh->coc[comp] = keep;
            }
            break;
        }
        case mk::QCD:
            st = read_sqcd(msg, body, n, mh->qcd, "QCD");
            has_qcd = true;
            break;
        case mk::QCC: {
            std::uint32_t comp = 0;
            auto* q = arena.make<Quantization>();
            if (!q) {
                return arena_failure(msg, arena);
            }
            st = read_qcc(msg, mh->numcomps, body, n, comp, *q);
            if (st == NJ2K_OK) {
                mh->qcc[comp] = q;
            }
            break;
        }
        case mk::RGN: {
            std::uint32_t comp = 0;
            std::uint8_t shift = 0;
            st = read_rgn(msg, mh->numcomps, body, n, comp, shift);
            if (st == NJ2K_OK) {
                mh->rgn[comp] = shift;
            }
            break;
        }
        case mk::POC:
            st = read_poc(msg, arena, mh->numcomps, body, n, mh->poc.size, mh->poc);
            break;
        case mk::PPM:
            mh->has_ppm = true;
            st = add_packed(msg, arena, body, n, mh->ppm_chunks, true);
            break;
        case mk::TLM:
            st = read_tlm(msg, arena, *mh, body, n);
            break;
        case mk::PLM:
            if (n < 1) {
                st = fail(msg, NJ2K_E_HEADER, "Error reading PLM marker\n");
            }
            break;
        case mk::CRG:
            if (n != mh->numcomps * 4) {
                st = fail(msg, NJ2K_E_HEADER, "Error reading CRG marker\n");
            }
            break;
        case mk::CAP:
            mh->has_cap = true;
            break;
        case mk::COM:
        case mk::CPF:
            break;
        case mk::PLT:
        case mk::PPT:
        case mk::SOP:
            st = fail(msg, NJ2K_E_HEADER, "Marker is not compliant with its position\n");
            break;
        default:
            // ISO 15444-1 A.1: a decoder skips marker segments it does not know, by their length
            // (OpenJPEG scans for the next marker it knows instead)
            msg.emit(Messages::Warning, "Unknown marker 0x%04X (%u bytes) skipped in the main header\n", unsigned(m), n);
            break;
        }
        if (st != NJ2K_OK) {
            return st;
        }
    }
    if (!has_cod) {
        return fail(msg, NJ2K_E_HEADER, "required COD marker not found in main header\n");
    }
    if (!has_qcd) {
        return fail(msg, NJ2K_E_HEADER, "required QCD marker not found in main header\n");
    }
    if (mh->has_ppm) {
        if (const nj2k_status_t st = merge_ppm(msg, arena, *mh); st != NJ2K_OK) {
            return fail(msg, st, "Failed to merge PPM data\n");
        }
    }
    // the main header's defaults of every component: precedence and step counts (W7, W8)
    mh->min_resolutions = kMaxResolutions;
    for (std::uint32_t c = 0; c < mh->numcomps; ++c) {
        TileComponentParams params{};
        if (const nj2k_status_t st = resolve_tile_component(*mh, nullptr, c, msg, params); st != NJ2K_OK) {
            return st;
        }
        if (params.style->numresolutions < mh->min_resolutions) {
            mh->min_resolutions = params.style->numresolutions;
        }
    }
    out = mh;
    return NJ2K_OK;
}

nj2k_status_t read_tile_part_marker(Arena& arena, const Messages& msg, const HeaderSettings& set, MainHeader& mh,
                                    std::uint16_t tile, std::uint16_t m, const std::uint8_t* body, std::uint32_t n) noexcept
{
    TileOverrides* t = nullptr;
    if (m == mk::COD || m == mk::COC || m == mk::QCD || m == mk::QCC || m == mk::RGN || m == mk::POC ||
        m == mk::PPT) {
        t = tile_overrides(arena, mh, tile);
        if (!t) {
            return arena_failure(msg, arena);
        }
    }
    nj2k_status_t st = NJ2K_OK;
    switch (m) {
    case mk::COD:
        st = read_cod(msg, set, body, n, t->cod, t->cod_style);
        if (st == NJ2K_OK) {
            t->has_cod = true;
        }
        break;
    case mk::COC: {
        std::uint32_t comp = 0;
        CodingStyle cs{};
        st = read_coc(msg, mh.numcomps, body, n, comp, cs);
        if (st == NJ2K_OK) {
            auto* keep = arena.make<CodingStyle>();
            if (!keep || !t->coc.push(arena, CompValue<const CodingStyle*>{std::uint16_t(comp), keep})) {
                return arena_failure(msg, arena);
            }
            *keep = cs;
        }
        break;
    }
    case mk::QCD: {
        auto* q = arena.make<Quantization>();
        if (!q) {
            return arena_failure(msg, arena);
        }
        st = read_sqcd(msg, body, n, *q, "QCD");
        if (st == NJ2K_OK) {
            t->qcd = q;
        }
        break;
    }
    case mk::QCC: {
        std::uint32_t comp = 0;
        auto* q = arena.make<Quantization>();
        if (!q) {
            return arena_failure(msg, arena);
        }
        st = read_qcc(msg, mh.numcomps, body, n, comp, *q);
        if (st == NJ2K_OK && !t->qcc.push(arena, CompValue<const Quantization*>{std::uint16_t(comp), q})) {
            return arena_failure(msg, arena);
        }
        break;
    }
    case mk::RGN: {
        std::uint32_t comp = 0;
        std::uint8_t shift = 0;
        st = read_rgn(msg, mh.numcomps, body, n, comp, shift);
        if (st == NJ2K_OK && !t->rgn.push(arena, CompValue<std::uint8_t>{std::uint16_t(comp), shift})) {
            return arena_failure(msg, arena);
        }
        break;
    }
    case mk::POC:
        // OpenJPEG's limit counts the main header's changes too
        st = read_poc(msg, arena, mh.numcomps, body, n, mh.poc.size + t->poc.size, t->poc);
        break;
    case mk::PPT:
        if (mh.has_ppm) {
            st = fail(msg, NJ2K_E_HEADER,
                      "Error reading PPT marker: packet header have been previously found in the main header "
                      "(PPM marker).\n");
        } else {
            st = add_packed(msg, arena, body, n, t->ppt, false);
        }
        break;
    case mk::PLT:
        st = check_plt(msg, body, n);
        break;
    case mk::COM:
        break;
    case mk::SOT:
    case mk::SIZ:
    case mk::TLM:
    case mk::PLM:
    case mk::PPM:
    case mk::CRG:
    case mk::CAP:
    case mk::CPF:
    case mk::SOP:
        st = fail(msg, NJ2K_E_HEADER, "Marker is not compliant with its position\n");
        break;
    default:
        // skipped by its length (OpenJPEG fails a tile-part header with an unknown marker)
        msg.emit(Messages::Warning, "Unknown marker 0x%04X (%u bytes) skipped in a tile-part header\n", unsigned(m),
                 n);
        break;
    }
    return st;
}

nj2k_status_t read_tile_part_header(Source& src, Arena& arena, const Messages& msg, const HeaderSettings& set,
                                    MainHeader& mh, std::uint64_t sot, TilePartHeader& out) noexcept
{
    std::uint8_t b[8] = {};
    if (src.read(b, 2) != 2) {
        return need_data(msg, "a SOT marker segment");
    }
    if (be16(b) != 10) {
        return fail(msg, NJ2K_E_HEADER, "Error reading SOT marker\n");
    }
    if (src.read(b, 8) != 8) {
        return need_data(msg, "a SOT marker segment");
    }
    TilePartHeader tp{};
    tp.sot = sot;
    tp.tile = be16(b);
    tp.psot = be32(b + 2);
    tp.tpsot = b[6];
    tp.tnsot = b[7];
    if (tp.tile >= mh.tile_count()) {
        return fail(msg, NJ2K_E_HEADER, "Invalid tile number %u\n", unsigned(tp.tile));
    }
    if (tp.psot != 0 && tp.psot < 14) {
        if (tp.psot != 12) {
            return fail(msg, NJ2K_E_HEADER, "Psot value is not correct regards to the JPEG2000 norm: %u.\n", tp.psot);
        }
        msg.emit(Messages::Warning, "Empty SOT marker detected: Psot=%u.\n", tp.psot);
    }
    // what Psot leaves for the tile-part's markers, its SOD and its data
    std::uint64_t room = tp.psot ? tp.psot - 12u : 0;
    for (;;) {
        if (src.read(b, 2) != 2) {
            return need_data(msg, "a tile-part header");
        }
        const std::uint16_t m = be16(b);
        if (m == mk::SOD) {
            break;
        }
        if (m < 0xFF00) {
            return fail(msg, NJ2K_E_HEADER, "A marker ID was expected (0xff--) instead of %.8x\n", unsigned(m));
        }
        if (segmentless(m)) {
            if (m >= 0xFF30 && m <= 0xFF3F) {
                if (tp.psot) {
                    if (room < 2) {
                        return fail(msg, NJ2K_E_HEADER, "Sot length is less than marker size + marker ID\n");
                    }
                    room -= 2;
                }
                msg.emit(Messages::Warning, "Marker 0x%04X without a segment skipped in a tile-part header\n",
                         unsigned(m));
                continue;
            }
            return fail(msg, NJ2K_E_HEADER, "Marker is not compliant with its position\n");
        }
        if (src.read(b, 2) != 2) {
            return need_data(msg, "a tile-part header");
        }
        const std::uint32_t length = be16(b);
        if (length < 2) {
            return fail(msg, NJ2K_E_HEADER, "Inconsistent marker size\n");
        }
        if (tp.psot) {
            if (room < std::uint64_t(length) + 2) {
                return fail(msg, NJ2K_E_HEADER, "Sot length is less than marker size + marker ID\n");
            }
            room -= std::uint64_t(length) + 2;
        }
        if (part2_marker(m)) {
            return fail(msg, NJ2K_E_UNSUPPORTED, "Marker 0x%04X is a JPEG 2000 Part 2 extension, not supported\n",
                        unsigned(m));
        }
        std::uint8_t* body = nullptr;
        const std::uint32_t n = length - 2;
        if (const nj2k_status_t st = read_body(src, arena, msg, n, body, "a tile-part header"); st != NJ2K_OK) {
            return st;
        }
        if (const nj2k_status_t st = read_tile_part_marker(arena, msg, set, mh, tp.tile, m, body, n); st != NJ2K_OK) {
            return st;
        }
    }
    tp.data = src.tell();
    if (tp.psot) {
        // SOD's two bytes come out of the tile-part's length too (when they fit, as OpenJPEG counts)
        tp.data_length = std::uint32_t(room >= 2 ? room - 2 : room);
    }
    out = tp;
    return NJ2K_OK;
}

// Marker precedence (W7): tile-part COC > tile-part COD > main COC > main COD, and the same for
// QCC/QCD and RGN; of several markers for one component, the last one read counts.
nj2k_status_t resolve_tile(const MainHeader& mh, const TileOverrides* tile, const Messages& msg,
                           TileComponentParams* out) noexcept
{
    for (std::uint32_t c = 0; c < mh.numcomps; ++c) {
        out[c] = component_defaults(mh, tile, c);
    }
    if (tile) {
        // in the order read, so the last marker for a component is the one that stays
        for (const auto& v : tile->coc) {
            out[v.comp].style = v.value;
        }
        for (const auto& v : tile->qcc) {
            out[v.comp].quant = v.value;
        }
        for (const auto& v : tile->rgn) {
            out[v.comp].roishift = v.value;
        }
    }
    for (std::uint32_t c = 0; c < mh.numcomps; ++c) {
        if (const nj2k_status_t st = check_step_count(out[c], tile, c, msg); st != NJ2K_OK) {
            return st;
        }
    }
    return NJ2K_OK;
}

nj2k_status_t resolve_tile_component(const MainHeader& mh, const TileOverrides* tile, std::uint32_t comp,
                                     const Messages& msg, TileComponentParams& out) noexcept
{
    TileComponentParams p = component_defaults(mh, tile, comp);
    if (tile) {
        if (const auto* v = last_comp_value(tile->coc, comp)) {
            p.style = v->value;
        }
        if (const auto* v = last_comp_value(tile->qcc, comp)) {
            p.quant = v->value;
        }
        if (const auto* v = last_comp_value(tile->rgn, comp)) {
            p.roishift = v->value;
        }
    }
    if (const nj2k_status_t st = check_step_count(p, tile, comp, msg); st != NJ2K_OK) {
        return st;
    }
    out = p;
    return NJ2K_OK;
}

Rect tile_rect(const MainHeader& mh, std::uint32_t tile) noexcept
{
    const std::uint64_t p = tile % mh.tw, q = tile / mh.tw;
    const std::uint64_t x0 = mh.tx0 + p * mh.tdx, y0 = mh.ty0 + q * mh.tdy;
    Rect r{};
    r.x0 = std::uint32_t(x0 > mh.x0 ? x0 : mh.x0);
    r.y0 = std::uint32_t(y0 > mh.y0 ? y0 : mh.y0);
    r.x1 = std::uint32_t(x0 + mh.tdx < mh.x1 ? x0 + mh.tdx : mh.x1);
    r.y1 = std::uint32_t(y0 + mh.tdy < mh.y1 ? y0 + mh.tdy : mh.y1);
    return r;
}

void header_component_geometry(const MainHeader& mh, std::uint32_t comp, std::uint32_t factor, std::uint32_t& x0,
                               std::uint32_t& y0, std::uint32_t& w, std::uint32_t& h) noexcept
{
    // opj_image_comp_header_update: the tile grid's area clipped to the image
    const std::uint32_t gx0 = mh.tx0 > mh.x0 ? mh.tx0 : mh.x0;
    const std::uint32_t gy0 = mh.ty0 > mh.y0 ? mh.ty0 : mh.y0;
    const std::uint64_t ex1 = std::uint64_t(mh.tx0) + std::uint64_t(mh.tw) * mh.tdx;
    const std::uint64_t ey1 = std::uint64_t(mh.ty0) + std::uint64_t(mh.th) * mh.tdy;
    const std::uint32_t gx1 = ex1 < mh.x1 ? std::uint32_t(ex1) : mh.x1;
    const std::uint32_t gy1 = ey1 < mh.y1 ? std::uint32_t(ey1) : mh.y1;
    const SizComponent& c = mh.comps[comp];
    const std::uint32_t cx0 = ceildiv(gx0, c.dx), cy0 = ceildiv(gy0, c.dy);
    const std::uint32_t cx1 = ceildiv(gx1, c.dx), cy1 = ceildiv(gy1, c.dy);
    x0 = cx0;
    y0 = cy0;
    w = ceildivpow2(cx1 - cx0, factor);
    h = ceildivpow2(cy1 - cy0, factor);
}

bool decoded_component_geometry(const MainHeader& mh, std::uint32_t comp, std::uint32_t factor, std::uint32_t& x0,
                                std::uint32_t& y0, std::uint32_t& w, std::uint32_t& h) noexcept
{
    constexpr std::uint32_t kIntMax = 0x7FFFFFFFu;
    if (mh.x0 > kIntMax || mh.y0 > kIntMax || mh.x1 > kIntMax || mh.y1 > kIntMax) {
        return false;
    }
    const SizComponent& c = mh.comps[comp];
    const std::uint32_t cx0 = ceildiv(mh.x0, c.dx), cy0 = ceildiv(mh.y0, c.dy);
    const std::uint32_t cx1 = ceildiv(mh.x1, c.dx), cy1 = ceildiv(mh.y1, c.dy);
    x0 = cx0;
    y0 = cy0;
    w = ceildivpow2(cx1, factor) - ceildivpow2(cx0, factor);
    h = ceildivpow2(cy1, factor) - ceildivpow2(cy0, factor);
    return true;
}

namespace {

// Bytes per item in the working-memory estimate. Stage 1c measured what decodes take from the
// allocator against it (decode_unit, test_memory_bound): the structures live in the working arena,
// which reserves blocks in a doubling series, so they count twice (kArenaReserve).
constexpr std::uint64_t kCodeBlockDecoded = 128;  // code-block state of a resolution that is decoded
constexpr std::uint64_t kCodeBlockHeader = 32;    // tag-tree nodes and Lblock of a dropped resolution
constexpr std::uint64_t kPrecinct = 64;
constexpr std::uint64_t kScratch = 256 * 1024;    // Tier-1 buffers and the decoder's own state
constexpr std::uint64_t kLineBytes = 64;          // wavelet lines: per sample of the longest side
constexpr std::uint64_t kArenaReserve = 2;        // what the working arena reserves for what it holds

std::uint64_t sat_add(std::uint64_t a, std::uint64_t b) noexcept
{
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

std::uint64_t sat_mul(std::uint64_t a, std::uint64_t b) noexcept
{
    if (((a | b) >> 32) == 0) {
        return a * b;  // both below 2^32: the product fits, no division needed
    }
    return a && b > UINT64_MAX / a ? UINT64_MAX : a * b;
}

// Cells of a 2^e grid anchored at 0 that [a, b) meets, plus one: an upper bound that does not
// depend on where the tile sits.
std::uint64_t cells(std::uint64_t a, std::uint64_t b, std::uint32_t e) noexcept
{
    if (a >= b) {
        return 0;
    }
    return ((b - a + (std::uint64_t(1) << e) - 1) >> e) + 1;
}

// Code-block and precinct state of one tile-component, over every resolution; most_precincts
// becomes the most precincts of any of its resolutions, if more.
std::uint64_t structure_bytes(const CodingStyle& s, std::uint32_t factor, std::uint64_t tcx0, std::uint64_t tcy0,
                              std::uint64_t tcx1, std::uint64_t tcy1, std::uint64_t& most_precincts) noexcept
{
    std::uint64_t bytes = 0;
    const std::uint32_t levels = s.numresolutions - 1u;
    for (std::uint32_t r = 0; r <= levels; ++r) {
        const std::uint32_t down = levels - r;
        const std::uint64_t rx0 = ceildivpow2(std::uint32_t(tcx0), down), ry0 = ceildivpow2(std::uint32_t(tcy0), down);
        const std::uint64_t rx1 = ceildivpow2(std::uint32_t(tcx1), down), ry1 = ceildivpow2(std::uint32_t(tcy1), down);
        const std::uint32_t ppx = s.precincts[r] & 0xF, ppy = s.precincts[r] >> 4;
        const std::uint64_t precincts = sat_mul(cells(rx0, rx1, ppx), cells(ry0, ry1, ppy));
        most_precincts = precincts > most_precincts ? precincts : most_precincts;
        bytes = sat_add(bytes, sat_mul(precincts, kPrecinct));
        // code-blocks never cross a precinct: above resolution 0 the bands are half the resolution
        const std::uint32_t gx = r ? (ppx ? ppx - 1 : 0) : ppx, gy = r ? (ppy ? ppy - 1 : 0) : ppy;
        const std::uint32_t cbw = s.cblkw < gx ? s.cblkw : gx, cbh = s.cblkh < gy ? s.cblkh : gy;
        const bool decoded = r + factor <= levels;
        const std::uint64_t each = decoded ? kCodeBlockDecoded : kCodeBlockHeader;
        if (r == 0) {
            bytes = sat_add(bytes, sat_mul(sat_mul(cells(rx0, rx1, cbw), cells(ry0, ry1, cbh)), each));
            continue;
        }
        // bands HL, LH, HH: each about half the resolution in each direction
        const std::uint64_t bw = (rx1 - rx0 + 1) / 2 + 1, bh = (ry1 - ry0 + 1) / 2 + 1;
        const std::uint64_t per_band = sat_mul(cells(0, bw, cbw), cells(0, bh, cbh));
        bytes = sat_add(bytes, sat_mul(sat_mul(per_band, 3), each));
    }
    return bytes;
}

// One tile's part of the working memory, with each component's coding style from style(c) and the
// tile's layers: the code-block and precinct state of every resolution, the packet iterator's table
// of packets met (a bit for each layer, plus one, resolution, component and precinct), and the
// tile's coefficients at the decoded level when there are several tiles (the coefficients of a
// single tile are the image's samples).
template <class Style>
std::uint64_t tile_part(const MainHeader& mh, const Rect& t, Style style, std::uint32_t layers, std::uint32_t factor,
                        const bool* selected) noexcept
{
    const bool several = mh.tile_count() > 1;
    std::uint64_t tile = 0, most_precincts = 0;
    std::uint32_t most_resolutions = 0;
    for (std::uint32_t c = 0; c < mh.numcomps; ++c) {
        const SizComponent& sc = mh.comps[c];
        const std::uint64_t tcx0 = ceildiv(t.x0, sc.dx), tcy0 = ceildiv(t.y0, sc.dy);
        const std::uint64_t tcx1 = ceildiv(t.x1, sc.dx), tcy1 = ceildiv(t.y1, sc.dy);
        const CodingStyle& s = style(c);
        most_resolutions = s.numresolutions > most_resolutions ? s.numresolutions : most_resolutions;
        tile = sat_add(tile,
                       sat_mul(structure_bytes(s, factor, tcx0, tcy0, tcx1, tcy1, most_precincts), kArenaReserve));
        if (several && (!selected || selected[c])) {
            // the tile's coefficients at the decoded level, before they go to the image
            const std::uint64_t w = ceildivpow2(std::uint32_t(tcx1), factor) - ceildivpow2(std::uint32_t(tcx0), factor);
            const std::uint64_t h = ceildivpow2(std::uint32_t(tcy1), factor) - ceildivpow2(std::uint32_t(tcy0), factor);
            tile = sat_add(tile, sat_mul(sat_mul(w, h), 4));
        }
    }
    const std::uint64_t table_bits =
        sat_mul(sat_mul(sat_mul(std::uint64_t(layers) + 1, most_resolutions), mh.numcomps), most_precincts);
    return sat_add(tile, sat_mul(table_bits / 8 + 16, kArenaReserve));
}

}  // namespace

std::uint64_t image_working_memory(const MainHeader& mh, std::uint32_t factor, const bool* selected) noexcept
{
    std::uint64_t total = kScratch;
    // the decoded samples: int32 per sample, whichever geometry rule the decode uses
    std::uint64_t longest = 1;
    for (std::uint32_t c = 0; c < mh.numcomps; ++c) {
        if (selected && !selected[c]) {
            continue;
        }
        std::uint32_t x0 = 0, y0 = 0, w1 = 0, h1 = 0, w2 = 0, h2 = 0;
        header_component_geometry(mh, c, factor, x0, y0, w1, h1);
        if (!decoded_component_geometry(mh, c, factor, x0, y0, w2, h2)) {
            w2 = w1;
            h2 = h1;
        }
        const std::uint64_t w = w1 > w2 ? w1 : w2, h = h1 > h2 ? h1 : h2;
        total = sat_add(total, sat_mul(sat_mul(w, h), 4));
        longest = w > longest ? w : longest;
        longest = h > longest ? h : longest;
    }
    return sat_add(total, sat_mul(longest, kLineBytes));
}

std::uint64_t tile_working_memory(const MainHeader& mh, std::uint32_t tile, const TileComponentParams* params,
                                  std::uint32_t factor, const bool* selected) noexcept
{
    return tile_part(mh, tile_rect(mh, tile), [params](std::uint32_t c) -> const CodingStyle& { return *params[c].style; },
                     params[0].coding->numlayers, factor, selected);
}

std::uint64_t decode_working_memory(const MainHeader& mh, std::uint32_t factor, const bool* selected) noexcept
{
    // the largest tile with the main header's coding: the first, an inner and the last column and
    // row of the grid, each once
    std::uint32_t cols[3] = {0, 0, 0}, rows[3] = {0, 0, 0}, ncols = 1, nrows = 1;
    if (mh.tw > 2) {
        cols[ncols++] = 1;
    }
    if (mh.tw > 1) {
        cols[ncols++] = mh.tw - 1;
    }
    if (mh.th > 2) {
        rows[nrows++] = 1;
    }
    if (mh.th > 1) {
        rows[nrows++] = mh.th - 1;
    }
    std::uint64_t largest = 0;
    for (std::uint32_t qi = 0; qi < nrows; ++qi) {
        for (std::uint32_t pi = 0; pi < ncols; ++pi) {
            const std::uint64_t tile =
                tile_part(mh, tile_rect(mh, rows[qi] * mh.tw + cols[pi]),
                          [&mh](std::uint32_t c) -> const CodingStyle& { return mh.main_style(c); }, mh.cod.numlayers,
                          factor, selected);
            largest = tile > largest ? tile : largest;
        }
    }
    return sat_add(image_working_memory(mh, factor, selected), largest);
}

}  // namespace nj2k::detail

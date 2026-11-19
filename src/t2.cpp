// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// Tier-2 decoding of a tile: the packet iterator of OpenJPEG 2.5.4's pi.c (opj_pi_create_decode
// and the five progression orders, POC included), its bit reader (bio.c, with the stage 1a
// overrun flag), its tag trees (tgt.c) and its packet reader (t2.c: opj_t2_decode_packets,
// opj_t2_read_packet_header, opj_t2_read_packet_data, opj_t2_skip_packet_data) at the oracle's
// pin, which keeps every complete codeword segment of a cut tile, treats a cut packet header as
// an absent packet, keeps packed packet headers in step and stops visiting packets once the
// tile's data has run out. Rewritten on neo-j2k's flat layout with the same order and the same
// arithmetic, so every packet header is read as OpenJPEG reads it. Departures: the exhaustion exit
// also applies in strict mode (owner decision 2026-11-19; with EPH markers it reports OpenJPEG's
// error at the next packet), and the packet iterator's table of packets seen is a bit set.
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
#include <climits>
#include <cstring>

#include "tile.h"

namespace nj2k::detail {

namespace {


enum Prog : std::uint32_t { LRCP = 0, RLCP = 1, RPCL = 2, PCRL = 3, CPRL = 4 };

inline std::uint32_t uint_min(std::uint32_t a, std::uint32_t b) noexcept
{
    return a < b ? a : b;
}

inline std::uint32_t uint_floordivpow2(std::uint32_t a, std::uint32_t b) noexcept
{
    return a >> b;
}

inline std::uint32_t uint64_ceildiv_res_uint32(std::uint64_t a, std::uint64_t b) noexcept
{
    return static_cast<std::uint32_t>((a + b - 1) / b);
}

inline std::uint32_t uint_ceildivpow2(std::uint32_t a, std::uint32_t b) noexcept
{
    return static_cast<std::uint32_t>((a + (std::uint64_t(1) << b) - 1U) >> b);
}

inline std::uint32_t uint_adds(std::uint32_t a, std::uint32_t b) noexcept
{
    const std::uint64_t sum = std::uint64_t(a) + b;
    return static_cast<std::uint32_t>(-static_cast<std::int32_t>(sum >> 32)) | static_cast<std::uint32_t>(sum);
}

inline std::uint32_t uint_floorlog2(std::uint32_t a) noexcept
{
    std::uint32_t l;
    for (l = 0; a > 1; ++l) {
        a >>= 1;
    }
    return l;
}

// ---- bio.c: the bit reader of packet headers ------------------------------------------------------

struct Bio {
    const std::uint8_t* start;
    const std::uint8_t* end;
    const std::uint8_t* bp;
    std::uint32_t buf, ct;
    bool overrun;

    void init(const std::uint8_t* p, std::uint32_t len) noexcept
    {
        start = bp = p;
        end = p + len;
        buf = 0;
        ct = 0;
        overrun = false;
    }
    bool bytein() noexcept
    {
        buf = (buf << 8) & 0xffff;
        ct = buf == 0xff00 ? 7 : 8;
        if (bp >= end) {
            overrun = true;
            return false;
        }
        buf |= *bp++;
        return true;
    }
    std::uint32_t getbit() noexcept
    {
        if (ct == 0) {
            bytein();
        }
        ct--;
        return (buf >> ct) & 1;
    }
    std::uint32_t read(std::uint32_t n) noexcept
    {
        std::uint32_t v = 0;
        for (std::int32_t i = static_cast<std::int32_t>(n) - 1; i >= 0; i--) {
            v |= getbit() << i;
        }
        return v;
    }
    bool inalign() noexcept
    {
        if ((buf & 0xff) == 0xff) {
            if (!bytein()) {
                return false;
            }
        }
        ct = 0;
        return true;
    }
    std::ptrdiff_t numbytes() const noexcept { return bp - start; }
};

std::uint32_t getcommacode(Bio& bio) noexcept
{
    std::uint32_t n = 0;
    while (bio.read(1)) {
        ++n;
    }
    return n;
}

std::uint32_t getnumpasses(Bio& bio) noexcept
{
    std::uint32_t n;
    if (!bio.read(1)) {
        return 1;
    }
    if (!bio.read(1)) {
        return 2;
    }
    if ((n = bio.read(2)) != 3) {
        return (3 + n);
    }
    if ((n = bio.read(5)) != 31) {
        return (6 + n);
    }
    return (37 + bio.read(7));
}

// ---- tgt.c: tag trees ------------------------------------------------------------------------------

void tgt_reset(TagTree& t) noexcept
{
    if (!t.nodes) {
        return;
    }
    std::uint32_t w = t.w, h = t.h;
    std::size_t total = 0;
    for (;;) {
        const std::size_t n = std::size_t(w) * h;
        total += n;
        if (n <= 1) {
            break;
        }
        w = (w + 1) / 2;
        h = (h + 1) / 2;
    }
    for (std::size_t i = 0; i < total; ++i) {
        t.nodes[i].value = 999;
        t.nodes[i].low = 0;
    }
}

// opj_tgt_decode: the path from the root down to the leaf, each node's value read until the
// threshold is reached
std::uint32_t tgt_decode(Bio& bio, TagTree& t, std::uint32_t leafno, std::int32_t threshold) noexcept
{
    TagNode* stk[32];
    std::uint32_t depth = 0;
    std::uint32_t w = t.w, h = t.h;
    std::uint32_t x = leafno % t.w, y = leafno / t.w;
    std::size_t base = 0;
    for (;;) {
        TagNode* node = &t.nodes[base + std::size_t(y) * w + x];
        stk[depth++] = node;
        const std::size_t n = std::size_t(w) * h;
        if (n <= 1) {
            break;
        }
        base += n;
        w = (w + 1) / 2;
        h = (h + 1) / 2;
        x >>= 1;
        y >>= 1;
    }
    // stk[0] is the leaf, stk[depth - 1] the root
    TagNode* node = stk[--depth];
    std::int32_t low = 0;
    for (;;) {
        if (low > node->low) {
            node->low = low;
        } else {
            low = node->low;
        }
        while (low < threshold && low < node->value) {
            if (bio.read(1)) {
                node->value = low;
            } else {
                ++low;
            }
        }
        node->low = low;
        if (depth == 0) {
            break;
        }
        node = stk[--depth];
    }
    return (node->value < threshold) ? 1 : 0;
}

// ---- pi.c: the packet iterator ---------------------------------------------------------------------

struct PiResolution {
    std::uint32_t pdx, pdy, pw, ph;
};

struct PiComp {
    std::uint32_t dx, dy, numresolutions;
    PiResolution* resolutions;
};

struct Poc {
    std::uint32_t resno0, compno0, layno0, precno0, resno1, compno1, layno1, precno1;
    std::uint32_t prg;
    std::uint32_t tx0, ty0, tx1, ty1;
};

// Packets already met (opj_pi_iterator_t::include, as bits): one per layer (numlayers + 1),
// resolution, component and precinct.
struct Included {
    std::uint64_t* bits;
    std::uint64_t size;
    bool test_and_set(std::uint64_t index) noexcept
    {
        std::uint64_t& w = bits[index >> 6];
        const std::uint64_t m = std::uint64_t(1) << (index & 63);
        const bool was = (w & m) != 0;
        w |= m;
        return was;
    }
};

struct Pi {
    Included* include;
    std::uint32_t step_l, step_r, step_c, step_p;
    std::uint32_t compno, resno, precno, layno;
    bool first;
    Poc poc;
    std::uint32_t numcomps;
    PiComp* comps;
    std::uint32_t tx0, ty0, tx1, ty1;
    std::uint32_t x, y, dx, dy;
    const Messages* msg;

    bool next() noexcept;
    bool next_lrcp() noexcept;
    bool next_rlcp() noexcept;
    bool next_rpcl() noexcept;
    bool next_pcrl() noexcept;
    bool next_cprl() noexcept;
    // the include table: true when the packet is new (and is marked), false when met before; an
    // index past the table ends the iterator with OpenJPEG's error
    int include_packet() noexcept
    {
        const std::uint64_t index = std::uint64_t(layno) * step_l + std::uint64_t(resno) * step_r +
                                    std::uint64_t(compno) * step_c + std::uint64_t(precno) * step_p;
        if (index >= include->size) {
            msg->emit(Messages::Error, "Invalid access to pi->include");
            return -1;
        }
        return include->test_and_set(index) ? 0 : 1;
    }
};

bool Pi::next_lrcp() noexcept
{
    PiComp* comp = nullptr;
    PiResolution* res = nullptr;
    if (poc.compno0 >= numcomps || poc.compno1 >= numcomps + 1) {
        msg->emit(Messages::Error, "opj_pi_next_lrcp(): invalid compno0/compno1\n");
        return false;
    }
    if (!first) {
        comp = &comps[compno];
        res = &comp->resolutions[resno];
        goto LABEL_SKIP;
    } else {
        first = false;
    }
    for (layno = poc.layno0; layno < poc.layno1; layno++) {
        for (resno = poc.resno0; resno < poc.resno1; resno++) {
            for (compno = poc.compno0; compno < poc.compno1; compno++) {
                comp = &comps[compno];
                if (resno >= comp->numresolutions) {
                    continue;
                }
                res = &comp->resolutions[resno];
                poc.precno1 = res->pw * res->ph;
                for (precno = poc.precno0; precno < poc.precno1; precno++) {
                    {
                        const int r = include_packet();
                        if (r < 0) {
                            return false;
                        }
                        if (r) {
                            return true;
                        }
                    }
                LABEL_SKIP:;
                }
            }
        }
    }
    return false;
}

bool Pi::next_rlcp() noexcept
{
    PiComp* comp = nullptr;
    PiResolution* res = nullptr;
    if (poc.compno0 >= numcomps || poc.compno1 >= numcomps + 1) {
        msg->emit(Messages::Error, "opj_pi_next_rlcp(): invalid compno0/compno1\n");
        return false;
    }
    if (!first) {
        comp = &comps[compno];
        res = &comp->resolutions[resno];
        goto LABEL_SKIP;
    } else {
        first = false;
    }
    for (resno = poc.resno0; resno < poc.resno1; resno++) {
        for (layno = poc.layno0; layno < poc.layno1; layno++) {
            for (compno = poc.compno0; compno < poc.compno1; compno++) {
                comp = &comps[compno];
                if (resno >= comp->numresolutions) {
                    continue;
                }
                res = &comp->resolutions[resno];
                poc.precno1 = res->pw * res->ph;
                for (precno = poc.precno0; precno < poc.precno1; precno++) {
                    {
                        const int r = include_packet();
                        if (r < 0) {
                            return false;
                        }
                        if (r) {
                            return true;
                        }
                    }
                LABEL_SKIP:;
                }
            }
        }
    }
    return false;
}

// The precinct grid step over every component and resolution (RPCL, PCRL) or over one component's
// resolutions (CPRL), as opj_pi_next_rpcl and its siblings compute it.
void pi_min_steps(const PiComp& comp, std::uint32_t& pdx, std::uint32_t& pdy) noexcept
{
    for (std::uint32_t resno = 0; resno < comp.numresolutions; resno++) {
        const PiResolution& r = comp.resolutions[resno];
        const std::uint32_t ex = r.pdx + comp.numresolutions - 1 - resno;
        const std::uint32_t ey = r.pdy + comp.numresolutions - 1 - resno;
        if (ex < 32 && comp.dx <= UINT_MAX / (1u << ex)) {
            const std::uint32_t dx = comp.dx * (1u << ex);
            pdx = !pdx ? dx : uint_min(pdx, dx);
        }
        if (ey < 32 && comp.dy <= UINT_MAX / (1u << ey)) {
            const std::uint32_t dy = comp.dy * (1u << ey);
            pdy = !pdy ? dy : uint_min(pdy, dy);
        }
    }
}

// The precinct of comp at resolution resno that the position (x, y) starts, as OpenJPEG decides it
// for the three position-driven orders; false when the position starts none.
bool pi_position_precinct(const Pi& pi, const PiComp* comp, std::uint32_t resno, std::uint32_t& precno) noexcept
{
    const PiResolution* res = &comp->resolutions[resno];
    const std::uint32_t levelno = comp->numresolutions - 1 - resno;
    if (static_cast<std::uint32_t>((std::uint64_t(comp->dx) << levelno) >> levelno) != comp->dx ||
        static_cast<std::uint32_t>((std::uint64_t(comp->dy) << levelno) >> levelno) != comp->dy) {
        return false;
    }
    const std::uint32_t trx0 = uint64_ceildiv_res_uint32(pi.tx0, std::uint64_t(comp->dx) << levelno);
    const std::uint32_t try0 = uint64_ceildiv_res_uint32(pi.ty0, std::uint64_t(comp->dy) << levelno);
    const std::uint32_t trx1 = uint64_ceildiv_res_uint32(pi.tx1, std::uint64_t(comp->dx) << levelno);
    const std::uint32_t try1 = uint64_ceildiv_res_uint32(pi.ty1, std::uint64_t(comp->dy) << levelno);
    const std::uint32_t rpx = res->pdx + levelno;
    const std::uint32_t rpy = res->pdy + levelno;
    if (static_cast<std::uint32_t>((std::uint64_t(comp->dx) << rpx) >> rpx) != comp->dx ||
        static_cast<std::uint32_t>((std::uint64_t(comp->dy) << rpy) >> rpy) != comp->dy) {
        return false;
    }
    // ISO 15444-1 B.12.1.3 to B.12.1.5
    if (!((std::uint64_t(pi.y) % (std::uint64_t(comp->dy) << rpy) == 0) ||
          ((pi.y == pi.ty0) && ((std::uint64_t(try0) << levelno) % (std::uint64_t(1U) << rpy))))) {
        return false;
    }
    if (!((std::uint64_t(pi.x) % (std::uint64_t(comp->dx) << rpx) == 0) ||
          ((pi.x == pi.tx0) && ((std::uint64_t(trx0) << levelno) % (std::uint64_t(1U) << rpx))))) {
        return false;
    }
    if ((res->pw == 0) || (res->ph == 0)) {
        return false;
    }
    if ((trx0 == trx1) || (try0 == try1)) {
        return false;
    }
    const std::uint32_t prci =
        uint_floordivpow2(uint64_ceildiv_res_uint32(pi.x, std::uint64_t(comp->dx) << levelno), res->pdx) -
        uint_floordivpow2(trx0, res->pdx);
    const std::uint32_t prcj =
        uint_floordivpow2(uint64_ceildiv_res_uint32(pi.y, std::uint64_t(comp->dy) << levelno), res->pdy) -
        uint_floordivpow2(try0, res->pdy);
    precno = prci + prcj * res->pw;
    return true;
}

bool Pi::next_rpcl() noexcept
{
    PiComp* comp = nullptr;
    if (poc.compno0 >= numcomps || poc.compno1 >= numcomps + 1) {
        msg->emit(Messages::Error, "opj_pi_next_rpcl(): invalid compno0/compno1\n");
        return false;
    }
    if (!first) {
        goto LABEL_SKIP;
    } else {
        first = false;
        dx = 0;
        dy = 0;
        for (std::uint32_t c = 0; c < numcomps; c++) {
            pi_min_steps(comps[c], dx, dy);
        }
        if (dx == 0 || dy == 0) {
            return false;
        }
    }
    poc.ty0 = ty0;
    poc.tx0 = tx0;
    poc.ty1 = ty1;
    poc.tx1 = tx1;
    for (resno = poc.resno0; resno < poc.resno1; resno++) {
        for (y = poc.ty0; y < poc.ty1; y += (dy - (y % dy))) {
            for (x = poc.tx0; x < poc.tx1; x += (dx - (x % dx))) {
                for (compno = poc.compno0; compno < poc.compno1; compno++) {
                    comp = &comps[compno];
                    if (resno >= comp->numresolutions) {
                        continue;
                    }
                    if (!pi_position_precinct(*this, comp, resno, precno)) {
                        continue;
                    }
                    for (layno = poc.layno0; layno < poc.layno1; layno++) {
                        {
                            const int r = include_packet();
                            if (r < 0) {
                                return false;
                            }
                            if (r) {
                                return true;
                            }
                        }
                    LABEL_SKIP:;
                    }
                }
            }
        }
    }
    return false;
}

bool Pi::next_pcrl() noexcept
{
    PiComp* comp = nullptr;
    if (poc.compno0 >= numcomps || poc.compno1 >= numcomps + 1) {
        msg->emit(Messages::Error, "opj_pi_next_pcrl(): invalid compno0/compno1\n");
        return false;
    }
    if (!first) {
        comp = &comps[compno];
        goto LABEL_SKIP;
    } else {
        first = false;
        dx = 0;
        dy = 0;
        for (std::uint32_t c = 0; c < numcomps; c++) {
            pi_min_steps(comps[c], dx, dy);
        }
        if (dx == 0 || dy == 0) {
            return false;
        }
    }
    poc.ty0 = ty0;
    poc.tx0 = tx0;
    poc.ty1 = ty1;
    poc.tx1 = tx1;
    for (y = poc.ty0; y < poc.ty1; y += (dy - (y % dy))) {
        for (x = poc.tx0; x < poc.tx1; x += (dx - (x % dx))) {
            for (compno = poc.compno0; compno < poc.compno1; compno++) {
                comp = &comps[compno];
                for (resno = poc.resno0; resno < uint_min(poc.resno1, comp->numresolutions); resno++) {
                    if (!pi_position_precinct(*this, comp, resno, precno)) {
                        continue;
                    }
                    for (layno = poc.layno0; layno < poc.layno1; layno++) {
                        {
                            const int r = include_packet();
                            if (r < 0) {
                                return false;
                            }
                            if (r) {
                                return true;
                            }
                        }
                    LABEL_SKIP:;
                    }
                }
            }
        }
    }
    return false;
}

bool Pi::next_cprl() noexcept
{
    PiComp* comp = nullptr;
    if (poc.compno0 >= numcomps || poc.compno1 >= numcomps + 1) {
        msg->emit(Messages::Error, "opj_pi_next_cprl(): invalid compno0/compno1\n");
        return false;
    }
    if (!first) {
        comp = &comps[compno];
        goto LABEL_SKIP;
    } else {
        first = false;
    }
    for (compno = poc.compno0; compno < poc.compno1; compno++) {
        comp = &comps[compno];
        dx = 0;
        dy = 0;
        pi_min_steps(*comp, dx, dy);
        if (dx == 0 || dy == 0) {
            return false;
        }
        poc.ty0 = ty0;
        poc.tx0 = tx0;
        poc.ty1 = ty1;
        poc.tx1 = tx1;
        for (y = poc.ty0; y < poc.ty1; y += (dy - (y % dy))) {
            for (x = poc.tx0; x < poc.tx1; x += (dx - (x % dx))) {
                for (resno = poc.resno0; resno < uint_min(poc.resno1, comp->numresolutions); resno++) {
                    if (!pi_position_precinct(*this, comp, resno, precno)) {
                        continue;
                    }
                    for (layno = poc.layno0; layno < poc.layno1; layno++) {
                        {
                            const int r = include_packet();
                            if (r < 0) {
                                return false;
                            }
                            if (r) {
                                return true;
                            }
                        }
                    LABEL_SKIP:;
                    }
                }
            }
        }
    }
    return false;
}

bool Pi::next() noexcept
{
    switch (poc.prg) {
    case LRCP:
        return next_lrcp();
    case RLCP:
        return next_rlcp();
    case RPCL:
        return next_rpcl();
    case PCRL:
        return next_pcrl();
    case CPRL:
        return next_cprl();
    default:
        return false;
    }
}

// ---- t2.c: packets ---------------------------------------------------------------------------------

struct T2 {
    const Tile* tile;
    const TileInput* in;
    WorkArena* arena;
    const Messages* msg;
    std::uint8_t csty;
    bool strict;
    bool sop_warned = false;
    bool cut_warned = false;
    PackedCursor* ppt = nullptr;  // the tile's packed packet headers, when it has PPT markers
    bool nomem = false;
};

// opj_t2_init_seg
bool init_seg(T2& t2, CodeBlock* cblk, std::uint32_t index, std::uint32_t cblksty, std::uint32_t first) noexcept
{
    const std::uint32_t nb_segs = index + 1;
    if (nb_segs > cblk->segs_cap) {
        // the segments already made keep their state (OpenJPEG's realloc grows by 10 at a time)
        if (!grow(*t2.arena, cblk->segs, cblk->segs_cap, cblk->segs_cap, nb_segs)) {
            t2.nomem = true;
            return false;
        }
    }
    Segment* seg = &cblk->segs[index];
    std::memset(seg, 0, sizeof(Segment));
    if (cblksty & kCblkTermall) {
        seg->maxpasses = 1;
    } else if (cblksty & kCblkLazy) {
        if (first) {
            seg->maxpasses = 10;
        } else {
            seg->maxpasses = (((seg - 1)->maxpasses == 1) || ((seg - 1)->maxpasses == 10)) ? 2 : 1;
        }
    } else {
        seg->maxpasses = 109;
    }
    return true;
}

// opj_t2_drop_new_passes
void drop_new_passes(const Resolution* res, std::uint32_t precno) noexcept
{
    for (std::uint32_t bandno = 0; bandno < res->numbands; ++bandno) {
        const Band* band = &res->bands[bandno];
        if (band->empty()) {
            continue;
        }
        Precinct* prc = &band->precincts[precno];
        CodeBlock* cblk = prc->cblks;
        const std::uint32_t n = prc->cw * prc->ch;
        for (std::uint32_t cblkno = 0; cblkno < n; ++cblkno, ++cblk) {
            if (cblk->numnewpasses) {
                cblk->corrupted = true;
                cblk->numnewpasses = 0;
            }
        }
    }
}

// opj_t2_first_new_seg
Segment* first_new_seg(CodeBlock* cblk) noexcept
{
    Segment* seg;
    if (!cblk->numsegs) {
        seg = cblk->segs;
        ++cblk->numsegs;
    } else {
        seg = &cblk->segs[cblk->numsegs - 1];
        if (seg->numpasses == seg->maxpasses) {
            ++seg;
            ++cblk->numsegs;
        }
    }
    return seg;
}

// opj_t2_skip_cblk_passes
void skip_cblk_passes(CodeBlock* cblk, Segment* seg) noexcept
{
    for (;;) {
        seg->numpasses += seg->numnewpasses;
        cblk->numnewpasses -= seg->numnewpasses;
        if (cblk->numnewpasses == 0) {
            break;
        }
        ++seg;
        ++cblk->numsegs;
    }
}

// opj_t2_consume_header_source
inline void consume_header_source(const std::uint8_t** start, std::uint32_t* length) noexcept
{
    *start += *length;
    *length = 0;
}

struct PacketPos {
    std::uint32_t compno, resno, precno, layno;
};

// opj_t2_read_packet_header; false is a failure (an error message given)
bool read_packet_header(T2& t2, const PacketPos& pi, bool& is_data_present, const std::uint8_t* p_src_data,
                        std::uint32_t& data_read, std::uint32_t p_max_length) noexcept
{
    const TileComp& tilec = t2.tile->comps[pi.compno];
    Resolution* l_res = &tilec.res[pi.resno];
    const std::uint32_t cblksty = tilec.params.style->cblksty;
    const std::uint8_t* l_current_data = p_src_data;
    std::uint32_t l_remaining_length = 0;
    const std::uint8_t** l_header_data_start = nullptr;
    std::uint32_t* l_modified_length_ptr = nullptr;

    if (pi.layno == 0) {
        // reset the tag trees
        for (std::uint32_t bandno = 0; bandno < l_res->numbands; ++bandno) {
            Band* band = &l_res->bands[bandno];
            if (!band->empty()) {
                Precinct* prc = &band->precincts[pi.precno];
                tgt_reset(prc->incl);
                tgt_reset(prc->imsb);
                CodeBlock* cblk = prc->cblks;
                const std::uint32_t n = prc->cw * prc->ch;
                for (std::uint32_t cblkno = 0; cblkno < n; ++cblkno, ++cblk) {
                    cblk->numsegs = 0;
                    cblk->real_num_segs = 0;
                }
            }
        }
    }

    // SOP markers are optional: a warning, once per tile (CVE-2023-39327)
    if (t2.csty & kCstySop) {
        if (p_max_length < 6) {
            if (!t2.sop_warned) {
                t2.sop_warned = true;
                t2.msg->emit(Messages::Warning, "Not enough space for expected SOP marker\n");
            }
        } else if ((*l_current_data) != 0xff || (*(l_current_data + 1) != 0x91)) {
            if (!t2.sop_warned) {
                t2.sop_warned = true;
                t2.msg->emit(Messages::Warning, "Expected SOP marker\n");
            }
        } else {
            l_current_data += 6;
        }
    }

    Bio bio;
    if (t2.in->ppm) {
        l_header_data_start = &t2.in->ppm->data;
        l_modified_length_ptr = &t2.in->ppm->len;
    } else if (t2.ppt) {
        l_header_data_start = &t2.ppt->data;
        l_modified_length_ptr = &t2.ppt->len;
    } else {
        l_header_data_start = &l_current_data;
        l_remaining_length = static_cast<std::uint32_t>(p_src_data + p_max_length - l_current_data);
        l_modified_length_ptr = &l_remaining_length;
    }
    const std::uint8_t* l_header_data = *l_header_data_start;
    bio.init(l_header_data, *l_modified_length_ptr);

    const std::uint32_t l_present = bio.read(1);
    if (!l_present) {
        const bool l_cut = !bio.inalign() || bio.overrun;
        l_header_data += bio.numbytes();
        if (l_cut && !t2.strict) {
            // the data ended inside this packet header: the packet is absent, and the rest of the
            // tile's data and of the packet headers counts as read
            consume_header_source(l_header_data_start, l_modified_length_ptr);
            is_data_present = false;
            data_read = p_max_length;
            return true;
        }
        if (t2.csty & kCstyEph) {
            if ((*l_modified_length_ptr - static_cast<std::uint32_t>(l_header_data - *l_header_data_start)) < 2U) {
                if (!t2.strict) {
                    consume_header_source(l_header_data_start, l_modified_length_ptr);
                    is_data_present = false;
                    data_read = p_max_length;
                    return true;
                }
                t2.msg->emit(Messages::Error, "Not enough space for required EPH marker\n");
                return false;
            } else if ((*l_header_data) != 0xff || (*(l_header_data + 1) != 0x92)) {
                t2.msg->emit(Messages::Error, "Expected EPH marker\n");
                return false;
            } else {
                l_header_data += 2;
            }
        }
        const std::uint32_t l_header_length = static_cast<std::uint32_t>(l_header_data - *l_header_data_start);
        *l_modified_length_ptr -= l_header_length;
        *l_header_data_start += l_header_length;
        is_data_present = false;
        data_read = static_cast<std::uint32_t>(l_current_data - p_src_data);
        return true;
    }

    for (std::uint32_t bandno = 0; bandno < l_res->numbands; ++bandno) {
        Band* band = &l_res->bands[bandno];
        Precinct* prc = &band->precincts[pi.precno];
        if (band->empty()) {
            continue;
        }
        const std::uint32_t l_nb_code_blocks = prc->cw * prc->ch;
        CodeBlock* l_cblk = prc->cblks;
        for (std::uint32_t cblkno = 0; cblkno < l_nb_code_blocks; cblkno++) {
            std::uint32_t l_included, l_increment, l_segno;
            std::int32_t n;
            // a code-block not yet included: the inclusion tag tree, else one bit
            if (!l_cblk->numsegs) {
                l_included = tgt_decode(bio, prc->incl, cblkno, static_cast<std::int32_t>(pi.layno + 1));
            } else {
                l_included = bio.read(1);
            }
            if (!l_included) {
                l_cblk->numnewpasses = 0;
                ++l_cblk;
                continue;
            }
            // a code-block not yet included: the zero bit-plane tag tree
            if (!l_cblk->numsegs) {
                std::uint32_t i = 0;
                while (!tgt_decode(bio, prc->imsb, cblkno, static_cast<std::int32_t>(i))) {
                    ++i;
                }
                if (static_cast<std::uint32_t>(band->numbps) + 1 < i) {
                    // as OpenJPEG (pull request 1488): no overflow, a large count
                    l_cblk->numbps = static_cast<std::uint32_t>(band->numbps + 1 - static_cast<int>(i));
                } else {
                    l_cblk->numbps = static_cast<std::uint32_t>(band->numbps) + 1 - i;
                }
                l_cblk->numlenbits = 3;
            }
            l_cblk->numnewpasses = getnumpasses(bio);
            l_increment = getcommacode(bio);
            l_cblk->numlenbits += l_increment;
            l_segno = 0;
            if (!l_cblk->numsegs) {
                if (!init_seg(t2, l_cblk, l_segno, cblksty, 1)) {
                    return false;
                }
            } else {
                l_segno = l_cblk->numsegs - 1;
                if (l_cblk->segs[l_segno].numpasses == l_cblk->segs[l_segno].maxpasses) {
                    ++l_segno;
                    if (!init_seg(t2, l_cblk, l_segno, cblksty, 0)) {
                        return false;
                    }
                }
            }
            n = static_cast<std::int32_t>(l_cblk->numnewpasses);
            do {
                Segment* s = &l_cblk->segs[l_segno];
                const std::int32_t room = static_cast<std::int32_t>(s->maxpasses - s->numpasses);
                s->numnewpasses = static_cast<std::uint32_t>(room < n ? room : n);
                const std::uint32_t bit_number = l_cblk->numlenbits + uint_floorlog2(s->numnewpasses);
                if (bit_number > 32) {
                    t2.msg->emit(Messages::Error, "Invalid bit number %u in opj_t2_read_packet_header()\n",
                                 bit_number);
                    return false;
                }
                s->newlen = bio.read(bit_number);
                n -= static_cast<std::int32_t>(s->numnewpasses);
                if (n > 0) {
                    ++l_segno;
                    if (!init_seg(t2, l_cblk, l_segno, cblksty, 0)) {
                        return false;
                    }
                }
            } while (n > 0);
            ++l_cblk;
        }
    }

    {
        const bool l_aligned = bio.inalign();
        if ((!l_aligned || bio.overrun) && !t2.strict) {
            // the data ended inside this packet header: the packet is absent, everything earlier
            // packets gave stays, and the rest of the tile's data and of the packet headers counts
            // as read
            drop_new_passes(l_res, pi.precno);
            consume_header_source(l_header_data_start, l_modified_length_ptr);
            is_data_present = false;
            data_read = p_max_length;
            return true;
        }
        if (!l_aligned) {
            return false;
        }
    }
    l_header_data += bio.numbytes();

    if (t2.csty & kCstyEph) {
        if ((*l_modified_length_ptr - static_cast<std::uint32_t>(l_header_data - *l_header_data_start)) < 2U) {
            if (!t2.strict) {
                drop_new_passes(l_res, pi.precno);
                consume_header_source(l_header_data_start, l_modified_length_ptr);
                is_data_present = false;
                data_read = p_max_length;
                return true;
            }
            t2.msg->emit(Messages::Error, "Not enough space for required EPH marker\n");
            return false;
        } else if ((*l_header_data) != 0xff || (*(l_header_data + 1) != 0x92)) {
            t2.msg->emit(Messages::Error, "Expected EPH marker\n");
            return false;
        } else {
            l_header_data += 2;
        }
    }

    const std::uint32_t l_header_length = static_cast<std::uint32_t>(l_header_data - *l_header_data_start);
    if (!l_header_length) {
        return false;
    }
    *l_modified_length_ptr -= l_header_length;
    *l_header_data_start += l_header_length;
    is_data_present = true;
    data_read = static_cast<std::uint32_t>(l_current_data - p_src_data);
    return true;
}

// opj_t2_read_packet_data
bool read_packet_data(T2& t2, const PacketPos& pi, const std::uint8_t* p_src_data, std::uint32_t& data_read,
                      std::uint32_t p_max_length) noexcept
{
    const TileComp& tilec = t2.tile->comps[pi.compno];
    const Resolution* l_res = &tilec.res[pi.resno];
    const std::uint8_t* l_current_data = p_src_data;
    bool partial_buffer = false;
    for (std::uint32_t bandno = 0; bandno < l_res->numbands; ++bandno) {
        const Band* band = &l_res->bands[bandno];
        Precinct* prc = &band->precincts[pi.precno];
        if (band->empty()) {
            continue;
        }
        const std::uint32_t l_nb_code_blocks = prc->cw * prc->ch;
        CodeBlock* l_cblk = prc->cblks;
        for (std::uint32_t cblkno = 0; cblkno < l_nb_code_blocks; ++cblkno, ++l_cblk) {
            if (!l_cblk->numnewpasses) {
                continue;
            }
            Segment* l_seg = first_new_seg(l_cblk);
            if (partial_buffer || l_cblk->corrupted) {
                // the packet's data ended before this contribution, or the code-block lost passes
                // of an earlier packet: these passes are left out, everything earlier packets gave
                // stays, and the passes count in the header state so packed headers stay in step
                skip_cblk_passes(l_cblk, l_seg);
                l_cblk->corrupted = true;
                continue;
            }
            do {
                if ((std::uintptr_t(l_current_data) + l_seg->newlen < std::uintptr_t(l_current_data)) ||
                    (l_current_data + l_seg->newlen > p_src_data + p_max_length) || partial_buffer) {
                    if (t2.strict) {
                        t2.msg->emit(Messages::Error,
                                     "read: segment too long (%u) with max (%u) for codeblock %u (p=%u, b=%u, r=%u, "
                                     "c=%u)\n",
                                     l_seg->newlen, p_max_length, cblkno, pi.precno, bandno, pi.resno, pi.compno);
                        return false;
                    }
                    if (!t2.cut_warned) {
                        t2.cut_warned = true;
                        t2.msg->emit(Messages::Warning,
                                     "read: segment too long (%u) with max (%u) for codeblock %u (p=%u, b=%u, r=%u, "
                                     "c=%u)\n",
                                     l_seg->newlen, p_max_length, cblkno, pi.precno, bandno, pi.resno, pi.compno);
                    }
                    // the data ended inside this contribution: keep the segments it completed
                    // before the cut and everything earlier packets gave; the code-block takes no
                    // more passes
                    partial_buffer = true;
                    l_cblk->real_num_segs = l_cblk->numsegs - (l_seg->numpasses == 0U ? 1U : 0U);
                    skip_cblk_passes(l_cblk, l_seg);
                    l_cblk->corrupted = true;
                    break;
                }
                if (!grow(*t2.arena, l_cblk->chunks, l_cblk->nchunks, l_cblk->chunks_cap, l_cblk->nchunks + 1)) {
                    t2.nomem = true;
                    t2.msg->emit(Messages::Error, "cannot allocate opj_tcd_seg_data_chunk_t* array");
                    return false;
                }
                l_cblk->chunks[l_cblk->nchunks].data = l_current_data;
                l_cblk->chunks[l_cblk->nchunks].len = l_seg->newlen;
                l_cblk->nchunks++;
                l_current_data += l_seg->newlen;
                l_seg->len += l_seg->newlen;
                l_seg->numpasses += l_seg->numnewpasses;
                l_cblk->numnewpasses -= l_seg->numnewpasses;
                l_seg->real_num_passes = l_seg->numpasses;
                if (l_cblk->numnewpasses > 0) {
                    ++l_seg;
                    ++l_cblk->numsegs;
                }
            } while (l_cblk->numnewpasses > 0);
            if (!partial_buffer) {
                l_cblk->real_num_segs = l_cblk->numsegs;
            }
        }
    }
    data_read = partial_buffer ? p_max_length : static_cast<std::uint32_t>(l_current_data - p_src_data);
    return true;
}

// opj_t2_skip_packet_data
bool skip_packet_data(T2& t2, const PacketPos& pi, std::uint32_t& data_read, std::uint32_t p_max_length) noexcept
{
    const TileComp& tilec = t2.tile->comps[pi.compno];
    const Resolution* l_res = &tilec.res[pi.resno];
    bool l_cut = false;
    data_read = 0;
    for (std::uint32_t bandno = 0; bandno < l_res->numbands; ++bandno) {
        const Band* band = &l_res->bands[bandno];
        Precinct* prc = &band->precincts[pi.precno];
        if (band->empty()) {
            continue;
        }
        const std::uint32_t l_nb_code_blocks = prc->cw * prc->ch;
        CodeBlock* l_cblk = prc->cblks;
        for (std::uint32_t cblkno = 0; cblkno < l_nb_code_blocks; ++cblkno, ++l_cblk) {
            if (!l_cblk->numnewpasses) {
                continue;
            }
            Segment* l_seg = first_new_seg(l_cblk);
            if (l_cut) {
                skip_cblk_passes(l_cblk, l_seg);
                continue;
            }
            do {
                if (((data_read + l_seg->newlen) < data_read) || ((data_read + l_seg->newlen) > p_max_length)) {
                    if (t2.strict) {
                        t2.msg->emit(Messages::Error,
                                     "skip: segment too long (%u) with max (%u) for codeblock %u (p=%u, b=%u, r=%u, "
                                     "c=%u)\n",
                                     l_seg->newlen, p_max_length, cblkno, pi.precno, bandno, pi.resno, pi.compno);
                        return false;
                    }
                    if (!t2.cut_warned) {
                        t2.cut_warned = true;
                        t2.msg->emit(Messages::Warning,
                                     "skip: segment too long (%u) with max (%u) for codeblock %u (p=%u, b=%u, r=%u, "
                                     "c=%u)\n",
                                     l_seg->newlen, p_max_length, cblkno, pi.precno, bandno, pi.resno, pi.compno);
                    }
                    // the data ended inside this contribution: the rest of the tile's data counts
                    // as read; the passes still count in the header state
                    l_cut = true;
                    skip_cblk_passes(l_cblk, l_seg);
                    break;
                }
                data_read += l_seg->newlen;
                l_seg->numpasses += l_seg->numnewpasses;
                l_cblk->numnewpasses -= l_seg->numnewpasses;
                if (l_cblk->numnewpasses > 0) {
                    ++l_seg;
                    ++l_cblk->numsegs;
                }
            } while (l_cblk->numnewpasses > 0);
        }
    }
    if (l_cut) {
        data_read = p_max_length;
    }
    return true;
}

// opj_t2_decode_packet / opj_t2_skip_packet
bool visit_packet(T2& t2, const PacketPos& pi, const std::uint8_t* p_src, std::uint32_t& p_data_read,
                  std::uint32_t p_max_length, bool skip) noexcept
{
    bool l_read_data = false;
    std::uint32_t l_nb_bytes_read = 0;
    std::uint32_t l_nb_total_bytes_read = 0;
    p_data_read = 0;
    if (!read_packet_header(t2, pi, l_read_data, p_src, l_nb_bytes_read, p_max_length)) {
        return false;
    }
    p_src += l_nb_bytes_read;
    l_nb_total_bytes_read += l_nb_bytes_read;
    p_max_length -= l_nb_bytes_read;
    if (l_read_data) {
        l_nb_bytes_read = 0;
        const bool ok = skip ? skip_packet_data(t2, pi, l_nb_bytes_read, p_max_length)
                             : read_packet_data(t2, pi, p_src, l_nb_bytes_read, p_max_length);
        if (!ok) {
            return false;
        }
        l_nb_total_bytes_read += l_nb_bytes_read;
    }
    p_data_read = l_nb_total_bytes_read;
    return true;
}

// opj_tcd_is_subband_area_of_interest for a whole-tile decode: the area of interest is the whole
// tile-component, widened by the filter's margin
bool area_of_interest(const TileComp& tilec, std::uint32_t resno, std::uint32_t bandno, std::uint32_t band_x0,
                      std::uint32_t band_y0, std::uint32_t band_x1, std::uint32_t band_y1) noexcept
{
    const std::uint32_t filter_margin = (tilec.params.style->qmfbid == 1) ? 2 : 3;
    const std::uint32_t tcx0 = static_cast<std::uint32_t>(tilec.x0), tcy0 = static_cast<std::uint32_t>(tilec.y0);
    const std::uint32_t tcx1 = static_cast<std::uint32_t>(tilec.x1), tcy1 = static_cast<std::uint32_t>(tilec.y1);
    const std::uint32_t nb = (resno == 0) ? tilec.numresolutions - 1 : tilec.numresolutions - resno;
    const std::uint32_t x0b = bandno & 1;
    const std::uint32_t y0b = bandno >> 1;
    std::uint32_t tbx0 = (nb == 0) ? tcx0
                         : (tcx0 <= (1U << (nb - 1)) * x0b) ? 0
                                                             : uint_ceildivpow2(tcx0 - (1U << (nb - 1)) * x0b, nb);
    std::uint32_t tby0 = (nb == 0) ? tcy0
                         : (tcy0 <= (1U << (nb - 1)) * y0b) ? 0
                                                             : uint_ceildivpow2(tcy0 - (1U << (nb - 1)) * y0b, nb);
    std::uint32_t tbx1 = (nb == 0) ? tcx1
                         : (tcx1 <= (1U << (nb - 1)) * x0b) ? 0
                                                             : uint_ceildivpow2(tcx1 - (1U << (nb - 1)) * x0b, nb);
    std::uint32_t tby1 = (nb == 0) ? tcy1
                         : (tcy1 <= (1U << (nb - 1)) * y0b) ? 0
                                                             : uint_ceildivpow2(tcy1 - (1U << (nb - 1)) * y0b, nb);
    tbx0 = tbx0 < filter_margin ? 0 : tbx0 - filter_margin;
    tby0 = tby0 < filter_margin ? 0 : tby0 - filter_margin;
    tbx1 = uint_adds(tbx1, filter_margin);
    tby1 = uint_adds(tby1, filter_margin);
    return band_x0 < tbx1 && band_y0 < tby1 && band_x1 > tbx0 && band_y1 > tby0;
}

}  // namespace

nj2k_status_t Tile::read_packets(WorkArena& arena, const Messages& msg, std::uint32_t* resno_decoded) noexcept
{
    const MainHeader& mh = *in->mh;
    const TileComponentParams& p0 = in->params[0];
    const TileCoding& coding = *p0.coding;
    const TileOverrides* ov = mh.overrides(in->tile);
    const std::uint32_t numlayers = coding.numlayers;
    const std::uint32_t layers_to_decode = in->layers ? in->layers : numlayers;

    // the progression changes: the tile's own replace the main header's, as ISO 15444-1 A.6.6 has
    // it (OpenJPEG appends the tile's to the main header's: departure DP-12)
    const ArenaList<PocEntry>* pocs = (ov && ov->poc.size) ? &ov->poc : &mh.poc;
    const std::uint32_t npocs = pocs->size;
    const std::uint32_t bound = npocs ? npocs : 1;

    // opj_get_all_encoding_parameters: the tile's area, the most resolutions and precincts
    const std::uint32_t p = in->tile % mh.tw, q = in->tile / mh.tw;
    const std::uint32_t l_tx0 = mh.tx0 + p * mh.tdx, l_ty0 = mh.ty0 + q * mh.tdy;
    const std::uint32_t tx0_ = l_tx0 > mh.x0 ? l_tx0 : mh.x0;
    const std::uint32_t tx1_ = uint_min(uint_adds(l_tx0, mh.tdx), mh.x1);
    const std::uint32_t ty0_ = l_ty0 > mh.y0 ? l_ty0 : mh.y0;
    const std::uint32_t ty1_ = uint_min(uint_adds(l_ty0, mh.tdy), mh.y1);
    std::uint32_t max_prec = 0, max_res = 0;
    PiComp* pcomps = arena.make_array<PiComp>(numcomps);
    if (!pcomps) {
        return arena.budget_failed() ? NJ2K_E_LIMIT : NJ2K_E_NO_MEMORY;
    }
    for (std::uint32_t compno = 0; compno < numcomps; ++compno) {
        const SizComponent& sc = mh.comps[compno];
        const CodingStyle& style = *in->params[compno].style;
        PiComp& pc = pcomps[compno];
        pc.dx = sc.dx;
        pc.dy = sc.dy;
        pc.numresolutions = style.numresolutions;
        pc.resolutions = arena.make_array<PiResolution>(style.numresolutions);
        if (!pc.resolutions) {
            return arena.budget_failed() ? NJ2K_E_LIMIT : NJ2K_E_NO_MEMORY;
        }
        const std::uint32_t tcx0 = ceildiv(tx0_, sc.dx), tcy0 = ceildiv(ty0_, sc.dy);
        const std::uint32_t tcx1 = ceildiv(tx1_, sc.dx), tcy1 = ceildiv(ty1_, sc.dy);
        if (style.numresolutions > max_res) {
            max_res = style.numresolutions;
        }
        std::uint32_t level_no = style.numresolutions;
        for (std::uint32_t resno = 0; resno < style.numresolutions; ++resno) {
            --level_no;
            const std::uint32_t pdx = style.precincts[resno] & 0xFu, pdy = style.precincts[resno] >> 4;
            const std::uint32_t rx0 = uint_ceildivpow2(tcx0, level_no), ry0 = uint_ceildivpow2(tcy0, level_no);
            const std::uint32_t rx1 = uint_ceildivpow2(tcx1, level_no), ry1 = uint_ceildivpow2(tcy1, level_no);
            const std::uint32_t px0 = uint_floordivpow2(rx0, pdx) << pdx, py0 = uint_floordivpow2(ry0, pdy) << pdy;
            const std::uint32_t px1 = uint_ceildivpow2(rx1, pdx) << pdx, py1 = uint_ceildivpow2(ry1, pdy) << pdy;
            const std::uint32_t pw = (rx0 == rx1) ? 0 : ((px1 - px0) >> pdx);
            const std::uint32_t ph = (ry0 == ry1) ? 0 : ((py1 - py0) >> pdy);
            pc.resolutions[resno] = PiResolution{pdx, pdy, pw, ph};
            const std::uint32_t product = pw * ph;
            if (product > max_prec) {
                max_prec = product;
            }
        }
    }
    // the table of packets met: (layers + 1) x resolutions x components x precincts bits
    const std::uint32_t step_p = 1, step_c = max_prec * step_p, step_r = numcomps * step_c, step_l = max_res * step_r;
    Included included{nullptr, 0};
    if (step_l <= (UINT_MAX / (numlayers + 1U))) {
        included.size = std::uint64_t(numlayers + 1U) * step_l;
        included.bits = arena.make_array<std::uint64_t>(std::size_t((included.size + 63) / 64) + 1);
    }
    if (!included.bits) {
        return arena.budget_failed() ? NJ2K_E_LIMIT : NJ2K_E_NO_MEMORY;
    }

    T2 t2;
    t2.tile = this;
    t2.in = in;
    t2.arena = &arena;
    t2.msg = &msg;
    t2.csty = coding.csty;
    t2.strict = in->strict;
    ppt = in->ppt;
    t2.ppt = in->has_ppt ? &ppt : nullptr;

    const std::uint8_t* l_current_data = in->data;
    std::uint32_t p_max_len = in->size;
    bool l_exhausted = false;
    std::uint32_t l_nb_comps_final = 0;
    bool l_done = false;
    bool* first_pass_failed = arena.raw_array<bool>(numcomps);
    if (!first_pass_failed) {
        return arena.budget_failed() ? NJ2K_E_LIMIT : NJ2K_E_NO_MEMORY;
    }
    for (std::uint32_t c = 0; c < numcomps; ++c) {
        resno_decoded[c] = 0;
    }

    for (std::uint32_t pino = 0; pino < bound; ++pino) {
        Pi pi{};
        pi.include = &included;
        pi.step_l = step_l;
        pi.step_r = step_r;
        pi.step_c = step_c;
        pi.step_p = step_p;
        pi.numcomps = numcomps;
        pi.comps = pcomps;
        pi.tx0 = tx0_;
        pi.ty0 = ty0_;
        pi.tx1 = tx1_;
        pi.ty1 = ty1_;
        pi.msg = &msg;
        pi.first = true;
        if (npocs) {
            // opj_pi_update_decode_poc (the layer end bounded by the tile's layers, the component
            // end by the components, as opj_j2k_read_poc and opj_pi_update_decode_poc bound them)
            const PocEntry& e = pocs->data[pino];
            pi.poc.prg = e.prg;
            pi.poc.resno0 = e.resno0;
            pi.poc.compno0 = e.compno0;
            pi.poc.layno0 = 0;
            pi.poc.precno0 = 0;
            pi.poc.resno1 = e.resno1;
            pi.poc.compno1 = uint_min(e.compno1, numcomps);
            pi.poc.layno1 = uint_min(e.layno1, numlayers);
            pi.poc.precno1 = max_prec;
        } else {
            // opj_pi_update_decode_not_poc
            pi.poc.prg = coding.prg;
            pi.poc.resno0 = 0;
            pi.poc.compno0 = 0;
            pi.poc.layno0 = 0;
            pi.poc.precno0 = 0;
            pi.poc.resno1 = max_res;
            pi.poc.compno1 = numcomps;
            pi.poc.layno1 = numlayers;
            pi.poc.precno1 = max_prec;
        }
        for (std::uint32_t c = 0; c < numcomps; ++c) {
            first_pass_failed[c] = true;
        }

        while (pi.next()) {
            bool skip_packet = false;
            const PacketPos pos{pi.compno, pi.resno, pi.precno, pi.layno};
            const TileComp& tilec = comps[pi.compno];
            if (pi.layno >= layers_to_decode) {
                skip_packet = true;
            } else if (pi.resno >= tilec.minimum_num_resolutions) {
                skip_packet = true;
            } else {
                // a precinct that meets the area of interest in some band (the components not
                // selected included: OpenJPEG's Tier-2 reads their packets as the others)
                const Resolution& res = tilec.res[pi.resno];
                skip_packet = true;
                for (std::uint32_t bandno = 0; bandno < res.numbands; ++bandno) {
                    const Band& band = res.bands[bandno];
                    const Precinct& prec = band.precincts[pi.precno];
                    if (area_of_interest(tilec, pi.resno, band.bandno, std::uint32_t(prec.x0), std::uint32_t(prec.y0),
                                         std::uint32_t(prec.x1), std::uint32_t(prec.y1))) {
                        skip_packet = false;
                        break;
                    }
                }
            }
            if (l_exhausted) {
                if (in->strict && (coding.csty & kCstyEph)) {
                    // OpenJPEG reads this packet's header from no bytes, and its EPH check fails
                    msg.emit(Messages::Error, "Not enough space for required EPH marker\n");
                    return NJ2K_E_DATA;
                }
                // an absent packet: its visit would change only its component's decoded resolution
                const std::uint32_t l_compno = pi.compno;
                const std::uint32_t l_last_resno = comps[l_compno].minimum_num_resolutions - 1;
                if (resno_decoded[l_compno] < l_last_resno) {
                    if (!skip_packet) {
                        first_pass_failed[l_compno] = false;
                        resno_decoded[l_compno] = pi.resno > resno_decoded[l_compno] ? pi.resno : resno_decoded[l_compno];
                    } else if (first_pass_failed[l_compno] && resno_decoded[l_compno] == 0) {
                        resno_decoded[l_compno] = l_last_resno;
                    }
                    if (resno_decoded[l_compno] >= l_last_resno && ++l_nb_comps_final == numcomps) {
                        l_done = true;
                        break;
                    }
                }
                continue;
            }
            std::uint32_t l_nb_bytes_read = 0;
            if (!skip_packet) {
                first_pass_failed[pi.compno] = false;
                if (!visit_packet(t2, pos, l_current_data, l_nb_bytes_read, p_max_len, false)) {
                    return t2.nomem ? NJ2K_E_NO_MEMORY : NJ2K_E_DATA;
                }
                resno_decoded[pi.compno] = pi.resno > resno_decoded[pi.compno] ? pi.resno : resno_decoded[pi.compno];
            } else {
                if (!visit_packet(t2, pos, l_current_data, l_nb_bytes_read, p_max_len, true)) {
                    return t2.nomem ? NJ2K_E_NO_MEMORY : NJ2K_E_DATA;
                }
            }
            if (first_pass_failed[pi.compno]) {
                if (resno_decoded[pi.compno] == 0) {
                    resno_decoded[pi.compno] = comps[pi.compno].minimum_num_resolutions - 1;
                }
            }
            l_current_data += l_nb_bytes_read;
            p_max_len -= l_nb_bytes_read;
            if (p_max_len == 0 && !(in->ppm && in->ppm->len != 0) && !(in->has_ppt && ppt.len != 0)) {
                // the tile's data is exhausted: every remaining packet would read an empty header
                // and be absent
                l_exhausted = true;
                l_nb_comps_final = 0;
                for (std::uint32_t c = 0; c < numcomps; ++c) {
                    if (resno_decoded[c] + 1U >= comps[c].minimum_num_resolutions) {
                        ++l_nb_comps_final;
                    }
                }
                if (l_nb_comps_final == numcomps) {
                    l_done = true;
                    break;
                }
            }
        }
        if (l_done) {
            break;
        }
    }
    return NJ2K_OK;
}

}  // namespace nj2k::detail

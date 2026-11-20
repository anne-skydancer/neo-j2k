// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The encoder's Tier-2 and rate allocation (stage 6a), ported from OpenJPEG 2.5.4 at the oracle's pin:
// the packet header coder (t2.c opj_t2_encode_packet, bio.c, tgt.c) and the layer allocation by
// rate (tcd.c opj_tcd_rateallocate: the bisection on the slope threshold with a Tier-2 simulation per
// step, and opj_tcd_makelayer), with the same arithmetic, so the layers, packets and bytes are
// OpenJPEG's. The packets come in the viewer's progression (RLCP) for one tile-part. OpenJPEG's notice:
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
#include <cfloat>
#include <cmath>
#include <cstring>

#include "enc_tile.h"

namespace nj2k::detail {

namespace {

// ---- bio.c: the bit writer of the packet headers ----

struct Bio {
    std::uint8_t* start;
    std::uint8_t* end;
    std::uint8_t* bp;
    std::uint32_t buf;
    std::uint32_t ct;

    void init_enc(std::uint8_t* p, std::uint32_t len) noexcept
    {
        start = p;
        end = p + len;
        bp = p;
        buf = 0;
        ct = 8;
    }
    bool byteout() noexcept
    {
        buf = (buf << 8) & 0xffff;
        ct = buf == 0xff00 ? 7 : 8;
        if (reinterpret_cast<std::size_t>(bp) >= reinterpret_cast<std::size_t>(end)) {
            return false;
        }
        *bp++ = static_cast<std::uint8_t>(buf >> 8);
        return true;
    }
    void putbit(std::uint32_t b) noexcept
    {
        if (ct == 0) {
            byteout();  // a full buffer shows when the stream is flushed, as in OpenJPEG
        }
        ct--;
        buf |= b << ct;
    }
    void write(std::uint32_t v, std::uint32_t n) noexcept
    {
        for (std::int32_t i = static_cast<std::int32_t>(n) - 1; i >= 0; i--) {
            putbit((v >> i) & 1);
        }
    }
    bool flush() noexcept
    {
        if (!byteout()) {
            return false;
        }
        if (ct == 7) {
            if (!byteout()) {
                return false;
            }
        }
        return true;
    }
    std::ptrdiff_t numbytes() const noexcept { return bp - start; }
};

int floorlog2(std::int32_t a) noexcept
{
    int l;
    for (l = 0; a > 1; ++l) {
        a >>= 1;
    }
    return l;
}

// ---- tgt.c: the tag tree coder ----

void tgt_reset(EncTagTree& tree) noexcept
{
    EncTagNode* node = tree.nodes;
    for (std::uint32_t i = 0; i < tree.numnodes; ++i) {
        node->value = 999;
        node->low = 0;
        node->known = 0;
        ++node;
    }
}

void tgt_setvalue(EncTagTree& tree, std::uint32_t leafno, std::int32_t value) noexcept
{
    EncTagNode* node = &tree.nodes[leafno];
    while (node && node->value > value) {
        node->value = value;
        node = node->parent;
    }
}

void tgt_encode(Bio& bio, EncTagTree& tree, std::uint32_t leafno, std::int32_t threshold) noexcept
{
    EncTagNode* stk[31];
    EncTagNode** stkptr;
    EncTagNode* node;
    std::int32_t low;

    stkptr = stk;
    node = &tree.nodes[leafno];
    while (node->parent) {
        *stkptr++ = node;
        node = node->parent;
    }

    low = 0;
    for (;;) {
        if (low > node->low) {
            node->low = low;
        } else {
            low = node->low;
        }

        while (low < threshold) {
            if (low >= node->value) {
                if (!node->known) {
                    bio.putbit(1);
                    node->known = 1;
                }
                break;
            }
            bio.putbit(0);
            ++low;
        }

        node->low = low;

        if (stkptr == stk) {
            break;
        }
        node = *--stkptr;
    }
}

// ---- t2.c: the packet header's codes ----

void putcommacode(Bio& bio, std::int32_t n) noexcept
{
    while (--n >= 0) {
        bio.putbit(1);
    }
    bio.putbit(0);
}

void putnumpasses(Bio& bio, std::uint32_t n) noexcept
{
    if (n == 1) {
        bio.putbit(0);
    } else if (n == 2) {
        bio.write(2, 2);
    } else if (n <= 5) {
        bio.write(0xc | (n - 3), 4);
    } else if (n <= 36) {
        bio.write(0x1e0 | (n - 6), 9);
    } else if (n <= 164) {
        bio.write(0xff80 | (n - 37), 16);
    }
}

// opj_t2_encode_packet, for a packet without SOP and EPH markers and every precinct of 2^15 samples
bool encode_packet(const Messages& msg, EncTile& tile, const EncParams& p, std::uint32_t compno, std::uint32_t resno,
                   std::uint32_t layno, std::uint8_t* dest, std::uint32_t* p_data_written, std::uint32_t length,
                   bool final_pass) noexcept
{
    std::uint32_t bandno, cblkno;
    std::uint8_t* c = dest;
    std::uint32_t l_nb_bytes;
    EncComp& tilec = tile.comps[compno];
    EncRes& res = tilec.res[resno];
    Bio bio;

    if (!layno) {
        for (bandno = 0; bandno < res.numbands; ++bandno) {
            EncBand& band = res.bands[bandno];

            /* Skip empty bands */
            if (band.empty()) {
                continue;
            }

            tgt_reset(band.incltree);
            tgt_reset(band.imsbtree);

            const std::uint32_t l_nb_blocks = band.cw * band.ch;
            for (cblkno = 0; cblkno < l_nb_blocks; ++cblkno) {
                EncCblk& cblk = band.cblks[cblkno];

                cblk.numpasses = 0;
                /* The packet header writes the number of bit planes the band holds above the code-block's
                   first, and no less than zero (OD-6): refuse an image that does not fit */
                if (band.numbps < static_cast<std::int32_t>(cblk.numbps)) {
                    msg.emit(Messages::Error,
                             "opj_t2_encode_packet(): a code-block of component %u, resolution %u "
                             "needs %u bit planes, its band holds %d with the core encoder's %u guard bits: "
                             "the image does not fit (more guard bits, GUARD_BITS, belong to the j2k-extra module)\n",
                             compno, resno, cblk.numbps, band.numbps, p.numgbits);
                    return false;
                }
                tgt_setvalue(band.imsbtree, cblkno, band.numbps - static_cast<std::int32_t>(cblk.numbps));
            }
        }
    }

    bio.init_enc(c, length);

    bio.putbit(1); /* Empty header bit: no packet is written empty */

    /* Writing Packet header */
    for (bandno = 0; bandno < res.numbands; ++bandno) {
        EncBand& band = res.bands[bandno];

        /* Skip empty bands */
        if (band.empty()) {
            continue;
        }

        const std::uint32_t l_nb_blocks = band.cw * band.ch;
        EncCblk* cblk = band.cblks;

        for (cblkno = 0; cblkno < l_nb_blocks; ++cblkno) {
            EncLayer* layer = &cblk->layers[layno];

            if (!cblk->numpasses && layer->numpasses) {
                tgt_setvalue(band.incltree, cblkno, static_cast<std::int32_t>(layno));
            }

            ++cblk;
        }

        cblk = band.cblks;
        for (cblkno = 0; cblkno < l_nb_blocks; cblkno++) {
            EncLayer* layer = &cblk->layers[layno];
            std::uint32_t increment = 0;
            std::uint32_t nump = 0;
            std::uint32_t len = 0, passno;
            std::uint32_t l_nb_passes;

            /* cblk inclusion bits */
            if (!cblk->numpasses) {
                tgt_encode(bio, band.incltree, cblkno, static_cast<std::int32_t>(layno + 1));
            } else {
                bio.putbit(layer->numpasses != 0);
            }

            /* if cblk not included, go to the next cblk  */
            if (!layer->numpasses) {
                ++cblk;
                continue;
            }

            /* if first instance of cblk --> zero bit-planes information */
            if (!cblk->numpasses) {
                cblk->numlenbits = 3;
                tgt_encode(bio, band.imsbtree, cblkno, 999);
            }

            /* number of coding passes included */
            putnumpasses(bio, layer->numpasses);
            l_nb_passes = cblk->numpasses + layer->numpasses;
            EncPass* pass = cblk->passes + cblk->numpasses;

            /* computation of the increase of the length indicator and insertion in the header     */
            for (passno = cblk->numpasses; passno < l_nb_passes; ++passno) {
                ++nump;
                len += pass->len;

                if (pass->term || passno == (cblk->numpasses + layer->numpasses) - 1) {
                    const std::int32_t inc = static_cast<std::int32_t>(increment);
                    const std::int32_t want = floorlog2(static_cast<std::int32_t>(len)) + 1 -
                                              (static_cast<std::int32_t>(cblk->numlenbits) +
                                               floorlog2(static_cast<std::int32_t>(nump)));
                    increment = static_cast<std::uint32_t>(inc > want ? inc : want);
                    len = 0;
                    nump = 0;
                }

                ++pass;
            }
            putcommacode(bio, static_cast<std::int32_t>(increment));

            /* computation of the new Length indicator */
            cblk->numlenbits += increment;

            pass = cblk->passes + cblk->numpasses;
            /* insertion of the codeword segment length */
            for (passno = cblk->numpasses; passno < l_nb_passes; ++passno) {
                nump++;
                len += pass->len;

                if (pass->term || passno == (cblk->numpasses + layer->numpasses) - 1) {
                    bio.write(len, cblk->numlenbits + static_cast<std::uint32_t>(floorlog2(static_cast<std::int32_t>(nump))));
                    len = 0;
                    nump = 0;
                }
                ++pass;
            }

            ++cblk;
        }
    }

    if (!bio.flush()) {
        return false; /* modified to eliminate longjmp !! */
    }

    l_nb_bytes = static_cast<std::uint32_t>(bio.numbytes());
    c += l_nb_bytes;
    length -= l_nb_bytes;

    /* Writing the packet body */
    for (bandno = 0; bandno < res.numbands; bandno++) {
        EncBand& band = res.bands[bandno];

        /* Skip empty bands */
        if (band.empty()) {
            continue;
        }

        const std::uint32_t l_nb_blocks = band.cw * band.ch;
        EncCblk* cblk = band.cblks;

        for (cblkno = 0; cblkno < l_nb_blocks; ++cblkno) {
            EncLayer* layer = &cblk->layers[layno];

            if (!layer->numpasses) {
                ++cblk;
                continue;
            }

            if (layer->len > length) {
                if (final_pass) {
                    msg.emit(Messages::Error,
                             "opj_t2_encode_packet(): only %u bytes remaining in output buffer. %u needed.\n", length,
                             layer->len);
                }
                return false;
            }

            if (final_pass) {
                std::memcpy(c, layer->data, layer->len);
            }
            cblk->numpasses += layer->numpasses;
            c += layer->len;
            length -= layer->len;

            ++cblk;
        }
    }

    *p_data_written += static_cast<std::uint32_t>(c - dest);

    return true;
}

// opj_t2_encode_packets, in the progression the viewer's streams use (RLCP, one precinct for every
// resolution, no progression order changes and one tile-part): the packets of the layers below
// maxlayers, resolution by resolution, layer by layer, component by component.
bool encode_packets(const Messages& msg, EncTile& tile, const EncParams& p, std::uint32_t maxlayers, std::uint8_t* dest,
                    std::uint32_t* p_data_written, std::uint32_t max_len, bool final_pass) noexcept
{
    std::uint8_t* l_current_data = dest;
    *p_data_written = 0;

    for (std::uint32_t resno = 0; resno < p.numres; ++resno) {
        for (std::uint32_t layno = 0; layno < p.numlayers; ++layno) {
            if (layno >= maxlayers) {
                continue;
            }
            for (std::uint32_t compno = 0; compno < p.numcomps; ++compno) {
                std::uint32_t l_nb_bytes = 0;
                if (!encode_packet(msg, tile, p, compno, resno, layno, l_current_data, &l_nb_bytes, max_len, final_pass)) {
                    return false;
                }
                l_current_data += l_nb_bytes;
                max_len -= l_nb_bytes;
                *p_data_written += l_nb_bytes;
            }
        }
    }
    return true;
}

// ---- tcd.c: the rate allocation ----

/** Returns true if the layer allocation is unchanged w.r.t to the previous invocation with a different
 * threshold */
bool makelayer(EncTile& tile, std::uint32_t layno, double thresh, std::uint32_t final_pass) noexcept
{
    bool layer_allocation_is_same = true;

    for (std::uint32_t compno = 0; compno < tile.numcomps; compno++) {
        EncComp& tilec = tile.comps[compno];

        for (std::uint32_t resno = 0; resno < tilec.numres; resno++) {
            EncRes& res = tilec.res[resno];

            for (std::uint32_t bandno = 0; bandno < res.numbands; bandno++) {
                EncBand& band = res.bands[bandno];

                /* Skip empty bands */
                if (band.empty()) {
                    continue;
                }

                for (std::uint32_t cblkno = 0; cblkno < band.cw * band.ch; cblkno++) {
                    EncCblk* cblk = &band.cblks[cblkno];
                    EncLayer* layer = &cblk->layers[layno];
                    std::uint32_t n;

                    if (layno == 0) {
                        cblk->numpassesinlayers = 0;
                    }

                    n = cblk->numpassesinlayers;

                    if (thresh < 0) {
                        /* Special value to indicate to use all passes */
                        n = cblk->totalpasses;
                    } else {
                        for (std::uint32_t passno = cblk->numpassesinlayers; passno < cblk->totalpasses; passno++) {
                            std::uint32_t dr;
                            double dd;
                            EncPass* pass = &cblk->passes[passno];

                            if (n == 0) {
                                dr = pass->rate;
                                dd = pass->distortiondec;
                            } else {
                                dr = pass->rate - cblk->passes[n - 1].rate;
                                dd = pass->distortiondec - cblk->passes[n - 1].distortiondec;
                            }

                            if (!dr) {
                                if (dd != 0) {
                                    n = passno + 1;
                                }
                                continue;
                            }
                            if (thresh - (dd / dr) < DBL_EPSILON) { /* do not rely on float equality, check with DBL_EPSILON margin */
                                n = passno + 1;
                            }
                        }
                    }

                    if (layer->numpasses != n - cblk->numpassesinlayers) {
                        layer_allocation_is_same = false;
                        layer->numpasses = n - cblk->numpassesinlayers;
                    }

                    if (!layer->numpasses) {
                        layer->disto = 0;
                        continue;
                    }

                    if (cblk->numpassesinlayers == 0) {
                        layer->len = cblk->passes[n - 1].rate;
                        layer->data = cblk->data;
                        layer->disto = cblk->passes[n - 1].distortiondec;
                    } else {
                        layer->len = cblk->passes[n - 1].rate - cblk->passes[cblk->numpassesinlayers - 1].rate;
                        layer->data = cblk->data + cblk->passes[cblk->numpassesinlayers - 1].rate;
                        layer->disto =
                            cblk->passes[n - 1].distortiondec - cblk->passes[cblk->numpassesinlayers - 1].distortiondec;
                    }

                    if (final_pass) {
                        cblk->numpassesinlayers = n;
                    }
                }
            }
        }
    }
    return layer_allocation_is_same;
}

// opj_tcd_rateallocate for m_quality_layer_alloc_strategy == RATE_DISTORTION_RATIO
void rateallocate(const Messages& msg, EncTile& tile, const EncParams& p, std::uint8_t* dest, std::uint32_t len,
                  std::uint32_t* p_data_written) noexcept
{
    double min = DBL_MAX;
    double max = 0;

    for (std::uint32_t compno = 0; compno < tile.numcomps; compno++) {
        EncComp& tilec = tile.comps[compno];

        for (std::uint32_t resno = 0; resno < tilec.numres; resno++) {
            EncRes& res = tilec.res[resno];

            for (std::uint32_t bandno = 0; bandno < res.numbands; bandno++) {
                EncBand& band = res.bands[bandno];

                /* Skip empty bands */
                if (band.empty()) {
                    continue;
                }

                for (std::uint32_t cblkno = 0; cblkno < band.cw * band.ch; cblkno++) {
                    EncCblk* cblk = &band.cblks[cblkno];

                    for (std::uint32_t passno = 0; passno < cblk->totalpasses; passno++) {
                        EncPass* pass = &cblk->passes[passno];
                        std::int32_t dr;
                        double dd, rdslope;

                        if (passno == 0) {
                            dr = static_cast<std::int32_t>(pass->rate);
                            dd = pass->distortiondec;
                        } else {
                            dr = static_cast<std::int32_t>(pass->rate - cblk->passes[passno - 1].rate);
                            dd = pass->distortiondec - cblk->passes[passno - 1].distortiondec;
                        }

                        if (dr == 0) {
                            continue;
                        }

                        rdslope = dd / dr;
                        if (rdslope < min) {
                            min = rdslope;
                        }

                        if (rdslope > max) {
                            max = rdslope;
                        }
                    } /* passno */
                } /* cbklno */
            } /* bandno */
        } /* resno */
    } /* compno */

    for (std::uint32_t layno = 0; layno < p.numlayers; layno++) {
        double lo = min;
        double hi = max;
        std::uint32_t maxlen = p.rates[layno] > 0.0f ? static_cast<std::uint32_t>(std::ceil(p.rates[layno])) : len;
        if (p.rates[layno] > 0.0f && len < maxlen) {
            maxlen = len;
        }
        double goodthresh = 0;
        double stable_thresh = 0;
        std::uint32_t i;

        /* Don't try to find an optimal threshold but rather take everything not included yet, if
          -r xx,yy,zz,0   (m_quality_layer_alloc_strategy == RATE_DISTORTION_RATIO and rates == NULL)
          ==> possible to have some lossy layers and the last layer for sure lossless */
        if (p.rates[layno] > 0.0f) {
            double thresh = 0;
            bool last_layer_allocation_ok = false;

            for (i = 0; i < 128; ++i) {
                bool layer_allocation_is_same;

                double new_thresh = (lo + hi) / 2;
                /* Stop iterating when the threshold has stabilized enough */
                /* 0.5 * 1e-5 is somewhat arbitrary, but has been selected */
                /* so that this doesn't change the results of the regression */
                /* test suite. */
                if (std::fabs(new_thresh - thresh) <= 0.5 * 1e-5 * thresh) {
                    break;
                }
                thresh = new_thresh;

                layer_allocation_is_same = makelayer(tile, layno, thresh, 0) && i != 0;
                /* Check if the layer allocation done by makelayer() is compatible of the maximum rate
                 * allocation. If not, retry with a higher threshold. If OK, try with a lower threshold.
                 * Call encode_packets() only if makelayer() has resulted in different truncation points
                 * since its last call. */
                if ((layer_allocation_is_same && !last_layer_allocation_ok) ||
                    (!layer_allocation_is_same &&
                     !encode_packets(msg, tile, p, layno + 1, dest, p_data_written, maxlen, false))) {
                    last_layer_allocation_ok = false;
                    lo = thresh;
                    continue;
                }

                last_layer_allocation_ok = true;
                hi = thresh;
                stable_thresh = thresh;
            }

            goodthresh = stable_thresh == 0 ? thresh : stable_thresh;
        } else {
            /* Special value to indicate to use all passes */
            goodthresh = -1;
        }

        makelayer(tile, layno, goodthresh, 1);
    }
}

}  // namespace

bool enc_tgt_create(Arena& arena, EncTagTree& tree, std::uint32_t numleafsh, std::uint32_t numleafsv) noexcept
{
    std::int32_t nplh[32];
    std::int32_t nplv[32];
    EncTagNode* node = nullptr;
    EncTagNode* l_parent_node = nullptr;
    EncTagNode* l_parent_node0 = nullptr;
    std::uint32_t i;
    std::int32_t j, k;
    std::uint32_t numlvls;
    std::uint32_t n;

    tree.numleafsh = numleafsh;
    tree.numleafsv = numleafsv;

    numlvls = 0;
    nplh[0] = static_cast<std::int32_t>(numleafsh);
    nplv[0] = static_cast<std::int32_t>(numleafsv);
    tree.numnodes = 0;
    do {
        n = static_cast<std::uint32_t>(nplh[numlvls] * nplv[numlvls]);
        nplh[numlvls + 1] = (nplh[numlvls] + 1) / 2;
        nplv[numlvls + 1] = (nplv[numlvls] + 1) / 2;
        tree.numnodes += n;
        ++numlvls;
    } while (n > 1);

    if (tree.numnodes == 0) {
        return false;
    }
    tree.nodes = arena.make_array<EncTagNode>(tree.numnodes);
    if (!tree.nodes) {
        return false;
    }

    node = tree.nodes;
    l_parent_node = &tree.nodes[tree.numleafsh * tree.numleafsv];
    l_parent_node0 = l_parent_node;

    for (i = 0; i < numlvls - 1; ++i) {
        for (j = 0; j < nplv[i]; ++j) {
            k = nplh[i];
            while (--k >= 0) {
                node->parent = l_parent_node;
                ++node;
                if (--k >= 0) {
                    node->parent = l_parent_node;
                    ++node;
                }
                ++l_parent_node;
            }
            if ((j & 1) || j == nplv[i] - 1) {
                l_parent_node0 = l_parent_node;
            } else {
                l_parent_node = l_parent_node0;
                l_parent_node0 += nplh[i];
            }
        }
    }
    node->parent = nullptr;
    tgt_reset(tree);
    return true;
}

bool enc_rate_allocate_and_packets(const Messages& msg, EncTile& tile, const EncParams& p,
                                   std::uint8_t* dest, std::uint32_t len, std::uint32_t* written) noexcept
{
    std::uint32_t scratch_written = 0;
    rateallocate(msg, tile, p, dest, len, &scratch_written);
    return encode_packets(msg, tile, p, p.numlayers, dest, written, len, true);
}

}  // namespace nj2k::detail

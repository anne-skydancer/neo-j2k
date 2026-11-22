// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The encoder's Tier-1 (stage 6a), ported from OpenJPEG 2.5.4 at the oracle's pin (oracle/PIN): the MQ
// encoder (mqc.c, mqc_inl.h), the context tables (t1_luts.h; t1_tables.h, enc_luts.h) and the three
// encoding passes with the code-block driver (t1.c) are OpenJPEG's code with the same operations, so
// the bytes and the rates are OpenJPEG's, for the code-blocks the viewer's streams hold (64 x 64, no
// mode switches: the RAW (BYPASS) coder, the terminations and the vertically causal contexts of the
// other modes belong to the j2k-extra module). The MQ coder's output is bounded here, where OpenJPEG
// writes without a bound (OD-4). OpenJPEG's notice:
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
#include <cstring>
#include <new>

#include "enc_luts.h"
#include "enc_mq.h"
#include "enc_tile.h"
#include "t1.h"
#include "t1_flags.h"
#include "t1_tables.h"

namespace nj2k::detail {

namespace {

using OPJ_UINT32 = std::uint32_t;
using OPJ_INT32 = std::int32_t;
using opj_flag_t = std::uint32_t;

// the largest code-block of the encoder: 64 x 64
constexpr OPJ_UINT32 kCblkW = 64, kCblkH = 64;
constexpr OPJ_UINT32 kMaxSamples = kCblkW * kCblkH;
constexpr OPJ_UINT32 kFlagStride = kCblkW + 2;
constexpr OPJ_UINT32 kFlagCount = kFlagStride * ((kCblkH + 3) / 4 + 2);
// OpenJPEG's code-block buffer: 74 bytes and four per sample, plus the margin of the passes a
// code-block can terminate (OPJ_ENC_TERM_MARGIN); bounded here, where OpenJPEG writes unchecked
constexpr OPJ_UINT32 kTermMargin = 3U * (3U * (31U - T1_NMSEDEC_FRACBITS) - 2U);
constexpr OPJ_UINT32 kOutCapacity = 74 + kTermMargin + kMaxSamples * 4;
constexpr OPJ_UINT32 kMaxPasses = 3 * (31 - T1_NMSEDEC_FRACBITS);

inline OPJ_UINT32 smr_abs(OPJ_INT32 x) noexcept { return static_cast<OPJ_UINT32>(x) & 0x7FFFFFFFU; }
inline OPJ_UINT32 smr_sign(OPJ_INT32 x) noexcept { return static_cast<OPJ_UINT32>(x) >> 31; }

int floorlog2(OPJ_INT32 a) noexcept
{
    int l;
    for (l = 0; a > 1; ++l) {
        a >>= 1;
    }
    return l;
}

// the MQ encoder is enc_mq.h

}  // namespace

// ---- t1.c: the encoding passes (MQ only: the viewer's streams set no mode switches) ----

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)  // padded for the alignment of its buffers, as intended
#endif
struct EncT1 {
    MqEnc mqc;
    OPJ_UINT32 w, h;
    alignas(64) OPJ_INT32 data[kMaxSamples];
    alignas(64) opj_flag_t flags[kFlagCount];
    EncPass passes[kMaxPasses];
    std::uint8_t out[1 + kOutCapacity];  // out[0] is the fake byte before the code-block's
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace {

#define T1_FLAGS(x, y) (t1->flags[x + 1 + ((y / 4) + 1) * (t1->w + 2)])

inline std::uint8_t getctxno_zc(const MqEnc* mqc, OPJ_UINT32 f) noexcept
{
    return mqc->lut_ctxno_zc_orient[(f & T1_SIGMA_NEIGHBOURS)];
}

inline OPJ_UINT32 getctxtno_sc_or_spb_index(OPJ_UINT32 fX, OPJ_UINT32 pfX, OPJ_UINT32 nfX, OPJ_UINT32 ci) noexcept
{
    OPJ_UINT32 lu = (fX >> (ci * 3U)) & (T1_SIGMA_1 | T1_SIGMA_3 | T1_SIGMA_5 | T1_SIGMA_7);

    lu |= (pfX >> (T1_CHI_THIS_I + (ci * 3U))) & (1U << 0);
    lu |= (nfX >> (T1_CHI_THIS_I - 2U + (ci * 3U))) & (1U << 2);
    if (ci == 0U) {
        lu |= (fX >> (T1_CHI_0_I - 4U)) & (1U << 4);
    } else {
        lu |= (fX >> (T1_CHI_1_I - 4U + ((ci - 1U) * 3U))) & (1U << 4);
    }
    lu |= (fX >> (T1_CHI_2_I - 6U + (ci * 3U))) & (1U << 6);
    return lu;
}

inline OPJ_UINT32 getctxno_mag(OPJ_UINT32 f) noexcept
{
    OPJ_UINT32 tmp = (f & T1_SIGMA_NEIGHBOURS) ? T1_CTXNO_MAG + 1 : T1_CTXNO_MAG;
    OPJ_UINT32 tmp2 = (f & T1_MU_0) ? T1_CTXNO_MAG + 2 : tmp;
    return tmp2;
}

inline std::int16_t getnmsedec_sig(OPJ_UINT32 x, OPJ_UINT32 bitpos) noexcept
{
    if (bitpos > 0) {
        return kNmsedecSig[(x >> (bitpos)) & ((1 << T1_NMSEDEC_BITS) - 1)];
    }
    return kNmsedecSig0[x & ((1 << T1_NMSEDEC_BITS) - 1)];
}

inline std::int16_t getnmsedec_ref(OPJ_UINT32 x, OPJ_UINT32 bitpos) noexcept
{
    if (bitpos > 0) {
        return kNmsedecRef[(x >> (bitpos)) & ((1 << T1_NMSEDEC_BITS) - 1)];
    }
    return kNmsedecRef0[x & ((1 << T1_NMSEDEC_BITS) - 1)];
}

// opj_t1_update_flags with vsc = 0
inline void update_flags(opj_flag_t* flagsp, OPJ_UINT32 ci, OPJ_UINT32 s, OPJ_UINT32 stride) noexcept
{
    /* east */
    flagsp[-1] |= T1_SIGMA_5 << (3U * ci);

    /* mark target as significant */
    *flagsp |= ((s << T1_CHI_1_I) | T1_SIGMA_4) << (3U * ci);

    /* west */
    flagsp[1] |= T1_SIGMA_3 << (3U * ci);

    /* north-west, north, north-east */
    if (ci == 0U) {
        opj_flag_t* north = flagsp - stride;
        *north |= (s << T1_CHI_5_I) | T1_SIGMA_16;
        north[-1] |= T1_SIGMA_17;
        north[1] |= T1_SIGMA_15;
    }

    /* south-west, south, south-east */
    if (ci == 3U) {
        opj_flag_t* south = flagsp + stride;
        *south |= (s << T1_CHI_0_I) | T1_SIGMA_1;
        south[-1] |= T1_SIGMA_2;
        south[1] |= T1_SIGMA_0;
    }
}

NJ2K_MQ_INLINE void enc_sigpass_step(EncT1* t1, opj_flag_t* flagsp, const OPJ_INT32* datap, OPJ_INT32 bpno, OPJ_INT32 one,
                             OPJ_INT32* nmsedec, OPJ_UINT32 ci) noexcept
{
    MqEnc* mqc = &t1->mqc;
    OPJ_UINT32 v;
    OPJ_UINT32 const flags = *flagsp;
    if ((flags & ((T1_SIGMA_THIS | T1_PI_THIS) << (ci * 3U))) == 0U &&
        (flags & (T1_SIGMA_NEIGHBOURS << (ci * 3U))) != 0U) {
        OPJ_UINT32 ctxt1 = getctxno_zc(mqc, flags >> (ci * 3U));
        v = (smr_abs(*datap) & static_cast<OPJ_UINT32>(one)) ? 1 : 0;
        mqc->setcurctx(ctxt1);
        mqc->encode(v);
        if (v) {
            OPJ_UINT32 lu = getctxtno_sc_or_spb_index(*flagsp, flagsp[-1], flagsp[1], ci);
            OPJ_UINT32 ctxt2 = kLutCtxnoSc[lu];
            v = smr_sign(*datap);
            *nmsedec += getnmsedec_sig(smr_abs(*datap), static_cast<OPJ_UINT32>(bpno));
            mqc->setcurctx(ctxt2);
            OPJ_UINT32 spb = kLutSpb[lu];
            mqc->encode(v ^ spb);
            update_flags(flagsp, ci, v, t1->w + 2);
        }
        *flagsp |= T1_PI_THIS << (ci * 3U);
    }
}

void enc_sigpass(EncT1* t1, OPJ_INT32 bpno, OPJ_INT32* nmsedec) noexcept
{
    OPJ_UINT32 i, k;
    OPJ_INT32 const one = 1 << (bpno + T1_NMSEDEC_FRACBITS);
    opj_flag_t* f = &T1_FLAGS(0, 0);
    OPJ_UINT32 const extra = 2;
    const OPJ_INT32* datap = t1->data;

    *nmsedec = 0;
    for (k = 0; k < (t1->h & ~3U); k += 4, f += extra) {
        const OPJ_UINT32 w = t1->w;
        for (i = 0; i < w; ++i, ++f, datap += 4) {
            if (*f == 0U) {
                /* Nothing to do for any of the 4 data points */
                continue;
            }
            enc_sigpass_step(t1, f, &datap[0], bpno, one, nmsedec, 0);
            enc_sigpass_step(t1, f, &datap[1], bpno, one, nmsedec, 1);
            enc_sigpass_step(t1, f, &datap[2], bpno, one, nmsedec, 2);
            enc_sigpass_step(t1, f, &datap[3], bpno, one, nmsedec, 3);
        }
    }

    if (k < t1->h) {
        OPJ_UINT32 j;
        for (i = 0; i < t1->w; ++i, ++f) {
            if (*f == 0U) {
                /* Nothing to do for any of the 4 data points */
                datap += (t1->h - k);
                continue;
            }
            for (j = k; j < t1->h; ++j, ++datap) {
                enc_sigpass_step(t1, f, &datap[0], bpno, one, nmsedec, j - k);
            }
        }
    }
}

NJ2K_MQ_INLINE void enc_refpass_step(EncT1* t1, OPJ_UINT32 flags, OPJ_UINT32& flagsUpdated, const OPJ_INT32* datap,
                             OPJ_INT32 bpno, OPJ_INT32 one, OPJ_INT32* nmsedec, OPJ_UINT32 ci) noexcept
{
    MqEnc* mqc = &t1->mqc;
    OPJ_UINT32 v;
    if ((flags & ((T1_SIGMA_THIS | T1_PI_THIS) << ((ci) * 3U))) == (T1_SIGMA_THIS << ((ci) * 3U))) {
        const OPJ_UINT32 shift_flags = (flags >> ((ci) * 3U));
        OPJ_UINT32 ctxt = getctxno_mag(shift_flags);
        OPJ_UINT32 abs_data = smr_abs(*datap);
        *nmsedec += getnmsedec_ref(abs_data, static_cast<OPJ_UINT32>(bpno));
        v = (static_cast<OPJ_INT32>(abs_data) & one) ? 1 : 0;
        mqc->setcurctx(ctxt);
        mqc->encode(v);
        flagsUpdated |= T1_MU_THIS << ((ci) * 3U);
    }
}

void enc_refpass(EncT1* t1, OPJ_INT32 bpno, OPJ_INT32* nmsedec) noexcept
{
    OPJ_UINT32 i, k;
    const OPJ_INT32 one = 1 << (bpno + T1_NMSEDEC_FRACBITS);
    opj_flag_t* f = &T1_FLAGS(0, 0);
    const OPJ_UINT32 extra = 2U;
    const OPJ_INT32* datap = t1->data;

    *nmsedec = 0;
    for (k = 0; k < (t1->h & ~3U); k += 4, f += extra) {
        for (i = 0; i < t1->w; ++i, f++, datap += 4) {
            const OPJ_UINT32 flags = *f;
            OPJ_UINT32 flagsUpdated = flags;
            if ((flags & (T1_SIGMA_4 | T1_SIGMA_7 | T1_SIGMA_10 | T1_SIGMA_13)) == 0) {
                /* none significant */
                continue;
            }
            if ((flags & (T1_PI_0 | T1_PI_1 | T1_PI_2 | T1_PI_3)) == (T1_PI_0 | T1_PI_1 | T1_PI_2 | T1_PI_3)) {
                /* all processed by sigpass */
                continue;
            }

            enc_refpass_step(t1, flags, flagsUpdated, &datap[0], bpno, one, nmsedec, 0);
            enc_refpass_step(t1, flags, flagsUpdated, &datap[1], bpno, one, nmsedec, 1);
            enc_refpass_step(t1, flags, flagsUpdated, &datap[2], bpno, one, nmsedec, 2);
            enc_refpass_step(t1, flags, flagsUpdated, &datap[3], bpno, one, nmsedec, 3);
            *f = flagsUpdated;
        }
    }

    if (k < t1->h) {
        OPJ_UINT32 j;
        const OPJ_UINT32 remaining_lines = t1->h - k;
        for (i = 0; i < t1->w; ++i, ++f) {
            if ((*f & (T1_SIGMA_4 | T1_SIGMA_7 | T1_SIGMA_10 | T1_SIGMA_13)) == 0) {
                /* none significant */
                datap += remaining_lines;
                continue;
            }
            for (j = 0; j < remaining_lines; j++, datap++) {
                enc_refpass_step(t1, *f, *f, &datap[0], bpno, one, nmsedec, j);
            }
        }
    }
}

// opj_t1_enc_clnpass_step_macro with the viewer's modes (no vertically causal contexts)
NJ2K_MQ_INLINE void enc_clnpass_step(EncT1* t1, opj_flag_t* flagspIn, const OPJ_INT32* datapIn, OPJ_INT32 bpno,
                             OPJ_INT32 one, OPJ_INT32* nmsedec, OPJ_UINT32 agg, OPJ_UINT32 runlen,
                             OPJ_UINT32 lim) noexcept
{
    MqEnc* mqc = &t1->mqc;
    OPJ_UINT32 v;
    OPJ_UINT32 ci;
    opj_flag_t* const flagsp = flagspIn;
    const OPJ_INT32* l_datap = datapIn;
    const OPJ_UINT32 check =
        (T1_SIGMA_4 | T1_SIGMA_7 | T1_SIGMA_10 | T1_SIGMA_13 | T1_PI_0 | T1_PI_1 | T1_PI_2 | T1_PI_3);

    if ((*flagsp & check) == check) {
        if (runlen == 0) {
            *flagsp &= ~(T1_PI_0 | T1_PI_1 | T1_PI_2 | T1_PI_3);
        } else if (runlen == 1) {
            *flagsp &= ~(T1_PI_1 | T1_PI_2 | T1_PI_3);
        } else if (runlen == 2) {
            *flagsp &= ~(T1_PI_2 | T1_PI_3);
        } else if (runlen == 3) {
            *flagsp &= ~(T1_PI_3);
        }
    } else {
        for (ci = runlen; ci < lim; ++ci) {
            bool goto_PARTIAL = false;
            if ((agg != 0) && (ci == runlen)) {
                goto_PARTIAL = true;
            } else if (!(*flagsp & ((T1_SIGMA_THIS | T1_PI_THIS) << (ci * 3U)))) {
                OPJ_UINT32 ctxt1 = getctxno_zc(mqc, *flagsp >> (ci * 3U));
                mqc->setcurctx(ctxt1);
                v = (smr_abs(*l_datap) & static_cast<OPJ_UINT32>(one)) ? 1 : 0;
                mqc->encode(v);
                if (v) {
                    goto_PARTIAL = true;
                }
            }
            if (goto_PARTIAL) {
                OPJ_UINT32 ctxt2, spb;
                OPJ_UINT32 lu = getctxtno_sc_or_spb_index(*flagsp, flagsp[-1], flagsp[1], ci);
                *nmsedec += getnmsedec_sig(smr_abs(*l_datap), static_cast<OPJ_UINT32>(bpno));
                ctxt2 = kLutCtxnoSc[lu];
                mqc->setcurctx(ctxt2);

                v = smr_sign(*l_datap);
                spb = kLutSpb[lu];
                mqc->encode(v ^ spb);
                update_flags(flagsp, ci, v, t1->w + 2U);
            }
            *flagsp &= ~(T1_PI_THIS << (3U * ci));
            l_datap++;
        }
    }
}

void enc_clnpass(EncT1* t1, OPJ_INT32 bpno, OPJ_INT32* nmsedec) noexcept
{
    OPJ_UINT32 i, k;
    const OPJ_INT32 one = 1 << (bpno + T1_NMSEDEC_FRACBITS);
    MqEnc* mqc = &t1->mqc;
    const OPJ_INT32* datap = t1->data;
    opj_flag_t* f = &T1_FLAGS(0, 0);
    const OPJ_UINT32 extra = 2U;

    *nmsedec = 0;
    for (k = 0; k < (t1->h & ~3U); k += 4, f += extra) {
        for (i = 0; i < t1->w; ++i, f++) {
            OPJ_UINT32 agg, runlen;
            agg = !*f;
            if (agg) {
                for (runlen = 0; runlen < 4; ++runlen, ++datap) {
                    if (smr_abs(*datap) & static_cast<OPJ_UINT32>(one)) {
                        break;
                    }
                }
                mqc->setcurctx(T1_CTXNO_AGG);
                mqc->encode(runlen != 4);
                if (runlen == 4) {
                    continue;
                }
                mqc->setcurctx(T1_CTXNO_UNI);
                mqc->encode(runlen >> 1);
                mqc->encode(runlen & 1);
            } else {
                runlen = 0;
            }
            enc_clnpass_step(t1, f, datap, bpno, one, nmsedec, agg, runlen, 4U);
            datap += 4 - runlen;
        }
    }
    if (k < t1->h) {
        const OPJ_UINT32 agg = 0;
        const OPJ_UINT32 runlen = 0;
        for (i = 0; i < t1->w; ++i, f++) {
            enc_clnpass_step(t1, f, datap, bpno, one, nmsedec, agg, runlen, t1->h - k);
            datap += t1->h - k;
        }
    }
}

double getwmsedec(OPJ_INT32 nmsedec, OPJ_UINT32 compno, OPJ_UINT32 level, OPJ_UINT32 orient, OPJ_INT32 bpno,
                  bool reversible, double stepsize, const double* mct_norms, OPJ_UINT32 mct_numcomps) noexcept
{
    double w1 = 1, w2, wmsedec;

    if (mct_norms && (compno < mct_numcomps)) {
        w1 = mct_norms[compno];
    }

    if (reversible) {
        w2 = enc_dwt_norm(level, orient);
    } else {
        const OPJ_INT32 log2_gain = (orient == 0) ? 0 : (orient == 3) ? 2 : 1;
        w2 = enc_dwt_norm_real(level, orient);
        /* Not sure this is right. But preserves past behaviour */
        stepsize /= (1 << log2_gain);
    }

    wmsedec = w1 * w2 * stepsize * (1 << bpno);
    wmsedec *= wmsedec * nmsedec / 8192.0;

    return wmsedec;
}

}  // namespace

EncT1* enc_t1_create(const Allocator& alloc) noexcept
{
    void* mem = alloc.allocate(sizeof(EncT1), 64);
    if (!mem) {
        return nullptr;
    }
    auto* t1 = static_cast<EncT1*>(mem);
    std::memset(static_cast<void*>(t1), 0, sizeof(EncT1));
    return t1;
}

void enc_t1_destroy(EncT1* t1, const Allocator& alloc) noexcept
{
    if (t1) {
        alloc.release(t1, sizeof(EncT1));
    }
}

std::int32_t* enc_t1_data(EncT1* t1) noexcept
{
    return t1->data;
}

bool enc_t1_encode(EncT1* t1, EncCblk& cblk, std::uint32_t orient, std::uint32_t compno,
                   std::uint32_t level, bool reversible, double stepsize, const double* mct_norms,
                   std::uint32_t mct_numcomps) noexcept
{
    double cumwmsedec = 0.0;
    MqEnc* mqc = &t1->mqc;
    OPJ_UINT32 passno;
    OPJ_INT32 bpno;
    OPJ_UINT32 passtype;
    OPJ_INT32 nmsedec = 0;
    OPJ_INT32 max;
    OPJ_UINT32 i, j;
    double tempwmsedec;
    OPJ_INT32* datap;

    t1->w = static_cast<OPJ_UINT32>(cblk.x1 - cblk.x0);
    t1->h = static_cast<OPJ_UINT32>(cblk.y1 - cblk.y0);

    // opj_t1_allocate_buffers: the flags of an empty code-block, with the border rows that stop the
    // passes and the padding of a height that is no multiple of four
    {
        const OPJ_UINT32 flags_stride = t1->w + 2U;
        const OPJ_UINT32 flags_height = (t1->h + 3U) / 4U;
        const OPJ_UINT32 flagssize = (flags_height + 2U) * flags_stride;
        std::memset(t1->flags, 0, flagssize * sizeof(opj_flag_t));
        opj_flag_t* p = &t1->flags[0];
        for (OPJ_UINT32 x = 0; x < flags_stride; ++x) {
            /* magic value to hopefully stop any passes being interested in this entry */
            *p++ = (T1_PI_0 | T1_PI_1 | T1_PI_2 | T1_PI_3);
        }
        p = &t1->flags[((flags_height + 1) * flags_stride)];
        for (OPJ_UINT32 x = 0; x < flags_stride; ++x) {
            *p++ = (T1_PI_0 | T1_PI_1 | T1_PI_2 | T1_PI_3);
        }
        if (t1->h % 4) {
            OPJ_UINT32 v = 0;
            p = &t1->flags[((flags_height)*flags_stride)];
            if (t1->h % 4 == 1) {
                v |= T1_PI_1 | T1_PI_2 | T1_PI_3;
            } else if (t1->h % 4 == 2) {
                v |= T1_PI_2 | T1_PI_3;
            } else if (t1->h % 4 == 3) {
                v |= T1_PI_3;
            }
            for (OPJ_UINT32 x = 0; x < flags_stride; ++x) {
                *p++ = v;
            }
        }
    }

    mqc->lut_ctxno_zc_orient = kLutCtxnoZc + (orient << 9);

    max = 0;
    datap = t1->data;
    for (j = 0; j < t1->h; ++j) {
        const OPJ_UINT32 w = t1->w;
        for (i = 0; i < w; ++i, ++datap) {
            OPJ_INT32 tmp = *datap;
            if (tmp < 0) {
                OPJ_UINT32 tmp_unsigned;
                if (tmp == INT32_MIN) {
                    /* To avoid undefined behaviour when negating INT_MIN */
                    tmp = INT32_MIN + 1;
                }
                max = max > -tmp ? max : -tmp;
                tmp_unsigned = static_cast<OPJ_UINT32>(-tmp) | 0x80000000U;
                std::memcpy(datap, &tmp_unsigned, sizeof(OPJ_INT32));
            } else {
                max = max > tmp ? max : tmp;
            }
        }
    }

    cblk.numbps = max ? static_cast<OPJ_UINT32>((floorlog2(max) + 1) - T1_NMSEDEC_FRACBITS) : 0;
    if (cblk.numbps == 0) {
        cblk.totalpasses = 0;
        return true;
    }

    bpno = static_cast<OPJ_INT32>(cblk.numbps - 1);
    passtype = 2;

    t1->out[0] = 0;
    mqc->resetstates();
    mqc->setstate(T1_CTXNO_UNI, 0, 46);
    mqc->setstate(T1_CTXNO_AGG, 0, 3);
    mqc->setstate(T1_CTXNO_ZC, 0, 4);
    mqc->init_enc(t1->out + 1, t1->out + kOutCapacity);

    for (passno = 0; bpno >= 0; ++passno) {
        EncPass* pass = &t1->passes[passno];

        switch (passtype) {
        case 0:
            enc_sigpass(t1, bpno, &nmsedec);
            break;
        case 1:
            enc_refpass(t1, bpno, &nmsedec);
            break;
        case 2:
            enc_clnpass(t1, bpno, &nmsedec);
            break;
        }

        tempwmsedec = getwmsedec(nmsedec, compno, level, orient, bpno, reversible, stepsize, mct_norms, mct_numcomps);
        cumwmsedec += tempwmsedec;
        pass->distortiondec = cumwmsedec;

        if (passtype == 2 && bpno == 0) {
            /* the last cleanup pass terminates the code-block */
            mqc->flush();
            pass->term = 1;
            pass->rate = mqc->numbytes();
        } else {
            /* Non terminated pass */
            pass->term = 0;
            pass->rate = mqc->numbytes() + 3;
        }

        if (++passtype == 3) {
            passtype = 0;
            bpno--;
        }
    }

    if (mqc->overflow) {
        return false;
    }

    cblk.totalpasses = passno;

    if (cblk.totalpasses) {
        /* Make sure that pass rates are increasing */
        OPJ_UINT32 last_pass_rate = mqc->numbytes();
        for (passno = cblk.totalpasses; passno > 0;) {
            EncPass* pass = &t1->passes[--passno];
            if (pass->rate > last_pass_rate) {
                pass->rate = last_pass_rate;
            } else {
                last_pass_rate = pass->rate;
            }
        }
    }

    for (passno = 0; passno < cblk.totalpasses; passno++) {
        EncPass* pass = &t1->passes[passno];

        /* Prevent generation of FF as last data byte of a pass */
        if (t1->out[1 + pass->rate - 1] == 0xFF) {
            pass->rate--;
        }
        pass->len = pass->rate - (passno == 0 ? 0 : t1->passes[passno - 1].rate);
    }

    return true;
}

bool enc_t1_commit(const EncT1* t1, Arena& arena, EncCblk& cblk) noexcept
{
    // Keep no more than the code-block holds (OpenJPEG keeps the whole largest-output buffer).
    if (!cblk.totalpasses) {
        return true;
    }
    cblk.passes = arena.make_array<EncPass>(cblk.totalpasses);
    const OPJ_UINT32 bytes = t1->mqc.numbytes();
    cblk.data = static_cast<std::uint8_t*>(arena.allocate(bytes ? bytes : 1, 16));
    if (!cblk.passes || !cblk.data) {
        return false;
    }
    std::memcpy(cblk.passes, t1->passes, cblk.totalpasses * sizeof(EncPass));
    std::memcpy(cblk.data, t1->out + 1, bytes);
    return true;
}

}  // namespace nj2k::detail

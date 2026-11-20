// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// Tier-1 decoding, ported from OpenJPEG 2.5.4 at the oracle's pin (oracle/PIN): the MQ decoder
// (mqc_inl.h, mqc.c), the context tables (t1_luts.h) and the three decoding passes with their raw
// (BYPASS) variants (t1.c) are OpenJPEG's code, unchanged but for the C++ adjustments (no
// 'register', static inline functions, file-local names); the code-block driver and the store into
// the tile buffer follow opj_t1_decode_cblk and opj_t1_clbl_decode_processor. OpenJPEG's notice:
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
#include "t1.h"
#include "t1_tables.h"

#include <cassert>
#include <cstring>

#if defined(__AVX2__)
#include <immintrin.h>
#endif
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

namespace nj2k::detail {

namespace {

// OpenJPEG's names for the types and constants the ported code uses
using OPJ_BYTE = std::uint8_t;
using OPJ_INT16 = std::int16_t;
using OPJ_INT32 = std::int32_t;
using OPJ_UINT32 = std::uint32_t;
using OPJ_BOOL = int;
using opj_flag_t = std::uint32_t;
using opj_mqc_state_t = MqState;
using opj_mqc_t = Mqc;
using opj_t1_t = T1;
constexpr OPJ_BOOL OPJ_TRUE = 1, OPJ_FALSE = 0;
constexpr OPJ_UINT32 OPJ_COMMON_CBLK_DATA_EXTRA = 2;
constexpr OPJ_UINT32 MQC_NUMCTXS = 19;
constexpr OPJ_UINT32 J2K_CCP_CBLKSTY_LAZY = kCblkLazy, J2K_CCP_CBLKSTY_RESET = kCblkReset,
                     J2K_CCP_CBLKSTY_VSC = kCblkVsc, J2K_CCP_CBLKSTY_SEGSYM = kCblkSegsym;

#define opj_mqc_setcurctx(mqc, ctxno) (mqc)->curctx = &(mqc)->ctxs[(OPJ_UINT32)(ctxno)]

// t1.h: the contexts and the flags of opj_flag_t
#define T1_NUMCTXS_ZC 9
#define T1_NUMCTXS_SC 5
#define T1_NUMCTXS_MAG 3
#define T1_NUMCTXS_AGG 1
#define T1_NUMCTXS_UNI 1
#define T1_CTXNO_ZC 0
#define T1_CTXNO_SC (T1_CTXNO_ZC + T1_NUMCTXS_ZC)
#define T1_CTXNO_MAG (T1_CTXNO_SC + T1_NUMCTXS_SC)
#define T1_CTXNO_AGG (T1_CTXNO_MAG + T1_NUMCTXS_MAG)
#define T1_CTXNO_UNI (T1_CTXNO_AGG + T1_NUMCTXS_AGG)
#define T1_TYPE_MQ 0
#define T1_TYPE_RAW 1
#define T1_SIGMA_0 (1U << 0)
#define T1_SIGMA_1 (1U << 1)
#define T1_SIGMA_2 (1U << 2)
#define T1_SIGMA_3 (1U << 3)
#define T1_SIGMA_4 (1U << 4)
#define T1_SIGMA_5 (1U << 5)
#define T1_SIGMA_6 (1U << 6)
#define T1_SIGMA_7 (1U << 7)
#define T1_SIGMA_8 (1U << 8)
#define T1_SIGMA_15 (1U << 15)
#define T1_SIGMA_16 (1U << 16)
#define T1_SIGMA_17 (1U << 17)
#define T1_CHI_0_I 18
#define T1_CHI_1 (1U << 19)
#define T1_CHI_1_I 19
#define T1_MU_0 (1U << 20)
#define T1_PI_0 (1U << 21)
#define T1_CHI_2 (1U << 22)
#define T1_CHI_2_I 22
#define T1_PI_1 (1U << 24)
#define T1_PI_2 (1U << 27)
#define T1_PI_3 (1U << 30)
#define T1_CHI_5_I 31
#define T1_SIGMA_NW T1_SIGMA_0
#define T1_SIGMA_N T1_SIGMA_1
#define T1_SIGMA_NE T1_SIGMA_2
#define T1_SIGMA_W T1_SIGMA_3
#define T1_SIGMA_THIS T1_SIGMA_4
#define T1_SIGMA_E T1_SIGMA_5
#define T1_SIGMA_SW T1_SIGMA_6
#define T1_SIGMA_S T1_SIGMA_7
#define T1_SIGMA_SE T1_SIGMA_8
#define T1_SIGMA_NEIGHBOURS \
    (T1_SIGMA_NW | T1_SIGMA_N | T1_SIGMA_NE | T1_SIGMA_W | T1_SIGMA_E | T1_SIGMA_SW | T1_SIGMA_S | T1_SIGMA_SE)
#define T1_CHI_THIS T1_CHI_1
#define T1_CHI_THIS_I T1_CHI_1_I
#define T1_MU_THIS T1_MU_0
#define T1_PI_THIS T1_PI_0

// t1.c
#define T1_FLAGS(x, y) (t1->flags[x + 1 + ((y / 4) + 1) * (t1->w + 2)])
#define opj_t1_setcurctx(curctx, ctxno) curctx = &(mqc)->ctxs[(OPJ_UINT32)(ctxno)]

// The Tier-1 trace of the lane-per-code-block MQ prototype bench (bench/nj2k_mq_lanes.cpp, stage
// 4): a copy of the library built with NJ2K_T1_TRACE reports every MQ segment and decision. The
// library itself is built without it, and the hooks compile to nothing.
#if defined(NJ2K_T1_TRACE)
#define NJ2K_T1_TRACE_DECISION(ctx, d) g_t1_trace.decision(g_t1_trace.user, static_cast<std::uint32_t>(ctx), (d))
#else
#define NJ2K_T1_TRACE_DECISION(ctx, d)
#endif

// ---- mqc_inl.h: the MQ decoder ----

/* For internal use of opj_mqc_decode_macro() */
#define opj_mqc_mpsexchange_macro(d, curctx, a) \
{ \
    if (a < (*curctx)->qeval) { \
        d = !((*curctx)->mps); \
        *curctx = (*curctx)->nlps; \
    } else { \
        d = (*curctx)->mps; \
        *curctx = (*curctx)->nmps; \
    } \
}

/* For internal use of opj_mqc_decode_macro() */
#define opj_mqc_lpsexchange_macro(d, curctx, a) \
{ \
    if (a < (*curctx)->qeval) { \
        a = (*curctx)->qeval; \
        d = (*curctx)->mps; \
        *curctx = (*curctx)->nmps; \
    } else { \
        a = (*curctx)->qeval; \
        d = !((*curctx)->mps); \
        *curctx = (*curctx)->nlps; \
    } \
}


/**
Decode a symbol using raw-decoder. Cfr p.506 TAUBMAN
@param mqc MQC handle
@return Returns the decoded symbol (0 or 1)
*/
static inline OPJ_UINT32 opj_mqc_raw_decode(opj_mqc_t *mqc)
{
    OPJ_UINT32 d;
    if (mqc->ct == 0) {
        /* Given opj_mqc_raw_init_dec() we know that at some point we will */
        /* have a 0xFF 0xFF artificial marker */
        if (mqc->c == 0xff) {
            if (*mqc->bp  > 0x8f) {
                mqc->c = 0xff;
                mqc->ct = 8;
            } else {
                mqc->c = *mqc->bp;
                mqc->bp ++;
                mqc->ct = 7;
            }
        } else {
            mqc->c = *mqc->bp;
            mqc->bp ++;
            mqc->ct = 8;
        }
    }
    mqc->ct--;
    d = ((OPJ_UINT32)mqc->c >> mqc->ct) & 0x01U;

    return d;
}


#define opj_mqc_bytein_macro(mqc, c, ct) \
{ \
        OPJ_UINT32 l_c;  \
        /* Given opj_mqc_init_dec() we know that at some point we will */ \
        /* have a 0xFF 0xFF artificial marker */ \
        l_c = *(mqc->bp + 1); \
        if (*mqc->bp == 0xff) { \
            if (l_c > 0x8f) { \
                c += 0xff00; \
                ct = 8; \
                mqc->end_of_byte_stream_counter ++; \
            } else { \
                mqc->bp++; \
                c += l_c << 9; \
                ct = 7; \
            } \
        } else { \
            mqc->bp++; \
            c += l_c << 8; \
            ct = 8; \
        } \
}

// The renormalisation's shifts for a in [1, 0x7fff]: until a reaches 0x8000 (15 - floor(log2 a)),
// with the instructions of the build's floor: lzcnt in the AVX2 build (x86-64-v3 has it), bsr in
// the scalar one (MSVC's std::countl_zero would pick lzcnt at run time, which the scalar build's
// object check refuses)
inline OPJ_UINT32 mq_shifts(OPJ_UINT32 a) noexcept
{
#if defined(_MSC_VER) && !defined(__clang__)
#if defined(__AVX2__)
    return _lzcnt_u32(a) - 16U;
#else
    unsigned long msb;
    _BitScanReverse(&msb, a);
    return 15U - static_cast<OPJ_UINT32>(msb);
#endif
#else
    return static_cast<OPJ_UINT32>(__builtin_clz(a)) - 16U;
#endif
}

/* For internal use of opj_mqc_decode_macro() */
/* neo-j2k (stage 4): the shifts counted at once (count-leading-zeros) and taken as many at a time */
/* as c has bits left, a byte read whenever none is left: the same byte reads at the same shifts */
/* as OpenJPEG's one-bit loop (a is 1 to 0x7fff here: 1 to 15 shifts) */
#define opj_mqc_renormd_macro(mqc, a, c, ct) \
{ \
    OPJ_UINT32 shifts_ = mq_shifts(a); \
    do { \
        if (ct == 0) { \
            opj_mqc_bytein_macro(mqc, c, ct); \
        } \
        const OPJ_UINT32 k_ = shifts_ < ct ? shifts_ : ct; \
        a <<= k_; \
        c <<= k_; \
        ct -= k_; \
        shifts_ -= k_; \
    } while (shifts_ != 0); \
}

#define opj_mqc_decode_macro(d, mqc, curctx, a, c, ct) \
{ \
    /* Implements ISO 15444-1 C.3.2 Decoding a decision (DECODE) */ \
    /* Note: alternate "J.2 - Decoding an MPS or an LPS in the */ \
    /* software-conventions decoder" has been tried, but does not bring any */ \
    /* improvement. See https://github.com/uclouvain/openjpeg/issues/921 */ \
    a -= (*curctx)->qeval;  \
    if ((c >> 16) < (*curctx)->qeval) {  \
        opj_mqc_lpsexchange_macro(d, curctx, a);  \
        opj_mqc_renormd_macro(mqc, a, c, ct);  \
    } else {  \
        c -= (*curctx)->qeval << 16;  \
        if ((a & 0x8000) == 0) { \
            opj_mqc_mpsexchange_macro(d, curctx, a); \
            opj_mqc_renormd_macro(mqc, a, c, ct); \
        } else { \
            d = (*curctx)->mps; \
        } \
    } \
    NJ2K_T1_TRACE_DECISION(curctx - mqc->ctxs, d); \
}

#define DOWNLOAD_MQC_VARIABLES(mqc, curctx, a, c, ct) \
        const opj_mqc_state_t **curctx = mqc->curctx; \
        OPJ_UINT32 c = mqc->c; \
        OPJ_UINT32 a = mqc->a; \
        OPJ_UINT32 ct = mqc->ct

#define UPLOAD_MQC_VARIABLES(mqc, curctx, a, c, ct) \
        mqc->curctx = curctx; \
        mqc->c = c; \
        mqc->a = a; \
        mqc->ct = ct;

/**
Input a byte
@param mqc MQC handle
*/
static inline void opj_mqc_bytein(opj_mqc_t *const mqc)
{
    opj_mqc_bytein_macro(mqc, mqc->c, mqc->ct);
}

/**
Renormalize mqc->a and mqc->c while decoding
@param mqc MQC handle
*/
#define opj_mqc_renormd(mqc) \
    opj_mqc_renormd_macro(mqc, mqc->a, mqc->c, mqc->ct)

/**
Decode a symbol
@param d OPJ_UINT32 value where to store the decoded symbol
@param mqc MQC handle
@return Returns the decoded symbol (0 or 1) in d
*/
#define opj_mqc_decode(d, mqc) \
    opj_mqc_decode_macro(d, mqc, mqc->curctx, mqc->a, mqc->c, mqc->ct)

// ---- mqc.c: the states and the decoder's set-up ----

static void opj_mqc_init_dec_common(opj_mqc_t *mqc,
                                    OPJ_BYTE *bp,
                                    OPJ_UINT32 len,
                                    OPJ_UINT32 extra_writable_bytes)
{
    (void)extra_writable_bytes;

    assert(extra_writable_bytes >= OPJ_COMMON_CBLK_DATA_EXTRA);
    mqc->start = bp;
    mqc->end = bp + len;
    /* Insert an artificial 0xFF 0xFF marker at end of the code block */
    /* data so that the bytein routines stop on it. This saves us comparing */
    /* the bp and end pointers */
    /* But before inserting it, backup th bytes we will overwrite */
    memcpy(mqc->backup, mqc->end, OPJ_COMMON_CBLK_DATA_EXTRA);
    mqc->end[0] = 0xFF;
    mqc->end[1] = 0xFF;
    mqc->bp = bp;
}
void opj_mqc_init_dec(opj_mqc_t *mqc, OPJ_BYTE *bp, OPJ_UINT32 len,
                      OPJ_UINT32 extra_writable_bytes)
{
    /* Implements ISO 15444-1 C.3.5 Initialization of the decoder (INITDEC) */
    /* Note: alternate "J.1 - Initialization of the software-conventions */
    /* decoder" has been tried, but does */
    /* not bring any improvement. */
    /* See https://github.com/uclouvain/openjpeg/issues/921 */
    opj_mqc_init_dec_common(mqc, bp, len, extra_writable_bytes);
    opj_mqc_setcurctx(mqc, 0);
    mqc->end_of_byte_stream_counter = 0;
    if (len == 0) {
        mqc->c = 0xff << 16;
    } else {
        mqc->c = (OPJ_UINT32)(*mqc->bp << 16);
    }

    opj_mqc_bytein(mqc);
    mqc->c <<= 7;
    mqc->ct -= 7;
    mqc->a = 0x8000;
}


void opj_mqc_raw_init_dec(opj_mqc_t *mqc, OPJ_BYTE *bp, OPJ_UINT32 len,
                          OPJ_UINT32 extra_writable_bytes)
{
    opj_mqc_init_dec_common(mqc, bp, len, extra_writable_bytes);
    mqc->c = 0;
    mqc->ct = 0;
}


void opq_mqc_finish_dec(opj_mqc_t *mqc)
{
    /* Restore the bytes overwritten by opj_mqc_init_dec_common() */
    memcpy(mqc->end, mqc->backup, OPJ_COMMON_CBLK_DATA_EXTRA);
}

void opj_mqc_resetstates(opj_mqc_t *mqc)
{
    OPJ_UINT32 i;
    for (i = 0; i < MQC_NUMCTXS; i++) {
        mqc->ctxs[i] = kMqcStates;
    }
}

void opj_mqc_setstate(opj_mqc_t *mqc, OPJ_UINT32 ctxno, OPJ_UINT32 msb,
                      OPJ_INT32 prob)
{
    mqc->ctxs[ctxno] = &kMqcStates[msb + (OPJ_UINT32)(prob << 1)];
}

// ---- t1_luts.h: the context tables (decoding) ----



// ---- t1.c: the decoding passes ----

static inline OPJ_BYTE opj_t1_getctxno_zc(opj_mqc_t *mqc, OPJ_UINT32 f)
{
    return mqc->lut_ctxno_zc_orient[(f & T1_SIGMA_NEIGHBOURS)];
}

static inline OPJ_UINT32 opj_t1_getctxtno_sc_or_spb_index(OPJ_UINT32 fX,
        OPJ_UINT32 pfX,
        OPJ_UINT32 nfX,
        OPJ_UINT32 ci)
{
    /*
      0 pfX T1_CHI_THIS           T1_LUT_SGN_W
      1 tfX T1_SIGMA_1            T1_LUT_SIG_N
      2 nfX T1_CHI_THIS           T1_LUT_SGN_E
      3 tfX T1_SIGMA_3            T1_LUT_SIG_W
      4  fX T1_CHI_(THIS - 1)     T1_LUT_SGN_N
      5 tfX T1_SIGMA_5            T1_LUT_SIG_E
      6  fX T1_CHI_(THIS + 1)     T1_LUT_SGN_S
      7 tfX T1_SIGMA_7            T1_LUT_SIG_S
    */

    OPJ_UINT32 lu = (fX >> (ci * 3U)) & (T1_SIGMA_1 | T1_SIGMA_3 | T1_SIGMA_5 |
                                         T1_SIGMA_7);

    lu |= (pfX >> (T1_CHI_THIS_I      + (ci * 3U))) & (1U << 0);
    lu |= (nfX >> (T1_CHI_THIS_I - 2U + (ci * 3U))) & (1U << 2);
    if (ci == 0U) {
        lu |= (fX >> (T1_CHI_0_I - 4U)) & (1U << 4);
    } else {
        lu |= (fX >> (T1_CHI_1_I - 4U + ((ci - 1U) * 3U))) & (1U << 4);
    }
    lu |= (fX >> (T1_CHI_2_I - 6U + (ci * 3U))) & (1U << 6);
    return lu;
}

static inline OPJ_BYTE opj_t1_getctxno_sc(OPJ_UINT32 lu)
{
    return kLutCtxnoSc[lu];
}

static inline OPJ_UINT32 opj_t1_getctxno_mag(OPJ_UINT32 f)
{
    OPJ_UINT32 tmp = (f & T1_SIGMA_NEIGHBOURS) ? T1_CTXNO_MAG + 1 : T1_CTXNO_MAG;
    OPJ_UINT32 tmp2 = (f & T1_MU_0) ? T1_CTXNO_MAG + 2 : tmp;
    return tmp2;
}

static inline OPJ_BYTE opj_t1_getspb(OPJ_UINT32 lu)
{
    return kLutSpb[lu];
}


#define opj_t1_update_flags_macro(flags, flagsp, ci, s, stride, vsc) \
{ \
    /* east */ \
    flagsp[-1] |= T1_SIGMA_5 << (3U * ci); \
 \
    /* mark target as significant */ \
    flags |= ((s << T1_CHI_1_I) | T1_SIGMA_4) << (3U * ci); \
 \
    /* west */ \
    flagsp[1] |= T1_SIGMA_3 << (3U * ci); \
 \
    /* north-west, north, north-east */ \
    if (ci == 0U && !(vsc)) { \
        opj_flag_t* north = flagsp - (stride); \
        *north |= (s << T1_CHI_5_I) | T1_SIGMA_16; \
        north[-1] |= T1_SIGMA_17; \
        north[1] |= T1_SIGMA_15; \
    } \
 \
    /* south-west, south, south-east */ \
    if (ci == 3U) { \
        opj_flag_t* south = flagsp + (stride); \
        *south |= (s << T1_CHI_0_I) | T1_SIGMA_1; \
        south[-1] |= T1_SIGMA_2; \
        south[1] |= T1_SIGMA_0; \
    } \
}


static inline void opj_t1_update_flags(opj_flag_t *flagsp, OPJ_UINT32 ci,
                                       OPJ_UINT32 s, OPJ_UINT32 stride,
                                       OPJ_UINT32 vsc)
{
    opj_t1_update_flags_macro(*flagsp, flagsp, ci, s, stride, vsc);
}

static inline void opj_t1_dec_sigpass_step_raw(
    opj_t1_t *t1,
    opj_flag_t *flagsp,
    OPJ_INT32 *datap,
    OPJ_INT32 oneplushalf,
    OPJ_UINT32 vsc,
    OPJ_UINT32 ci)
{
    OPJ_UINT32 v;
    opj_mqc_t *mqc = &(t1->mqc);       /* RAW component */

    OPJ_UINT32 const flags = *flagsp;

    if ((flags & ((T1_SIGMA_THIS | T1_PI_THIS) << (ci * 3U))) == 0U &&
            (flags & (T1_SIGMA_NEIGHBOURS << (ci * 3U))) != 0U) {
        if (opj_mqc_raw_decode(mqc)) {
            v = opj_mqc_raw_decode(mqc);
            *datap = v ? -oneplushalf : oneplushalf;
            opj_t1_update_flags(flagsp, ci, v, t1->w + 2, vsc);
        }
        *flagsp |= T1_PI_THIS << (ci * 3U);
    }
}

#define opj_t1_dec_sigpass_step_mqc_macro(flags, flagsp, flags_stride, data, \
                                          data_stride, ci, mqc, curctx, \
                                          v, a, c, ct, oneplushalf, vsc) \
{ \
    if ((flags & ((T1_SIGMA_THIS | T1_PI_THIS) << (ci * 3U))) == 0U && \
        (flags & (T1_SIGMA_NEIGHBOURS << (ci * 3U))) != 0U) { \
        OPJ_UINT32 ctxt1 = opj_t1_getctxno_zc(mqc, flags >> (ci * 3U)); \
        opj_t1_setcurctx(curctx, ctxt1); \
        opj_mqc_decode_macro(v, mqc, curctx, a, c, ct); \
        if (v) { \
            OPJ_UINT32 lu = opj_t1_getctxtno_sc_or_spb_index( \
                                flags, \
                                flagsp[-1], flagsp[1], \
                                ci); \
            OPJ_UINT32 ctxt2 = opj_t1_getctxno_sc(lu); \
            OPJ_UINT32 spb = opj_t1_getspb(lu); \
            opj_t1_setcurctx(curctx, ctxt2); \
            opj_mqc_decode_macro(v, mqc, curctx, a, c, ct); \
            v = v ^ spb; \
            data[ci*data_stride] = v ? -oneplushalf : oneplushalf; \
            opj_t1_update_flags_macro(flags, flagsp, ci, v, flags_stride, vsc); \
        } \
        flags |= T1_PI_THIS << (ci * 3U); \
    } \
}

static inline void opj_t1_dec_sigpass_step_mqc(
    opj_t1_t *t1,
    opj_flag_t *flagsp,
    OPJ_INT32 *datap,
    OPJ_INT32 oneplushalf,
    OPJ_UINT32 ci,
    OPJ_UINT32 flags_stride,
    OPJ_UINT32 vsc)
{
    OPJ_UINT32 v;

    opj_mqc_t *mqc = &(t1->mqc);       /* MQC component */
    opj_t1_dec_sigpass_step_mqc_macro(*flagsp, flagsp, flags_stride, datap,
                                      0, ci, mqc, mqc->curctx,
                                      v, mqc->a, mqc->c, mqc->ct, oneplushalf, vsc);
}

static void opj_t1_dec_sigpass_raw(
    opj_t1_t *t1,
    OPJ_INT32 bpno,
    OPJ_INT32 cblksty)
{
    OPJ_INT32 one, half, oneplushalf;
    OPJ_UINT32 i, j, k;
    OPJ_INT32 *data = t1->data;
    opj_flag_t *flagsp = &T1_FLAGS(0, 0);
    const OPJ_UINT32 l_w = t1->w;
    one = 1 << bpno;
    half = one >> 1;
    oneplushalf = one | half;

    for (k = 0; k < (t1->h & ~3U); k += 4, flagsp += 2, data += 3 * l_w) {
        for (i = 0; i < l_w; ++i, ++flagsp, ++data) {
            opj_flag_t flags = *flagsp;
            if (flags != 0) {
                opj_t1_dec_sigpass_step_raw(
                    t1,
                    flagsp,
                    data,
                    oneplushalf,
                    cblksty & J2K_CCP_CBLKSTY_VSC, /* vsc */
                    0U);
                opj_t1_dec_sigpass_step_raw(
                    t1,
                    flagsp,
                    data + l_w,
                    oneplushalf,
                    OPJ_FALSE, /* vsc */
                    1U);
                opj_t1_dec_sigpass_step_raw(
                    t1,
                    flagsp,
                    data + 2 * l_w,
                    oneplushalf,
                    OPJ_FALSE, /* vsc */
                    2U);
                opj_t1_dec_sigpass_step_raw(
                    t1,
                    flagsp,
                    data + 3 * l_w,
                    oneplushalf,
                    OPJ_FALSE, /* vsc */
                    3U);
            }
        }
    }
    if (k < t1->h) {
        for (i = 0; i < l_w; ++i, ++flagsp, ++data) {
            for (j = 0; j < t1->h - k; ++j) {
                opj_t1_dec_sigpass_step_raw(
                    t1,
                    flagsp,
                    data + j * l_w,
                    oneplushalf,
                    cblksty & J2K_CCP_CBLKSTY_VSC, /* vsc */
                    j);
            }
        }
    }
}

#define opj_t1_dec_sigpass_mqc_internal(t1, bpno, vsc, w, h, flags_stride) \
{ \
        OPJ_INT32 one, half, oneplushalf; \
        OPJ_UINT32 i, j, k; \
        OPJ_INT32 *data = t1->data; \
        opj_flag_t *flagsp = &t1->flags[(flags_stride) + 1]; \
        const OPJ_UINT32 l_w = w; \
        opj_mqc_t* mqc = &(t1->mqc); \
        DOWNLOAD_MQC_VARIABLES(mqc, curctx, a, c, ct); \
        OPJ_UINT32 v; \
        one = 1 << bpno; \
        half = one >> 1; \
        oneplushalf = one | half; \
        for (k = 0; k < (h & ~3u); k += 4, data += 3*l_w, flagsp += 2) { \
                for (i = 0; i < l_w; ++i, ++data, ++flagsp) { \
                        opj_flag_t flags = *flagsp; \
                        if( flags != 0 ) { \
                            opj_t1_dec_sigpass_step_mqc_macro( \
                                flags, flagsp, flags_stride, data, \
                                l_w, 0, mqc, curctx, v, a, c, ct, oneplushalf, vsc); \
                            opj_t1_dec_sigpass_step_mqc_macro( \
                                flags, flagsp, flags_stride, data, \
                                l_w, 1, mqc, curctx, v, a, c, ct, oneplushalf, OPJ_FALSE); \
                            opj_t1_dec_sigpass_step_mqc_macro( \
                                flags, flagsp, flags_stride, data, \
                                l_w, 2, mqc, curctx, v, a, c, ct, oneplushalf, OPJ_FALSE); \
                            opj_t1_dec_sigpass_step_mqc_macro( \
                                flags, flagsp, flags_stride, data, \
                                l_w, 3, mqc, curctx, v, a, c, ct, oneplushalf, OPJ_FALSE); \
                            *flagsp = flags; \
                        } \
                } \
        } \
        UPLOAD_MQC_VARIABLES(mqc, curctx, a, c, ct); \
        if( k < h ) { \
            for (i = 0; i < l_w; ++i, ++data, ++flagsp) { \
                for (j = 0; j < h - k; ++j) { \
                        opj_t1_dec_sigpass_step_mqc(t1, flagsp, \
                            data + j * l_w, oneplushalf, j, flags_stride, vsc); \
                } \
            } \
        } \
}

static void opj_t1_dec_sigpass_mqc_64x64_novsc(
    opj_t1_t *t1,
    OPJ_INT32 bpno)
{
    opj_t1_dec_sigpass_mqc_internal(t1, bpno, OPJ_FALSE, 64, 64, 66);
}

static void opj_t1_dec_sigpass_mqc_64x64_vsc(
    opj_t1_t *t1,
    OPJ_INT32 bpno)
{
    opj_t1_dec_sigpass_mqc_internal(t1, bpno, OPJ_TRUE, 64, 64, 66);
}

static void opj_t1_dec_sigpass_mqc_generic_novsc(
    opj_t1_t *t1,
    OPJ_INT32 bpno)
{
    opj_t1_dec_sigpass_mqc_internal(t1, bpno, OPJ_FALSE, t1->w, t1->h,
                                    t1->w + 2U);
}

static void opj_t1_dec_sigpass_mqc_generic_vsc(
    opj_t1_t *t1,
    OPJ_INT32 bpno)
{
    opj_t1_dec_sigpass_mqc_internal(t1, bpno, OPJ_TRUE, t1->w, t1->h,
                                    t1->w + 2U);
}

static void opj_t1_dec_sigpass_mqc(
    opj_t1_t *t1,
    OPJ_INT32 bpno,
    OPJ_INT32 cblksty)
{
    if (t1->w == 64 && t1->h == 64) {
        if (cblksty & J2K_CCP_CBLKSTY_VSC) {
            opj_t1_dec_sigpass_mqc_64x64_vsc(t1, bpno);
        } else {
            opj_t1_dec_sigpass_mqc_64x64_novsc(t1, bpno);
        }
    } else {
        if (cblksty & J2K_CCP_CBLKSTY_VSC) {
            opj_t1_dec_sigpass_mqc_generic_vsc(t1, bpno);
        } else {
            opj_t1_dec_sigpass_mqc_generic_novsc(t1, bpno);
        }
    }
}

static inline void opj_t1_dec_refpass_step_raw(
    opj_t1_t *t1,
    opj_flag_t *flagsp,
    OPJ_INT32 *datap,
    OPJ_INT32 poshalf,
    OPJ_UINT32 ci)
{
    OPJ_UINT32 v;

    opj_mqc_t *mqc = &(t1->mqc);       /* RAW component */

    if ((*flagsp & ((T1_SIGMA_THIS | T1_PI_THIS) << (ci * 3U))) ==
            (T1_SIGMA_THIS << (ci * 3U))) {
        v = opj_mqc_raw_decode(mqc);
        *datap += (v ^ (*datap < 0)) ? poshalf : -poshalf;
        *flagsp |= T1_MU_THIS << (ci * 3U);
    }
}

#define opj_t1_dec_refpass_step_mqc_macro(flags, data, data_stride, ci, \
                                          mqc, curctx, v, a, c, ct, poshalf) \
{ \
    if ((flags & ((T1_SIGMA_THIS | T1_PI_THIS) << (ci * 3U))) == \
            (T1_SIGMA_THIS << (ci * 3U))) { \
        OPJ_UINT32 ctxt = opj_t1_getctxno_mag(flags >> (ci * 3U)); \
        opj_t1_setcurctx(curctx, ctxt); \
        opj_mqc_decode_macro(v, mqc, curctx, a, c, ct); \
        data[ci*data_stride] += (v ^ (data[ci*data_stride] < 0)) ? poshalf : -poshalf; \
        flags |= T1_MU_THIS << (ci * 3U); \
    } \
}

static inline void opj_t1_dec_refpass_step_mqc(
    opj_t1_t *t1,
    opj_flag_t *flagsp,
    OPJ_INT32 *datap,
    OPJ_INT32 poshalf,
    OPJ_UINT32 ci)
{
    OPJ_UINT32 v;

    opj_mqc_t *mqc = &(t1->mqc);       /* MQC component */
    opj_t1_dec_refpass_step_mqc_macro(*flagsp, datap, 0, ci,
                                      mqc, mqc->curctx, v, mqc->a, mqc->c,
                                      mqc->ct, poshalf);
}

static void opj_t1_dec_refpass_raw(
    opj_t1_t *t1,
    OPJ_INT32 bpno)
{
    OPJ_INT32 one, poshalf;
    OPJ_UINT32 i, j, k;
    OPJ_INT32 *data = t1->data;
    opj_flag_t *flagsp = &T1_FLAGS(0, 0);
    const OPJ_UINT32 l_w = t1->w;
    one = 1 << bpno;
    poshalf = one >> 1;
    for (k = 0; k < (t1->h & ~3U); k += 4, flagsp += 2, data += 3 * l_w) {
        for (i = 0; i < l_w; ++i, ++flagsp, ++data) {
            opj_flag_t flags = *flagsp;
            if (flags != 0) {
                opj_t1_dec_refpass_step_raw(
                    t1,
                    flagsp,
                    data,
                    poshalf,
                    0U);
                opj_t1_dec_refpass_step_raw(
                    t1,
                    flagsp,
                    data + l_w,
                    poshalf,
                    1U);
                opj_t1_dec_refpass_step_raw(
                    t1,
                    flagsp,
                    data + 2 * l_w,
                    poshalf,
                    2U);
                opj_t1_dec_refpass_step_raw(
                    t1,
                    flagsp,
                    data + 3 * l_w,
                    poshalf,
                    3U);
            }
        }
    }
    if (k < t1->h) {
        for (i = 0; i < l_w; ++i, ++flagsp, ++data) {
            for (j = 0; j < t1->h - k; ++j) {
                opj_t1_dec_refpass_step_raw(
                    t1,
                    flagsp,
                    data + j * l_w,
                    poshalf,
                    j);
            }
        }
    }
}

#define opj_t1_dec_refpass_mqc_internal(t1, bpno, w, h, flags_stride) \
{ \
        OPJ_INT32 one, poshalf; \
        OPJ_UINT32 i, j, k; \
        OPJ_INT32 *data = t1->data; \
        opj_flag_t *flagsp = &t1->flags[flags_stride + 1]; \
        const OPJ_UINT32 l_w = w; \
        opj_mqc_t* mqc = &(t1->mqc); \
        DOWNLOAD_MQC_VARIABLES(mqc, curctx, a, c, ct); \
        OPJ_UINT32 v; \
        one = 1 << bpno; \
        poshalf = one >> 1; \
        for (k = 0; k < (h & ~3u); k += 4, data += 3*l_w, flagsp += 2) { \
                for (i = 0; i < l_w; ++i, ++data, ++flagsp) { \
                        opj_flag_t flags = *flagsp; \
                        if( flags != 0 ) { \
                            opj_t1_dec_refpass_step_mqc_macro( \
                                flags, data, l_w, 0, \
                                mqc, curctx, v, a, c, ct, poshalf); \
                            opj_t1_dec_refpass_step_mqc_macro( \
                                flags, data, l_w, 1, \
                                mqc, curctx, v, a, c, ct, poshalf); \
                            opj_t1_dec_refpass_step_mqc_macro( \
                                flags, data, l_w, 2, \
                                mqc, curctx, v, a, c, ct, poshalf); \
                            opj_t1_dec_refpass_step_mqc_macro( \
                                flags, data, l_w, 3, \
                                mqc, curctx, v, a, c, ct, poshalf); \
                            *flagsp = flags; \
                        } \
                } \
        } \
        UPLOAD_MQC_VARIABLES(mqc, curctx, a, c, ct); \
        if( k < h ) { \
            for (i = 0; i < l_w; ++i, ++data, ++flagsp) { \
                for (j = 0; j < h - k; ++j) { \
                        opj_t1_dec_refpass_step_mqc(t1, flagsp, data + j * l_w, poshalf, j); \
                } \
            } \
        } \
}

static void opj_t1_dec_refpass_mqc_64x64(
    opj_t1_t *t1,
    OPJ_INT32 bpno)
{
    opj_t1_dec_refpass_mqc_internal(t1, bpno, 64, 64, 66);
}

static void opj_t1_dec_refpass_mqc_generic(
    opj_t1_t *t1,
    OPJ_INT32 bpno)
{
    opj_t1_dec_refpass_mqc_internal(t1, bpno, t1->w, t1->h, t1->w + 2U);
}

static void opj_t1_dec_refpass_mqc(
    opj_t1_t *t1,
    OPJ_INT32 bpno)
{
    if (t1->w == 64 && t1->h == 64) {
        opj_t1_dec_refpass_mqc_64x64(t1, bpno);
    } else {
        opj_t1_dec_refpass_mqc_generic(t1, bpno);
    }
}

#define opj_t1_dec_clnpass_step_macro(check_flags, partial, \
                                      flags, flagsp, flags_stride, data, \
                                      data_stride, ci, mqc, curctx, \
                                      v, a, c, ct, oneplushalf, vsc) \
{ \
    if ( !check_flags || !(flags & ((T1_SIGMA_THIS | T1_PI_THIS) << (ci * 3U)))) {\
        do { \
            if( !partial ) { \
                OPJ_UINT32 ctxt1 = opj_t1_getctxno_zc(mqc, flags >> (ci * 3U)); \
                opj_t1_setcurctx(curctx, ctxt1); \
                opj_mqc_decode_macro(v, mqc, curctx, a, c, ct); \
                if( !v ) \
                    break; \
            } \
            { \
                OPJ_UINT32 lu = opj_t1_getctxtno_sc_or_spb_index( \
                                    flags, flagsp[-1], flagsp[1], \
                                    ci); \
                opj_t1_setcurctx(curctx, opj_t1_getctxno_sc(lu)); \
                opj_mqc_decode_macro(v, mqc, curctx, a, c, ct); \
                v = v ^ opj_t1_getspb(lu); \
                data[ci*data_stride] = v ? -oneplushalf : oneplushalf; \
                opj_t1_update_flags_macro(flags, flagsp, ci, v, flags_stride, vsc); \
            } \
        } while(0); \
    } \
}

static void opj_t1_dec_clnpass_step(
    opj_t1_t *t1,
    opj_flag_t *flagsp,
    OPJ_INT32 *datap,
    OPJ_INT32 oneplushalf,
    OPJ_UINT32 ci,
    OPJ_UINT32 vsc)
{
    OPJ_UINT32 v;

    opj_mqc_t *mqc = &(t1->mqc);   /* MQC component */
    opj_t1_dec_clnpass_step_macro(OPJ_TRUE, OPJ_FALSE,
                                  *flagsp, flagsp, t1->w + 2U, datap,
                                  0, ci, mqc, mqc->curctx,
                                  v, mqc->a, mqc->c, mqc->ct, oneplushalf, vsc);
}

#define opj_t1_dec_clnpass_internal(t1, bpno, vsc, w, h, flags_stride) \
{ \
    OPJ_INT32 one, half, oneplushalf; \
    OPJ_UINT32 runlen; \
    OPJ_UINT32 i, j, k; \
    const OPJ_UINT32 l_w = w; \
    opj_mqc_t* mqc = &(t1->mqc); \
    OPJ_INT32 *data = t1->data; \
    opj_flag_t *flagsp = &t1->flags[flags_stride + 1]; \
    DOWNLOAD_MQC_VARIABLES(mqc, curctx, a, c, ct); \
    OPJ_UINT32 v; \
    one = 1 << bpno; \
    half = one >> 1; \
    oneplushalf = one | half; \
    for (k = 0; k < (h & ~3u); k += 4, data += 3*l_w, flagsp += 2) { \
        for (i = 0; i < l_w; ++i, ++data, ++flagsp) { \
            opj_flag_t flags = *flagsp; \
            if (flags == 0) { \
                OPJ_UINT32 partial = OPJ_TRUE; \
                opj_t1_setcurctx(curctx, T1_CTXNO_AGG); \
                opj_mqc_decode_macro(v, mqc, curctx, a, c, ct); \
                if (!v) { \
                    continue; \
                } \
                opj_t1_setcurctx(curctx, T1_CTXNO_UNI); \
                opj_mqc_decode_macro(runlen, mqc, curctx, a, c, ct); \
                opj_mqc_decode_macro(v, mqc, curctx, a, c, ct); \
                runlen = (runlen << 1) | v; \
                switch(runlen) { \
                    case 0: \
                        opj_t1_dec_clnpass_step_macro(OPJ_FALSE, OPJ_TRUE,\
                                            flags, flagsp, flags_stride, data, \
                                            l_w, 0, mqc, curctx, \
                                            v, a, c, ct, oneplushalf, vsc); \
                        partial = OPJ_FALSE; \
                        /* FALLTHRU */ \
                    case 1: \
                        opj_t1_dec_clnpass_step_macro(OPJ_FALSE, partial,\
                                            flags, flagsp, flags_stride, data, \
                                            l_w, 1, mqc, curctx, \
                                            v, a, c, ct, oneplushalf, OPJ_FALSE); \
                        partial = OPJ_FALSE; \
                        /* FALLTHRU */ \
                    case 2: \
                        opj_t1_dec_clnpass_step_macro(OPJ_FALSE, partial,\
                                            flags, flagsp, flags_stride, data, \
                                            l_w, 2, mqc, curctx, \
                                            v, a, c, ct, oneplushalf, OPJ_FALSE); \
                        partial = OPJ_FALSE; \
                        /* FALLTHRU */ \
                    case 3: \
                        opj_t1_dec_clnpass_step_macro(OPJ_FALSE, partial,\
                                            flags, flagsp, flags_stride, data, \
                                            l_w, 3, mqc, curctx, \
                                            v, a, c, ct, oneplushalf, OPJ_FALSE); \
                        break; \
                } \
            } else { \
                opj_t1_dec_clnpass_step_macro(OPJ_TRUE, OPJ_FALSE, \
                                    flags, flagsp, flags_stride, data, \
                                    l_w, 0, mqc, curctx, \
                                    v, a, c, ct, oneplushalf, vsc); \
                opj_t1_dec_clnpass_step_macro(OPJ_TRUE, OPJ_FALSE, \
                                    flags, flagsp, flags_stride, data, \
                                    l_w, 1, mqc, curctx, \
                                    v, a, c, ct, oneplushalf, OPJ_FALSE); \
                opj_t1_dec_clnpass_step_macro(OPJ_TRUE, OPJ_FALSE, \
                                    flags, flagsp, flags_stride, data, \
                                    l_w, 2, mqc, curctx, \
                                    v, a, c, ct, oneplushalf, OPJ_FALSE); \
                opj_t1_dec_clnpass_step_macro(OPJ_TRUE, OPJ_FALSE, \
                                    flags, flagsp, flags_stride, data, \
                                    l_w, 3, mqc, curctx, \
                                    v, a, c, ct, oneplushalf, OPJ_FALSE); \
            } \
            *flagsp = flags & ~(T1_PI_0 | T1_PI_1 | T1_PI_2 | T1_PI_3); \
        } \
    } \
    UPLOAD_MQC_VARIABLES(mqc, curctx, a, c, ct); \
    if( k < h ) { \
        for (i = 0; i < l_w; ++i, ++flagsp, ++data) { \
            for (j = 0; j < h - k; ++j) { \
                opj_t1_dec_clnpass_step(t1, flagsp, data + j * l_w, oneplushalf, j, vsc); \
            } \
            *flagsp &= ~(T1_PI_0 | T1_PI_1 | T1_PI_2 | T1_PI_3); \
        } \
    } \
}

static void opj_t1_dec_clnpass_check_segsym(opj_t1_t *t1, OPJ_INT32 cblksty)
{
    if (cblksty & J2K_CCP_CBLKSTY_SEGSYM) {
        opj_mqc_t* mqc = &(t1->mqc);
        OPJ_UINT32 v, v2;
        opj_mqc_setcurctx(mqc, T1_CTXNO_UNI);
        opj_mqc_decode(v, mqc);
        opj_mqc_decode(v2, mqc);
        v = (v << 1) | v2;
        opj_mqc_decode(v2, mqc);
        v = (v << 1) | v2;
        opj_mqc_decode(v2, mqc);
        v = (v << 1) | v2;
        /*
        if (v!=0xa) {
            opj_event_msg(t1->cinfo, EVT_WARNING, "Bad segmentation symbol %x\n", v);
        }
        */
    }
}

static void opj_t1_dec_clnpass_64x64_novsc(
    opj_t1_t *t1,
    OPJ_INT32 bpno)
{
    opj_t1_dec_clnpass_internal(t1, bpno, OPJ_FALSE, 64, 64, 66);
}

static void opj_t1_dec_clnpass_64x64_vsc(
    opj_t1_t *t1,
    OPJ_INT32 bpno)
{
    opj_t1_dec_clnpass_internal(t1, bpno, OPJ_TRUE, 64, 64, 66);
}

static void opj_t1_dec_clnpass_generic_novsc(
    opj_t1_t *t1,
    OPJ_INT32 bpno)
{
    opj_t1_dec_clnpass_internal(t1, bpno, OPJ_FALSE, t1->w, t1->h,
                                t1->w + 2U);
}

static void opj_t1_dec_clnpass_generic_vsc(
    opj_t1_t *t1,
    OPJ_INT32 bpno)
{
    opj_t1_dec_clnpass_internal(t1, bpno, OPJ_TRUE, t1->w, t1->h,
                                t1->w + 2U);
}

static void opj_t1_dec_clnpass(
    opj_t1_t *t1,
    OPJ_INT32 bpno,
    OPJ_INT32 cblksty)
{
    if (t1->w == 64 && t1->h == 64) {
        if (cblksty & J2K_CCP_CBLKSTY_VSC) {
            opj_t1_dec_clnpass_64x64_vsc(t1, bpno);
        } else {
            opj_t1_dec_clnpass_64x64_novsc(t1, bpno);
        }
    } else {
        if (cblksty & J2K_CCP_CBLKSTY_VSC) {
            opj_t1_dec_clnpass_generic_vsc(t1, bpno);
        } else {
            opj_t1_dec_clnpass_generic_novsc(t1, bpno);
        }
    }
    opj_t1_dec_clnpass_check_segsym(t1, cblksty);
}

#undef opj_t1_setcurctx

// opj_t1_allocate_buffers, on the decoder's fixed buffers (T1::ready): the coefficients cleared,
// the flags cleared with OpenJPEG's guard rows and the partial last stripe marked
void allocate_buffers(opj_t1_t* t1, OPJ_UINT32 w, OPJ_UINT32 h)
{
    std::memset(t1->data, 0, std::size_t(w) * h * sizeof(OPJ_INT32));
    const OPJ_UINT32 flags_stride = w + 2U;
    const OPJ_UINT32 flags_height = (h + 3U) / 4U;
    const OPJ_UINT32 flagssize = (flags_height + 2U) * flags_stride;
    std::memset(t1->flags, 0, flagssize * sizeof(opj_flag_t));
    opj_flag_t* p = &t1->flags[0];
    for (OPJ_UINT32 x = 0; x < flags_stride; ++x) {
        *p++ = (T1_PI_0 | T1_PI_1 | T1_PI_2 | T1_PI_3);
    }
    p = &t1->flags[((flags_height + 1) * flags_stride)];
    for (OPJ_UINT32 x = 0; x < flags_stride; ++x) {
        *p++ = (T1_PI_0 | T1_PI_1 | T1_PI_2 | T1_PI_3);
    }
    if (h % 4) {
        OPJ_UINT32 v = 0;
        p = &t1->flags[((flags_height)*flags_stride)];
        if (h % 4 == 1) {
            v |= T1_PI_1 | T1_PI_2 | T1_PI_3;
        } else if (h % 4 == 2) {
            v |= T1_PI_2 | T1_PI_3;
        } else if (h % 4 == 3) {
            v |= T1_PI_3;
        }
        for (OPJ_UINT32 x = 0; x < flags_stride; ++x) {
            *p++ = v;
        }
    }
    t1->w = w;
    t1->h = h;
}

bool bitplanes_ok(const T1Block& b, const Messages& msg) noexcept
{
    const OPJ_INT32 bpno_plus_one = (OPJ_INT32)(b.roishift + b.numbps);
    if (bpno_plus_one >= 31) {
        msg.emit(Messages::Warning, "opj_t1_decode_cblk(): unsupported bpno_plus_one = %d >= 31\n", bpno_plus_one);
        return false;
    }
    return true;
}

}  // namespace

bool T1::ready(const Allocator& alloc) noexcept
{
    if (data) {
        return true;
    }
    data = static_cast<std::int32_t*>(alloc.allocate(kMaxData * sizeof(std::int32_t), 64));
    flags = static_cast<std::uint32_t*>(alloc.allocate(kMaxFlags * sizeof(std::uint32_t), 64));
    if (!data || !flags) {
        release(alloc);
        return false;
    }
    return true;
}

bool T1::reserve(const Allocator& alloc, std::size_t n) noexcept
{
    if (n > SIZE_MAX - OPJ_COMMON_CBLK_DATA_EXTRA) {
        return false;
    }
    const std::size_t need = n + OPJ_COMMON_CBLK_DATA_EXTRA;
    if (need <= cblkdata_size) {
        return true;
    }
    std::size_t size = cblkdata_size ? cblkdata_size : 4096;
    while (size < need) {
        size = size > SIZE_MAX / 2 ? need : size * 2;
    }
    auto* p = static_cast<std::uint8_t*>(alloc.allocate(size, 64));
    if (!p) {
        return false;
    }
    if (cblkdata) {
        alloc.release(cblkdata, cblkdata_size);
    }
    cblkdata = p;
    cblkdata_size = size;
    return true;
}

void T1::release(const Allocator& alloc) noexcept
{
    if (data) {
        alloc.release(data, kMaxData * sizeof(std::int32_t));
    }
    if (flags) {
        alloc.release(flags, kMaxFlags * sizeof(std::uint32_t));
    }
    if (cblkdata) {
        alloc.release(cblkdata, cblkdata_size);
    }
    data = nullptr;
    flags = nullptr;
    cblkdata = nullptr;
    cblkdata_size = 0;
}

std::size_t T1::bytes() const noexcept
{
    return (data ? kMaxData * sizeof(std::int32_t) + kMaxFlags * sizeof(std::uint32_t) : 0) + cblkdata_size;
}

bool t1_empty_cblk_ok(const T1Block& b, const Messages& msg) noexcept
{
    return bitplanes_ok(b, msg);
}

// opj_t1_decode_cblk. The code-block's chunks are always gathered into t1.cblkdata (OpenJPEG reads
// a single chunk where it lies, in its own copy of the tile's data); the decode is the same.
bool t1_decode_cblk(T1& t1_ref, const T1Block& b, const Messages& msg) noexcept
{
    opj_t1_t* t1 = &t1_ref;
    opj_mqc_t* mqc = &(t1->mqc);
    const OPJ_UINT32 orient = b.orient, roishift = b.roishift, cblksty = b.cblksty;

    mqc->lut_ctxno_zc_orient = kLutCtxnoZc + (orient << 9);
    allocate_buffers(t1, b.w, b.h);

    OPJ_INT32 bpno_plus_one = (OPJ_INT32)(roishift + b.numbps);
    if (!bitplanes_ok(b, msg)) {
        return false;
    }
    OPJ_UINT32 passtype = 2;

    opj_mqc_resetstates(mqc);
    opj_mqc_setstate(mqc, T1_CTXNO_UNI, 0, 46);
    opj_mqc_setstate(mqc, T1_CTXNO_AGG, 0, 3);
    opj_mqc_setstate(mqc, T1_CTXNO_ZC, 0, 4);

    if (b.nchunks == 0) {
        return true;
    }
    OPJ_BYTE* cblkdata = t1->cblkdata;
    OPJ_UINT32 cblk_len = 0;
    for (OPJ_UINT32 i = 0; i < b.nchunks; i++) {
        if (b.chunks[i].len) {
            std::memcpy(cblkdata + cblk_len, b.chunks[i].data, b.chunks[i].len);
        }
        cblk_len += b.chunks[i].len;
    }
    // the decoder reads at most the two bytes of the sentinel past the data, which
    // opj_mqc_init_dec writes first; cleared all the same, as OpenJPEG's grown buffer is
    cblkdata[cblk_len] = 0;
    cblkdata[cblk_len + 1] = 0;

    OPJ_UINT32 cblkdataindex = 0;
    for (OPJ_UINT32 segno = 0; segno < b.nsegs; ++segno) {
        const Segment* seg = &b.segs[segno];

        /* BYPASS mode */
        const OPJ_BYTE type = ((bpno_plus_one <= ((OPJ_INT32)(b.numbps)) - 4) && (passtype < 2) &&
                               (cblksty & J2K_CCP_CBLKSTY_LAZY))
                                  ? T1_TYPE_RAW
                                  : T1_TYPE_MQ;

        if (type == T1_TYPE_RAW) {
            opj_mqc_raw_init_dec(mqc, cblkdata + cblkdataindex, seg->len, OPJ_COMMON_CBLK_DATA_EXTRA);
        } else {
            opj_mqc_init_dec(mqc, cblkdata + cblkdataindex, seg->len, OPJ_COMMON_CBLK_DATA_EXTRA);
#if defined(NJ2K_T1_TRACE)
            {
                std::uint8_t states[MQC_NUMCTXS];
                for (OPJ_UINT32 k = 0; k < MQC_NUMCTXS; ++k) {
                    states[k] = static_cast<std::uint8_t>(mqc->ctxs[k] - kMqcStates);
                }
                g_t1_trace.segment(g_t1_trace.user, cblkdata + cblkdataindex, seg->len, states,
                                   (cblksty & J2K_CCP_CBLKSTY_RESET) != 0);
            }
#endif
        }
        cblkdataindex += seg->len;

        for (OPJ_UINT32 passno = 0; (passno < seg->real_num_passes) && (bpno_plus_one >= 1); ++passno) {
            switch (passtype) {
            case 0:
                if (type == T1_TYPE_RAW) {
                    opj_t1_dec_sigpass_raw(t1, bpno_plus_one, (OPJ_INT32)cblksty);
                } else {
                    opj_t1_dec_sigpass_mqc(t1, bpno_plus_one, (OPJ_INT32)cblksty);
                }
                break;
            case 1:
                if (type == T1_TYPE_RAW) {
                    opj_t1_dec_refpass_raw(t1, bpno_plus_one);
                } else {
                    opj_t1_dec_refpass_mqc(t1, bpno_plus_one);
                }
                break;
            case 2:
                opj_t1_dec_clnpass(t1, bpno_plus_one, (OPJ_INT32)cblksty);
                break;
            }

            if ((cblksty & J2K_CCP_CBLKSTY_RESET) && type == T1_TYPE_MQ) {
                opj_mqc_resetstates(mqc);
                opj_mqc_setstate(mqc, T1_CTXNO_UNI, 0, 46);
                opj_mqc_setstate(mqc, T1_CTXNO_AGG, 0, 3);
                opj_mqc_setstate(mqc, T1_CTXNO_ZC, 0, 4);
            }
            if (++passtype == 3) {
                passtype = 0;
                bpno_plus_one--;
            }
        }

        opq_mqc_finish_dec(mqc);
    }
    return true;
}

#if defined(NJ2K_T1_TRACE)
T1Trace g_t1_trace{};

const MqState* t1_trace_states() noexcept
{
    return kMqcStates;
}
#endif

namespace kernels {

// positive -> round down: (83)/2 = 41.5 -> 41; negative -> round up: (-83)/2 = -41.5 -> -41
void store53_scalar(const std::int32_t* src, std::uint32_t w, std::uint32_t h, std::int32_t* dst,
                    std::size_t stride) noexcept
{
    for (std::uint32_t j = 0; j < h; ++j) {
        const std::int32_t* in = src + std::size_t(j) * w;
        std::int32_t* out = dst + std::size_t(j) * stride;
        for (std::uint32_t i = 0; i < w; ++i) {
            out[i] = in[i] / 2;
        }
    }
}

void store97_scalar(const std::int32_t* src, std::uint32_t w, std::uint32_t h, float half_step, std::int32_t* dst,
                    std::size_t stride) noexcept
{
    for (std::uint32_t j = 0; j < h; ++j) {
        const std::int32_t* in = src + std::size_t(j) * w;
        std::int32_t* out = dst + std::size_t(j) * stride;
        for (std::uint32_t i = 0; i < w; ++i) {
            const float v = static_cast<float>(in[i]) * half_step;
            std::memcpy(out + i, &v, sizeof v);
        }
    }
}

#if defined(__AVX2__)
// The same conversion and product as store97_scalar, eight at a time (OpenJPEG's loop, which its
// compiler vectorises the same way): cvtdq2ps rounds as the scalar conversion does
void store97_avx2(const std::int32_t* src, std::uint32_t w, std::uint32_t h, float half_step, std::int32_t* dst,
                  std::size_t stride) noexcept
{
    const __m256 vstep = _mm256_set1_ps(half_step);
    for (std::uint32_t j = 0; j < h; ++j) {
        const std::int32_t* in = src + std::size_t(j) * w;
        std::int32_t* out = dst + std::size_t(j) * stride;
        std::uint32_t i = 0;
        for (; i + 8 <= w; i += 8) {
            const __m256 v = _mm256_cvtepi32_ps(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(in + i)));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + i), _mm256_castps_si256(_mm256_mul_ps(v, vstep)));
        }
        for (; i < w; ++i) {
            const float v = static_cast<float>(in[i]) * half_step;
            std::memcpy(out + i, &v, sizeof v);
        }
    }
}

// OpenJPEG's AVX2 path: (x + (x >>> 31)) >> 1, which is x / 2 rounded towards zero
void store53_avx2(const std::int32_t* src, std::uint32_t w, std::uint32_t h, std::int32_t* dst,
                  std::size_t stride) noexcept
{
    for (std::uint32_t j = 0; j < h; ++j) {
        const std::int32_t* in = src + std::size_t(j) * w;
        std::int32_t* out = dst + std::size_t(j) * stride;
        std::uint32_t i = 0;
        for (; i + 8 <= w; i += 8) {
            __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(in + i));
            v = _mm256_add_epi32(v, _mm256_srli_epi32(v, 31));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + i), _mm256_srai_epi32(v, 1));
        }
        for (; i < w; ++i) {
            out[i] = in[i] / 2;
        }
    }
}
#endif

}  // namespace kernels

void t1_store(const std::int32_t* src, std::uint32_t w, std::uint32_t h, std::uint32_t roishift, bool reversible,
              float stepsize, std::int32_t* dst, std::size_t stride) noexcept
{
    if (!src) {
        // a code-block without data: its coefficients are 0, in either representation
        for (std::uint32_t j = 0; j < h; ++j) {
            std::memset(dst + std::size_t(j) * stride, 0, std::size_t(w) * sizeof(std::int32_t));
        }
        return;
    }
    if (roishift) {
        // opj_t1_clbl_decode_processor, on the decoder's own buffer
        auto* data = const_cast<std::int32_t*>(src);
        const std::size_t n = std::size_t(w) * h;
        if (roishift >= 31) {
            std::memset(data, 0, n * sizeof(std::int32_t));
        } else {
            const std::int32_t thresh = 1 << roishift;
            for (std::size_t i = 0; i < n; ++i) {
                const std::int32_t val = data[i];
                // abs() of INT_MIN stays INT_MIN, as in OpenJPEG; computed without overflow here
                std::int32_t mag = val < 0 ? static_cast<std::int32_t>(0u - static_cast<std::uint32_t>(val)) : val;
                if (mag >= thresh) {
                    mag >>= roishift;
                    data[i] = val < 0 ? -mag : mag;
                }
            }
        }
    }
    if (reversible) {
#if defined(__AVX2__)
        kernels::store53_avx2(src, w, h, dst, stride);
#else
        kernels::store53_scalar(src, w, h, dst, stride);
#endif
    } else {
#if defined(__AVX2__)
        kernels::store97_avx2(src, w, h, 0.5f * stepsize, dst, stride);
#else
        kernels::store97_scalar(src, w, h, 0.5f * stepsize, dst, stride);
#endif
    }
}

}  // namespace nj2k::detail

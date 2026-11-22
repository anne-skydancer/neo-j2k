// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The MQ region of the packed variants, but for the decision itself (opj_mqc_decode_macro, which the variant
// defines before including this file): the byte input, the renormalisation, raw decoding, set-up and states
// are the library's operations (src/t1.cpp mqc_inl.h and mqc.c), on context slots of STUDY_SLOT_T holding
// STUDY_SLOT_OF(index) for the state index (state * 2 + MPS).
#if !defined(STUDY_DECISION)
#define STUDY_DECISION(mqc, curctx, d)
#endif
#if !defined(STUDY_SITE)
#define STUDY_SITE(k, x)
#endif
#if !defined(STUDY_BYTEIN)
#define STUDY_BYTEIN(marker)
#endif
#if !defined(STUDY_RENORM)
#define STUDY_RENORM(shifts)
#endif
#if !defined(STUDY_SEGMENT)
#define STUDY_SEGMENT(mqc, len)
#endif

// the library's helper: lzcnt in the AVX2 build, bsr in the scalar one
inline OPJ_UINT32 mq_shifts(OPJ_UINT32 a) noexcept
{
#if defined(__AVX2__)
    return _lzcnt_u32(a) - 16U;
#else
    unsigned long msb;
    _BitScanReverse(&msb, a);
    return 15U - static_cast<OPJ_UINT32>(msb);
#endif
}

#if !defined(STUDY_BYTE0)
// the bytes at bp and bp + 1: the sentinel the set-up wrote stops the reads at end + 1
#define STUDY_BYTE0(mqc) (*(mqc)->bp)
#define STUDY_BYTE1(mqc) (*((mqc)->bp + 1))
#define STUDY_SENTINEL 1
#endif

#if !defined(STUDY_RAWDEC)
#define STUDY_RAWDEC()
#endif

static inline OPJ_UINT32 opj_mqc_raw_decode(opj_mqc_t *mqc)
{
    OPJ_UINT32 d;
    STUDY_RAWDEC();
    if (mqc->ct == 0) {
        if (mqc->c == 0xff) {
            if (STUDY_BYTE0(mqc) > 0x8f) {
                mqc->c = 0xff;
                mqc->ct = 8;
            } else {
                mqc->c = STUDY_BYTE0(mqc);
                mqc->bp ++;
                mqc->ct = 7;
            }
        } else {
            mqc->c = STUDY_BYTE0(mqc);
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
        l_c = STUDY_BYTE1(mqc); \
        if (STUDY_BYTE0(mqc) == 0xff) { \
            if (l_c > 0x8f) { \
                c += 0xff00; \
                ct = 8; \
                mqc->end_of_byte_stream_counter ++; \
                STUDY_BYTEIN(1); \
            } else { \
                mqc->bp++; \
                c += l_c << 9; \
                ct = 7; \
                STUDY_BYTEIN(0); \
            } \
        } else { \
            mqc->bp++; \
            c += l_c << 8; \
            ct = 8; \
            STUDY_BYTEIN(0); \
        } \
}

#define opj_mqc_renormd_macro(mqc, a, c, ct) \
{ \
    OPJ_UINT32 shifts_ = mq_shifts(a); \
    STUDY_RENORM(shifts_); \
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

// the renormalisation always run after a select decision: no shift when bit 15 is set, the shifts at once
// when c has the bits, the library's loop (a byte read when none is left) otherwise
#define mq_renorm_always(mqc, a, c, ct) \
{ \
    const OPJ_UINT32 s0_ = mq_shifts(a); \
    if (s0_ <= ct) { \
        a <<= s0_; \
        c <<= s0_; \
        ct -= s0_; \
    } else { \
        opj_mqc_renormd_macro(mqc, a, c, ct); \
    } \
}

#define opj_mqc_setcurctx(mqc, ctxno) (mqc)->curctx = &(mqc)->st[(OPJ_UINT32)(ctxno)]
#define opj_t1_setcurctx(curctx, ctxno) curctx = &(mqc)->st[(OPJ_UINT32)(ctxno)]

#define DOWNLOAD_MQC_VARIABLES(mqc, curctx, a, c, ct) \
        STUDY_SLOT_T *curctx = mqc->curctx; \
        OPJ_UINT32 c = mqc->c; \
        OPJ_UINT32 a = mqc->a; \
        OPJ_UINT32 ct = mqc->ct

#define UPLOAD_MQC_VARIABLES(mqc, curctx, a, c, ct) \
        mqc->curctx = curctx; \
        mqc->c = c; \
        mqc->a = a; \
        mqc->ct = ct;

static inline void opj_mqc_bytein(opj_mqc_t *const mqc)
{
    opj_mqc_bytein_macro(mqc, mqc->c, mqc->ct);
}

#define opj_mqc_decode(d, mqc) \
    opj_mqc_decode_macro(d, mqc, mqc->curctx, mqc->a, mqc->c, mqc->ct)

static void opj_mqc_init_dec_common(opj_mqc_t *mqc, OPJ_BYTE *bp, OPJ_UINT32 len, OPJ_UINT32 extra_writable_bytes)
{
    (void)extra_writable_bytes;
    mqc->start = bp;
    mqc->end = bp + len;
#if STUDY_SENTINEL
    memcpy(mqc->backup, mqc->end, OPJ_COMMON_CBLK_DATA_EXTRA);
    mqc->end[0] = 0xFF;
    mqc->end[1] = 0xFF;
#endif
    mqc->bp = bp;
}

void opj_mqc_init_dec(opj_mqc_t *mqc, OPJ_BYTE *bp, OPJ_UINT32 len, OPJ_UINT32 extra_writable_bytes)
{
    opj_mqc_init_dec_common(mqc, bp, len, extra_writable_bytes);
    STUDY_SEGMENT(mqc, len);
    opj_mqc_setcurctx(mqc, 0);
    mqc->end_of_byte_stream_counter = 0;
    if (len == 0) {
        mqc->c = 0xff << 16;
    } else {
        mqc->c = (OPJ_UINT32)(STUDY_BYTE0(mqc) << 16);
    }
    opj_mqc_bytein(mqc);
    mqc->c <<= 7;
    mqc->ct -= 7;
    mqc->a = 0x8000;
}

void opj_mqc_raw_init_dec(opj_mqc_t *mqc, OPJ_BYTE *bp, OPJ_UINT32 len, OPJ_UINT32 extra_writable_bytes)
{
    opj_mqc_init_dec_common(mqc, bp, len, extra_writable_bytes);
    mqc->c = 0;
    mqc->ct = 0;
}

void opq_mqc_finish_dec(opj_mqc_t *mqc)
{
#if STUDY_SENTINEL
    memcpy(mqc->end, mqc->backup, OPJ_COMMON_CBLK_DATA_EXTRA);
#else
    (void)mqc;
#endif
}

void opj_mqc_resetstates(opj_mqc_t *mqc)
{
    for (OPJ_UINT32 i = 0; i < MQC_NUMCTXS; i++) {
        mqc->st[i] = STUDY_SLOT_OF(0);
    }
}

void opj_mqc_setstate(opj_mqc_t *mqc, OPJ_UINT32 ctxno, OPJ_UINT32 msb, OPJ_INT32 prob)
{
    mqc->st[ctxno] = STUDY_SLOT_OF(msb + (OPJ_UINT32)(prob << 1));
}

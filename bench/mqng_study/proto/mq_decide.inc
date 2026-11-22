// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The decision of ISO 15444-1 C.3.2 (DECODE) four ways; a variant picks one as opj_mqc_decode_macro.
// All four are the library's function: the MPS takes qe when a - qe < qe and keeps a - qe otherwise, the
// LPS interval the opposite, and an MPS that keeps bit 15 neither renormalises nor changes state.
//   flip = (c_hi < qe) ^ (a - qe < qe);  d = mps ^ flip;
//   the state changes when c_hi < qe or a - qe < 0x8000, to nlps when flip and to nmps otherwise.

// on byte slots (the state index), today's branches
#define mq_decide_branchy(d, mqc, curctx, a, c, ct) \
{ \
    const OPJ_UINT32 i_ = *(curctx); \
    const mqs::PackedState e_ = mqs::kPacked.s[i_]; \
    const OPJ_UINT32 qe_ = e_.qe; \
    a -= qe_; \
    if ((c >> 16) < qe_) { \
        if (a < qe_) { \
            d = i_ & 1U; \
            *(curctx) = e_.nmps; \
        } else { \
            d = (i_ & 1U) ^ 1U; \
            *(curctx) = e_.nlps; \
        } \
        a = qe_; \
        opj_mqc_renormd_macro(mqc, a, c, ct); \
    } else { \
        c -= qe_ << 16; \
        if ((a & 0x8000) == 0) { \
            if (a < qe_) { \
                d = (i_ & 1U) ^ 1U; \
                *(curctx) = e_.nlps; \
            } else { \
                d = i_ & 1U; \
                *(curctx) = e_.nmps; \
            } \
            opj_mqc_renormd_macro(mqc, a, c, ct); \
        } else { \
            d = i_ & 1U; \
        } \
    } \
}

// on byte slots, the choices as selects, the renormalisation always run
#define mq_decide_select(d, mqc, curctx, a, c, ct) \
{ \
    const OPJ_UINT32 i_ = *(curctx); \
    const mqs::PackedState e_ = mqs::kPacked.s[i_]; \
    const OPJ_UINT32 qe_ = e_.qe; \
    a -= qe_; \
    const OPJ_UINT32 lpsi_ = (c >> 16) < qe_; \
    const OPJ_UINT32 flip_ = lpsi_ ^ (OPJ_UINT32)(a < qe_); \
    const OPJ_UINT32 lm_ = 0U - lpsi_; \
    d = (i_ & 1U) ^ flip_; \
    const OPJ_UINT32 n_ = flip_ ? e_.nlps : e_.nmps; \
    *(curctx) = (std::uint8_t)((lpsi_ | (OPJ_UINT32)(a < 0x8000U)) ? n_ : i_); \
    c -= (qe_ << 16) & ~lm_; \
    a = (a & ~lm_) | (qe_ & lm_); \
    mq_renorm_always(mqc, a, c, ct); \
}

// on word slots (the state's qe, MPS and successors in the slot), today's branches
#define mq_decide_word_branchy(d, mqc, curctx, a, c, ct) \
{ \
    const OPJ_UINT32 w_ = *(curctx); \
    const OPJ_UINT32 qe_ = w_ & 0xFFFFU; \
    const OPJ_UINT32 m_ = (w_ >> 16) & 1U; \
    a -= qe_; \
    if ((c >> 16) < qe_) { \
        if (a < qe_) { \
            d = m_; \
            *(curctx) = mqs::kWords.w[(w_ >> 17) & 0x7FU]; \
        } else { \
            d = m_ ^ 1U; \
            *(curctx) = mqs::kWords.w[w_ >> 24]; \
        } \
        a = qe_; \
        opj_mqc_renormd_macro(mqc, a, c, ct); \
    } else { \
        c -= qe_ << 16; \
        if ((a & 0x8000) == 0) { \
            if (a < qe_) { \
                d = m_ ^ 1U; \
                *(curctx) = mqs::kWords.w[w_ >> 24]; \
            } else { \
                d = m_; \
                *(curctx) = mqs::kWords.w[(w_ >> 17) & 0x7FU]; \
            } \
            opj_mqc_renormd_macro(mqc, a, c, ct); \
        } else { \
            d = m_; \
        } \
    } \
}

// on word slots, selects (the _pre form with the slot's word already loaded)
#define mq_decide_word_select(d, mqc, curctx, a, c, ct) mq_decide_word_select_pre(d, *(curctx), mqc, curctx, a, c, ct)
#define mq_decide_word_select_pre(d, w0, mqc, curctx, a, c, ct) \
{ \
    const OPJ_UINT32 w_ = (w0); \
    const OPJ_UINT32 qe_ = w_ & 0xFFFFU; \
    a -= qe_; \
    const OPJ_UINT32 lpsi_ = (c >> 16) < qe_; \
    const OPJ_UINT32 flip_ = lpsi_ ^ (OPJ_UINT32)(a < qe_); \
    const OPJ_UINT32 lm_ = 0U - lpsi_; \
    d = ((w_ >> 16) & 1U) ^ flip_; \
    const OPJ_UINT32 n_ = flip_ ? (w_ >> 24) : ((w_ >> 17) & 0x7FU); \
    const OPJ_UINT32 nw_ = mqs::kWords.w[n_]; \
    *(curctx) = (lpsi_ | (OPJ_UINT32)(a < 0x8000U)) ? nw_ : w_; \
    c -= (qe_ << 16) & ~lm_; \
    a = (a & ~lm_) | (qe_ & lm_); \
    mq_renorm_always(mqc, a, c, ct); \
}

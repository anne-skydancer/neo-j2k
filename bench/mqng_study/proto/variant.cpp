// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// One decoder variant of the MQ-NG study, chosen by STUDY_V at compile time (bench/mqng_study/CMakeLists.txt
// compiles this file once a variant). Every variant is the library's src/t1.cpp decoder (the generated body)
// with only the parts named below changed.
//   0 copy    the library's text unchanged (noise control: the same source in another object)
//   1 pb      states as byte indices into a packed 4-byte table, today's branches
//   2 ps      packed, the decision by selects and the renormalisation always run (every pass)
//   3 wb      the state word in the context slot (qe, MPS, successors), today's branches
//   4 ws      state words, selects
//   5 mix     packed; branches where the decoded bit steers the pass (significance, cleanup), selects in the
//             refinement pass, whose bit only changes the coefficient, and that update branch-free
//   6 wmix    as mix on state words
//   7 err     ps with a per-decision error check (the Result-per-decode() design)
//   8 count   pb with the tallies and the recording (decisions, segments) of the replays
//   9 model   the passes alone: each decision's bit read from the recording, no MQ decoder
//  10 time    copy with a clock around each pass
//  11 psr     ps with the branch-free refinement update
//  12 mixnd   mix without the branch-free refinement update (the select alone)
//  13 wmixs   wmix, plus selects in the significance pass's sign decision (its bit steers no branch either)
//  14 wagg    ws with AGG MPS runs taken at once (idea 2)
//  15 wspec   ws with the sign's context and slot loaded before the significance decision (idea 3)
//  16 wchk    ws with a bounds-checked, read-only byte input and no sentinel (idea 1 reversed)
//  17 wall    14 + 15 + 16
//  18 wfn     14 with the decision an inline function (the backend seam)
#include "common.h"

#define STR2(x) #x
#define STR(x) STR2(x)

#if STUDY_V == 0 || STUDY_V == 10
#define STUDY_MQC_T Mqc
#else
#define STUDY_OWN_MQ "mq_region.inc"
#endif

#if STUDY_V == 3 || STUDY_V == 4 || STUDY_V == 6 || STUDY_V == 13 || STUDY_V >= 14
#define STUDY_MQC_T mqs::MqcV<std::uint32_t>
#define STUDY_SLOT_T std::uint32_t
#define STUDY_SLOT_OF(i) mqs::kWords.w[(i)]
#elif STUDY_V != 0 && STUDY_V != 10
#define STUDY_MQC_T mqs::MqcV<std::uint8_t>
#define STUDY_SLOT_T std::uint8_t
#define STUDY_SLOT_OF(i) static_cast<std::uint8_t>(i)
#endif

#if STUDY_V == 1 || STUDY_V == 5 || STUDY_V == 12
#define STUDY_DECIDE mq_decide_branchy
#elif STUDY_V == 2 || STUDY_V == 11
#define STUDY_DECIDE mq_decide_select
#elif STUDY_V == 3 || STUDY_V == 6 || STUDY_V == 13
#define STUDY_DECIDE mq_decide_word_branchy
#elif STUDY_V == 4 || STUDY_V >= 14
#define STUDY_DECIDE mq_decide_word_select
#elif STUDY_V == 7
#define STUDY_DECIDE(d, mqc, curctx, a, c, ct) \
    { \
        mq_decide_select(d, mqc, curctx, a, c, ct); \
        if ((mqc)->end_of_byte_stream_counter > 0xFFFFFFF0U) { \
            return; \
        } \
    }
#elif STUDY_V == 8
namespace mqs {
void count_decision(std::uint32_t cx, std::uint32_t i, std::uint32_t a0, std::uint32_t c0, std::uint32_t d) noexcept;
void count_segment(const std::uint8_t* bp, std::uint32_t len, const std::uint8_t* st) noexcept;
}
#define STUDY_DECIDE(d, mqc, curctx, a, c, ct) \
    { \
        const std::uint32_t cx_ = static_cast<std::uint32_t>((curctx) - (mqc)->st); \
        const std::uint32_t i0_ = *(curctx), a0_ = a, c0_ = c; \
        mq_decide_branchy(d, mqc, curctx, a, c, ct); \
        mqs::count_decision(cx_, i0_, a0_, c0_, d); \
    }
#define STUDY_BYTEIN(marker) (++mqs::g_counts.byteins, mqs::g_counts.markers += (marker))
#define STUDY_RENORM(s) (++mqs::g_counts.renorms, mqs::g_counts.shifts += (s))
#define STUDY_RAWDEC() (++mqs::g_counts.raw_decisions)
#define STUDY_SEGMENT(mqc, len) mqs::count_segment((mqc)->bp, (len), (mqc)->st)
#define STUDY_PASS_BEGIN() \
    (mqs::g_counts.pass_now = passtype, type == T1_TYPE_RAW ? ++mqs::g_counts.raw_passes : ++mqs::g_counts.passes[passtype])
#elif STUDY_V == 9
#define STUDY_DECIDE(d, mqc, curctx, a, c, ct) \
    { \
        d = *mqs::g_bits++; \
    }
#endif

#if STUDY_V == 10
namespace mqs {
extern std::uint64_t g_pass_cycles[4], g_pass_count[4];
}
#define STUDY_PASS_BEGIN() const std::uint64_t pass_t0_ = __rdtsc()
#define STUDY_PASS_END(p, t) \
    (mqs::g_pass_cycles[(t) == T1_TYPE_RAW ? 3 : (p)] += __rdtsc() - pass_t0_, ++mqs::g_pass_count[(t) == T1_TYPE_RAW ? 3 : (p)])
#endif

// the refinement step with its own decision (and update)
#if STUDY_V == 5 || STUDY_V == 6 || STUDY_V == 11 || STUDY_V == 12 || STUDY_V == 13
#if STUDY_V == 5 || STUDY_V == 12
#define STUDY_REF_DECIDE mq_decide_select
#elif STUDY_V == 6 || STUDY_V == 13
#define STUDY_REF_DECIDE mq_decide_word_select
#else
#define STUDY_REF_DECIDE STUDY_DECIDE
#endif
#if STUDY_V == 12
#define STUDY_REF_UPDATE(v, x, poshalf) x += ((v) ^ (x < 0)) ? poshalf : -poshalf
#else
// (v ^ negative) ? +poshalf : -poshalf, as two's complement arithmetic: n is 0 or -1
#define STUDY_REF_UPDATE(v, x, poshalf) \
    { \
        const OPJ_INT32 n_ = static_cast<OPJ_INT32>((static_cast<OPJ_UINT32>(v) ^ static_cast<OPJ_UINT32>(x < 0)) - 1U); \
        x += (poshalf ^ n_) - n_; \
    }
#endif
#define STUDY_OVR_opj_t1_dec_refpass_step_mqc_macro
#define opj_t1_dec_refpass_step_mqc_macro(flags, data, data_stride, ci, mqc, curctx, v, a, c, ct, poshalf) \
    { \
        if ((flags & ((T1_SIGMA_THIS | T1_PI_THIS) << (ci * 3U))) == (T1_SIGMA_THIS << (ci * 3U))) { \
            OPJ_UINT32 ctxt = opj_t1_getctxno_mag(flags >> (ci * 3U)); \
            opj_t1_setcurctx(curctx, ctxt); \
            STUDY_REF_DECIDE(v, mqc, curctx, a, c, ct); \
            STUDY_REF_UPDATE(v, data[ci * data_stride], poshalf); \
            flags |= T1_MU_THIS << (ci * 3U); \
        } \
    }
#endif

#if STUDY_V == 13
// the significance step with the sign decided by selects (its bit only picks the coefficient's sign and
// the neighbours' sign flags)
#define STUDY_OVR_opj_t1_dec_sigpass_step_mqc_macro
#define opj_t1_dec_sigpass_step_mqc_macro(flags, flagsp, flags_stride, data, data_stride, ci, mqc, curctx, v, a, c, \
                                          ct, oneplushalf, vsc) \
    { \
        if ((flags & ((T1_SIGMA_THIS | T1_PI_THIS) << (ci * 3U))) == 0U && \
            (flags & (T1_SIGMA_NEIGHBOURS << (ci * 3U))) != 0U) { \
            OPJ_UINT32 ctxt1 = opj_t1_getctxno_zc(mqc, flags >> (ci * 3U)); \
            opj_t1_setcurctx(curctx, ctxt1); \
            opj_mqc_decode_macro(v, mqc, curctx, a, c, ct); \
            if (v) { \
                OPJ_UINT32 lu = opj_t1_getctxtno_sc_or_spb_index(flags, flagsp[-1], flagsp[1], ci); \
                OPJ_UINT32 ctxt2 = opj_t1_getctxno_sc(lu); \
                OPJ_UINT32 spb = opj_t1_getspb(lu); \
                opj_t1_setcurctx(curctx, ctxt2); \
                mq_decide_word_select(v, mqc, curctx, a, c, ct); \
                v = v ^ spb; \
                { \
                    const OPJ_INT32 n_ = -static_cast<OPJ_INT32>(v); \
                    data[ci * data_stride] = (oneplushalf ^ n_) - n_; \
                } \
                opj_t1_update_flags_macro(flags, flagsp, ci, v, flags_stride, vsc); \
            } \
            flags |= T1_PI_THIS << (ci * 3U); \
        } \
    }
#endif

#if STUDY_V == 18
// the backend seam: wagg with the decision an inline function on the coder's registers (what a C++ or
// Rust implementation with a Decoder type would write) instead of the macro; the same operations
namespace nj2k::detail::v18 {
template <class M>
__forceinline std::uint32_t decide_fn(M* mqc, std::uint32_t* slot, std::uint32_t& a, std::uint32_t& c,
                                      std::uint32_t& ct) noexcept;
}
#undef STUDY_DECIDE
#define STUDY_DECIDE(d, mqc, curctx, a, c, ct) d = decide_fn(mqc, curctx, a, c, ct)
#endif

#if STUDY_V == 14 || STUDY_V == 17 || STUDY_V == 18
// idea 2: a run of AGG decisions that are MPS (symbol 0) without renormalisation, over columns with no
// flags, taken at once: step j of the run needs a - j qe >= 0x8000 and c_hi >= j qe, so the run lasts while
// j qe <= min(a - 0x8000, c_hi); the columns skipped have no other effect (a 0 there continues the loop)
#define STUDY_AGG(v, mqc, curctx, a, c, ct) \
    { \
        opj_t1_setcurctx(curctx, T1_CTXNO_AGG); \
        const OPJ_UINT32 wa_ = *(curctx); \
        const OPJ_UINT32 qa_ = wa_ & 0xFFFFU; \
        const OPJ_UINT32 ra_ = a - 0x8000U, rc_ = c >> 16; \
        const OPJ_UINT32 room_ = ra_ < rc_ ? ra_ : rc_; \
        if ((wa_ & 0x10000U) == 0 && qa_ <= room_) { \
            OPJ_UINT32 used_ = qa_, n_ = 1; \
            const OPJ_UINT32 left_ = l_w - i; \
            while (n_ < left_ && flagsp[n_] == 0 && used_ + qa_ <= room_) { \
                used_ += qa_; \
                ++n_; \
            } \
            a -= used_; \
            c -= used_ << 16; \
            i += n_ - 1; \
            data += n_ - 1; \
            flagsp += n_ - 1; \
            v = 0; \
        } else { \
            mq_decide_word_select(v, mqc, curctx, a, c, ct); \
        } \
    }
#endif

#if STUDY_V == 15 || STUDY_V == 17
// idea 3: the sign's context formed and its slot loaded before the significance decision (the decision
// changes only a ZC slot, never the SC one), so the sign decision does not wait for them
#define STUDY_OVR_opj_t1_dec_sigpass_step_mqc_macro
#define opj_t1_dec_sigpass_step_mqc_macro(flags, flagsp, flags_stride, data, data_stride, ci, mqc, curctx, v, a, c, \
                                          ct, oneplushalf, vsc) \
    { \
        if ((flags & ((T1_SIGMA_THIS | T1_PI_THIS) << (ci * 3U))) == 0U && \
            (flags & (T1_SIGMA_NEIGHBOURS << (ci * 3U))) != 0U) { \
            const OPJ_UINT32 lu = opj_t1_getctxtno_sc_or_spb_index(flags, flagsp[-1], flagsp[1], ci); \
            const OPJ_UINT32 ctxt2 = opj_t1_getctxno_sc(lu); \
            const OPJ_UINT32 spb = opj_t1_getspb(lu); \
            const OPJ_UINT32 sw_ = (mqc)->st[ctxt2]; \
            OPJ_UINT32 ctxt1 = opj_t1_getctxno_zc(mqc, flags >> (ci * 3U)); \
            opj_t1_setcurctx(curctx, ctxt1); \
            opj_mqc_decode_macro(v, mqc, curctx, a, c, ct); \
            if (v) { \
                opj_t1_setcurctx(curctx, ctxt2); \
                mq_decide_word_select_pre(v, sw_, mqc, curctx, a, c, ct); \
                v = v ^ spb; \
                data[ci * data_stride] = v ? -oneplushalf : oneplushalf; \
                opj_t1_update_flags_macro(flags, flagsp, ci, v, flags_stride, vsc); \
            } \
            flags |= T1_PI_THIS << (ci * 3U); \
        } \
    }
#endif

#if STUDY_V == 16 || STUDY_V == 17
// idea 1 the other way round: the byte input bounds-checked against end, no sentinel written, the input
// read-only (what a std::span / Rust slice decoder does); the reads past the end give 0xFF as the sentinel did
#define STUDY_BYTE0(mqc) ((mqc)->bp < (mqc)->end ? OPJ_UINT32(*(mqc)->bp) : 0xFFU)
#define STUDY_BYTE1(mqc) ((mqc)->bp + 1 < (mqc)->end ? OPJ_UINT32(*((mqc)->bp + 1)) : 0xFFU)
#define STUDY_SENTINEL 0
#endif

#define STUDY_VNAME_(n) v##n
#define STUDY_VNAME__(n) STUDY_VNAME_(n)
#define STUDY_VNAME STUDY_VNAME__(STUDY_V)
#define STUDY_RUN_(n) run_v##n
#define STUDY_RUN__(n) STUDY_RUN_(n)
#define STUDY_RUN STUDY_RUN__(STUDY_V)

#include "variant_tail.inc"

#if STUDY_V == 18
namespace nj2k::detail::v18 {
template <class M>
__forceinline std::uint32_t decide_fn(M* mqc, std::uint32_t* slot, std::uint32_t& a, std::uint32_t& c,
                                      std::uint32_t& ct) noexcept
{
    std::uint32_t d;
    std::uint32_t* curctx = slot;
    mq_decide_word_select(d, mqc, curctx, a, c, ct);
    return d;
}
}  // namespace nj2k::detail::v18
#endif

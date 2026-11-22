// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The MQ decoder's decision (ISO 15444-1 C.3.2 DECODE with RENORMD and BYTEIN of C.3.4) as macros, the form
// OpenJPEG 2.5.4's mqc_inl.h has and t1.cpp's passes expand on their locals (a, c, ct and the slot of the context
// being decided). The operations differ from OpenJPEG's; the decisions, the registers and the bytes read are the
// same (tests/unit/t1_unit.cpp runs every interval and state against the standard's model and the frozen copy of
// OpenJPEG's text):
//   - a context's state is one 32-bit word in its slot: the interval's qe (bits 0-15), the MPS (bit 16), the
//     index of the next state on an MPS (bits 17-23) and on an LPS (bits 24-30), so a decision loads no second
//     record, and a change of state is one load from the 94 words of kMqWords;
//   - the branches the decoded bit does not steer are selects: the LPS interval, the exchange of the conditional
//     exchange, the state change, the interval and the code register are computed from the comparisons and the
//     renormalisation is always run: no shift when bit 15 of a is set, the shifts at once when the code register
//     has the bits, OpenJPEG's loop with its byte-in only when a byte must come in (0.07-0.11 a decision).
// kMqWords is built at compile time from kMqcStates (t1_tables.h), the standard's Table C.2, which the encoder
// reads too; its own state-index convention (state * 2 + MPS) is the table's.
#pragma once

#include <cstddef>
#include <cstdint>

#include "t1.h"
#include "t1_flags.h"
#include "t1_tables.h"

namespace nj2k::detail {

// (not std::size: <iterator> pulls in an MSVC runtime weak variable the scalar build's object check refuses)
inline constexpr std::size_t kMqWordCount = sizeof(kMqcStates) / sizeof(kMqcStates[0]);

struct MqWordTable {
    std::uint32_t w[kMqWordCount];
    constexpr MqWordTable() noexcept : w{}
    {
        for (std::size_t i = 0; i < kMqWordCount; ++i) {
            w[i] = kMqcStates[i].qeval | (kMqcStates[i].mps << 16) |
                   (static_cast<std::uint32_t>(kMqcStates[i].nmps - kMqcStates) << 17) |
                   (static_cast<std::uint32_t>(kMqcStates[i].nlps - kMqcStates) << 24);
        }
    }
};
inline constexpr MqWordTable kMqWords;

constexpr bool mq_words_ok() noexcept
{
    for (std::size_t i = 0; i < kMqWordCount; ++i) {
        const std::uint32_t w = kMqWords.w[i];
        // the index convention (state * 2 + MPS), a qe that fits 16 bits, every next state an index below 94
        if (kMqcStates[i].mps != (i & 1U) || kMqcStates[i].qeval > 0xFFFFU || ((w >> 17) & 0x7FU) >= kMqWordCount ||
            ((w >> 24) & 0x7FU) >= kMqWordCount || (w >> 31) != 0 || kMqcStates[i].nmps->mps != kMqcStates[i].mps) {
            return false;
        }
    }
    return true;
}
static_assert(kMqWordCount == 94 && sizeof(MqWordTable) == 376, "94 states of 4 bytes");
static_assert(mq_words_ok(), "kMqWords: index = state * 2 + MPS, next states below 94, qe in 16 bits");
// a few rows pinned (Table C.2): state 0 (qe 0x5601, MPS 0: next MPS 1 -> index 2, next LPS 1 with the MPS switched -> 3),
// the UNI context's initial state 46 (index 92), and state 45 (qe 1, stays on an MPS and goes to 43 on an LPS)
static_assert(kMqWords.w[0] == (0x5601U | (2U << 17) | (3U << 24)));
static_assert(kMqWords.w[92] == (0x5601U | (92U << 17) | (92U << 24)));
static_assert(kMqWords.w[90] == (0x0001U | (90U << 17) | (86U << 24)));

// The contexts' initial states (the MqContexts of t1.h): every one at state 0 (index 0) but UNI (state 46, index 92),
// AGG (state 3, index 6) and ZC's first (state 4, index 8).
constexpr MqContexts mq_contexts_init() noexcept
{
    MqContexts c{};
    for (std::size_t i = 0; i < 19; ++i) {
        c.slot[i] = kMqWords.w[0];
    }
    c.slot[T1_CTXNO_UNI] = kMqWords.w[92];
    c.slot[T1_CTXNO_AGG] = kMqWords.w[6];
    c.slot[T1_CTXNO_ZC] = kMqWords.w[8];
    return c;
}
inline constexpr MqContexts kMqContextsInit = mq_contexts_init();

}  // namespace nj2k::detail

// ---- the macros: they expand in t1.cpp on std::uint32_t locals a, c, ct and an mqc with bp ----

// The Tier-1 trace of the lane-per-code-block MQ prototype bench (bench/nj2k_mq_lanes.cpp, stage 4): a copy of the
// library built with NJ2K_T1_TRACE reports every MQ segment and decision. The library itself is built without it, and
// the hook compiles to nothing.
#if defined(NJ2K_T1_TRACE)
#define NJ2K_T1_TRACE_DECISION(ctx, d)     nj2k::detail::g_t1_trace.decision(nj2k::detail::g_t1_trace.user, static_cast<std::uint32_t>(ctx), (d))
#else
#define NJ2K_T1_TRACE_DECISION(ctx, d)
#endif

// BYTEIN (C.3.4): a byte into c, or the 0xFF 0xFF marker stuffed in as 1s; bp stops at a marker. The decoder's
// input has a 0xFF 0xFF after each segment (opj_mqc_init_dec_common), so bp[1] is always readable.
#define opj_mqc_bytein_macro(mqc, c, ct) \
{ \
    std::uint32_t l_c = *((mqc)->bp + 1); \
    if (*(mqc)->bp == 0xff) { \
        if (l_c > 0x8f) { \
            c += 0xff00; \
            ct = 8; \
        } else { \
            (mqc)->bp++; \
            c += l_c << 9; \
            ct = 7; \
        } \
    } else { \
        (mqc)->bp++; \
        c += l_c << 8; \
        ct = 8; \
    } \
}

// RENORMD for a that has lost bit 15 (1 to 15 shifts): the shifts counted at once (mq_shifts) and taken as many at
// a time as c has bits left, a byte read whenever none is left: the same byte reads at the same shifts as
// OpenJPEG's one-bit loop
#define opj_mqc_renormd_macro(mqc, a, c, ct) \
{ \
    std::uint32_t shifts_ = nj2k::detail::mq_shifts(a); \
    do { \
        if (ct == 0) { \
            opj_mqc_bytein_macro(mqc, c, ct); \
        } \
        const std::uint32_t k_ = shifts_ < ct ? shifts_ : ct; \
        a <<= k_; \
        c <<= k_; \
        ct -= k_; \
        shifts_ -= k_; \
    } while (shifts_ != 0); \
}

// RENORMD always run: a with bit 15 set shifts 0 and costs no branch; the shifts at once when c has the bits (almost
// always: a byte comes in 0.07-0.11 times a decision), else the loop above
#define opj_mqc_renorm_always_macro(mqc, a, c, ct) \
{ \
    const std::uint32_t s0_ = nj2k::detail::mq_shifts(a); \
    if (s0_ <= ct) { \
        a <<= s0_; \
        c <<= s0_; \
        ct -= s0_; \
    } else { \
        opj_mqc_renormd_macro(mqc, a, c, ct); \
    } \
}

// DECODE (C.3.2) on the slot curctx: d is the decoded bit. With qe the state's interval, lps_ whether the code
// register is in the LPS sub-interval, flip_ whether the symbol is the less probable one (an LPS-interval hit that
// the exchange leaves an MPS, or an MPS-interval hit that the exchange makes an LPS), the state changes (to the
// next-LPS state when flip_, else the next-MPS state) when the interval is the LPS one or a has lost bit 15.
#if defined(__GNUC__) && !defined(__clang__)
// GCC turns the selects of the form below back into branches, so the same operations as masks and shifts
#define opj_mqc_decode_macro(d, mqc, curctx, a, c, ct) \
{ \
    const std::uint32_t w_ = *(curctx); \
    const std::uint32_t qe_ = w_ & 0xFFFFU; \
    a -= qe_; \
    const std::uint32_t lps_ = (c >> 16) < qe_; \
    const std::uint32_t flip_ = lps_ ^ (std::uint32_t)(a < qe_); \
    const std::uint32_t lm_ = 0U - lps_; \
    d = ((w_ >> 16) & 1U) ^ flip_; \
    const std::uint32_t nw_ = nj2k::detail::kMqWords.w[(w_ >> (17U + 7U * flip_)) & 0x7FU]; \
    const std::uint32_t chg_ = 0U - ((lps_ | ((a >> 15) ^ 1U)) & 1U); \
    *(curctx) = w_ ^ ((w_ ^ nw_) & chg_); \
    c -= (qe_ << 16) & ~lm_; \
    a = (a & ~lm_) | (qe_ & lm_); \
    opj_mqc_renorm_always_macro(mqc, a, c, ct); \
    NJ2K_T1_TRACE_DECISION((curctx) - (mqc)->ctx.slot, d); \
}
#else
#define opj_mqc_decode_macro(d, mqc, curctx, a, c, ct) \
{ \
    const std::uint32_t w_ = *(curctx); \
    const std::uint32_t qe_ = w_ & 0xFFFFU; \
    a -= qe_; \
    const std::uint32_t lps_ = (c >> 16) < qe_; \
    const std::uint32_t flip_ = lps_ ^ (std::uint32_t)(a < qe_); \
    const std::uint32_t lm_ = 0U - lps_; \
    d = ((w_ >> 16) & 1U) ^ flip_; \
    const std::uint32_t nw_ = nj2k::detail::kMqWords.w[flip_ ? (w_ >> 24) : ((w_ >> 17) & 0x7FU)]; \
    *(curctx) = (lps_ | (std::uint32_t)(a < 0x8000U)) ? nw_ : w_; \
    c -= (qe_ << 16) & ~lm_; \
    a = (a & ~lm_) | (qe_ & lm_); \
    opj_mqc_renorm_always_macro(mqc, a, c, ct); \
    NJ2K_T1_TRACE_DECISION((curctx) - (mqc)->ctx.slot, d); \
}
#endif

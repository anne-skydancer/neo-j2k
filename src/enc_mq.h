// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The encoder's MQ coder (OpenJPEG 2.5.4's mqc.c and mqc_inl.h, encoding side; their notice is in
// enc_t1.cpp), a header so that the tests can drive it alone. The coder's output is bounded here,
// where OpenJPEG writes without a bound (OD-4).
#pragma once

#include <cstdint>

#include "enc_luts.h"
#include "t1.h"
#include "t1_tables.h"

namespace nj2k::detail {

// The coder's hot functions are inlined into every pass step: MSVC's own choice leaves the renormalisation and
// the decision as calls, with the coder's words and the context slot stored and reloaded around each.
#if defined(_MSC_VER) && !defined(__clang__)
#define NJ2K_MQ_INLINE __forceinline
#else
#define NJ2K_MQ_INLINE inline __attribute__((always_inline))
#endif

struct MqEnc {
    std::uint32_t c, a, ct, cx;
    std::uint8_t* bp;
    std::uint8_t* start;
    std::uint8_t* limit;  // the last byte that may be written
    bool overflow;
    std::uint8_t ctxs[19];  // a context's state: its index (state * 2 + mps) in kMqEncStates
    const std::uint8_t* lut_ctxno_zc_orient;

    void setcurctx(std::uint32_t ctxno) noexcept { cx = ctxno; }

    void resetstates() noexcept
    {
        for (std::uint32_t i = 0; i < 19U; i++) {
            ctxs[i] = 0;
        }
    }
    void setstate(std::uint32_t ctxno, std::uint32_t msb, std::int32_t prob) noexcept
    {
        ctxs[ctxno] = static_cast<std::uint8_t>(msb + static_cast<std::uint32_t>(prob << 1));
    }
    // the first byte before the buffer is a fake one the caller set to 0
    void init_enc(std::uint8_t* buf, std::uint8_t* last) noexcept
    {
        setcurctx(0);
        a = 0x8000;
        c = 0;
        bp = buf - 1;
        ct = 12;
        start = buf;
        limit = last;
        overflow = false;
    }
    std::uint32_t numbytes() const noexcept { return static_cast<std::uint32_t>(bp - start); }

    void byteout() noexcept
    {
        if (bp >= limit) {
            // OpenJPEG would write past the buffer here (OD-4); neo-j2k stops the code-block
            overflow = true;
            bp = limit - 1;
        }
        if (*bp == 0xff) {
            bp++;
            *bp = static_cast<std::uint8_t>(c >> 20);
            c &= 0xfffff;
            ct = 7;
        } else if ((c & 0x8000000) == 0) {
            bp++;
            *bp = static_cast<std::uint8_t>(c >> 19);
            c &= 0x7ffff;
            ct = 8;
        } else {
            (*bp)++;
            if (*bp == 0xff) {
                c &= 0x7ffffff;
                bp++;
                *bp = static_cast<std::uint8_t>(c >> 20);
                c &= 0xfffff;
                ct = 7;
            } else {
                bp++;
                *bp = static_cast<std::uint8_t>(c >> 19);
                c &= 0x7ffff;
                ct = 8;
            }
        }
    }

    NJ2K_MQ_INLINE void renorme() noexcept
    {
        // every shift at once (count-leading-zeros) instead of OpenJPEG's one bit a turn: as many as c has
        // bits left to the next byte-out, which comes where the one-bit loop has it (when ct reaches 0), so
        // c, ct and the bytes are the loop's at every step
        std::uint32_t k = mq_shifts(a);
        a <<= k;
        while (k >= ct) {
            c <<= ct;
            k -= ct;
            byteout();
        }
        c <<= k;
        ct -= k;
    }

    // The encoder knows its bit, so the choice of the interval, the conditional exchange and the state
    // update are selects, not branches. With a' = a - qe and x = (a' < qe), the MPS takes qe as its
    // interval when x and adds qe to c otherwise, the LPS the opposite; an MPS whose a' keeps bit 15
    // neither renormalises (renorme shifts by 0) nor changes state: the same function as OpenJPEG's
    // three branches (mqc.c opj_mqc_encode, codeMPS and codeLPS).
    NJ2K_MQ_INLINE void encode(std::uint32_t d) noexcept
    {
#if defined(__GNUC__) && !defined(__clang__)
// GCC turns these selects back into branches (measured: 0.95 of the old coder against 0.70 with masks)
        const std::uint32_t i = ctxs[cx];
        const MqEncState e = kMqEncStates.t[i];
        const std::uint32_t q = e.qe;
        const std::uint32_t lps = (i ^ d) & 1U;
        const std::uint32_t an = a - q;
        const std::uint32_t swap = static_cast<std::uint32_t>(an < q) ^ lps;
        const std::uint32_t mswap = 0U - swap;
        a = an ^ ((an ^ q) & mswap);
        c += q & ~mswap;
        const std::uint32_t mlps = 0U - lps;
        const std::uint32_t next = e.nmps ^ ((e.nmps ^ e.nlps) & mlps);
        const std::uint32_t mkeep = 0U - static_cast<std::uint32_t>((lps | (an & 0x8000U)) == 0x8000U);
        ctxs[cx] = static_cast<std::uint8_t>(next ^ ((next ^ i) & mkeep));
#else
        const std::uint32_t i = ctxs[cx];
        const MqEncState e = kMqEncStates.t[i];
        const std::uint32_t q = e.qe;
        const std::uint32_t lps = (i ^ d) & 1U;
        const std::uint32_t an = a - q;
        const std::uint32_t swap = static_cast<std::uint32_t>(an < q) ^ lps;
        a = swap ? q : an;
        c += swap ? 0U : q;
        const bool keep = (lps | (an & 0x8000U)) == 0x8000U;
        const std::uint32_t next = lps ? e.nlps : e.nmps;
        ctxs[cx] = static_cast<std::uint8_t>(keep ? i : next);
#endif
        renorme();
    }

    void setbits() noexcept
    {
        const std::uint32_t tempc = c + a;
        c |= 0xffff;
        if (c >= tempc) {
            c -= 0x8000;
        }
    }

    void flush() noexcept
    {
        setbits();
        c <<= ct;
        byteout();
        c <<= ct;
        byteout();
        // it is forbidden that a coding pass ends with 0xff
        if (*bp != 0xff) {
            bp++;
        }
    }
};

}  // namespace nj2k::detail

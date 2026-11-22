// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The encoder's MQ coder (OpenJPEG 2.5.4's mqc.c and mqc_inl.h, encoding side; their notice is in
// enc_t1.cpp), a header so that the tests can drive it alone. It gives OpenJPEG's output in a different
// formulation (renormalisation by count-leading-zeros, byte-index states, the decision by selects or
// masks); tests/unit/enc_t1_unit.cpp checks it against a frozen copy of the earlier operations. The
// coder's output is bounded here, where OpenJPEG writes without a bound (OD-4).
#pragma once

#include <cassert>
#include <cstdint>

#include "enc_luts.h"
#include "t1.h"
#include "t1_tables.h"

namespace nj2k::detail {

// The coder's hot functions and the pass steps are inlined whatever the compiler's own choice: MSVC leaves the
// renormalisation and the decision as calls, with the coder's words and the context slot stored and reloaded
// around each. (Used by enc_t1.cpp's pass steps too.)
#if defined(_MSC_VER) && !defined(__clang__)
#define NJ2K_FORCE_INLINE __forceinline
#else
#define NJ2K_FORCE_INLINE inline __attribute__((always_inline))
#endif

// The coder's registers and its output position, without the context states: a function that codes many decisions
// in a row (MqEnc::encode_pairs) works on a local copy, which the compiler can keep in registers (the context states
// are an array indexed by the decision, and the byte stores to it and to the output are, to the compiler, writes that
// may change the coder's words in memory).
struct MqCore {
    std::uint32_t c, a, ct;
    std::uint8_t* bp;
    std::uint8_t* limit;  // the last byte that may be written
    bool overflow;

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

    NJ2K_FORCE_INLINE void renorme() noexcept
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
    // update are selects (masks where GCC would turn selects into branches), not branches. With a' = a - qe
    // and x = (a' < qe), the MPS takes qe as its interval when x and adds qe to c otherwise, the LPS the
    // opposite; an MPS whose a' keeps bit 15 neither renormalises (renorme shifts by 0) nor changes state:
    // the same function as OpenJPEG's three branches (mqc.c opj_mqc_encode, codeMPS and codeLPS).
    // `slot` is the context's state byte. Precondition: d is 0 or 1.
    NJ2K_FORCE_INLINE void encode(std::uint8_t* slot, std::uint32_t d) noexcept
    {
        assert(d <= 1U);
        const std::uint32_t i = *slot;
        const MqEncState e = kMqEncStates.t[i];
        const std::uint32_t q = e.qe;
        const std::uint32_t lps = (i ^ d) & 1U;
        const std::uint32_t an = a - q;
        const std::uint32_t swap = static_cast<std::uint32_t>(an < q) ^ lps;
#if defined(__GNUC__) && !defined(__clang__)
        // GCC turns the selects below back into branches (measured: 0.95 of the old coder against 0.70)
        const std::uint32_t mswap = 0U - swap;
        a = an ^ ((an ^ q) & mswap);
        c += q & ~mswap;
        // the next state is the table entry's byte 2 (MPS) or 3 (LPS), loaded in place of a mask (on Linux CI
        // 3 to 4 % faster than the mask on GCC 14 and 15; enc_luts.h asserts the layout)
        const std::uint32_t next = reinterpret_cast<const std::uint8_t*>(&kMqEncStates.t[i])[2U + lps];
        // an MPS whose a' keeps bit 15 keeps its state (as a bit expression: GCC branches on `!lps && ...`)
        const std::uint32_t mkeep = 0U - (((an >> 15) & ~lps) & 1U);
        *slot = static_cast<std::uint8_t>(next ^ ((next ^ i) & mkeep));
#else
        a = swap ? q : an;
        c += swap ? 0U : q;
        const std::uint32_t next = lps ? e.nlps : e.nmps;
        // an MPS (lps == 0) whose a' keeps bit 15 keeps its state; written as one compare, since MSVC turns
        // `!lps && (an & 0x8000U)` into a branch
        const bool keep = (lps | (an & 0x8000U)) == 0x8000U;
        *slot = static_cast<std::uint8_t>(keep ? i : next);
#endif
        renorme();
    }
};

struct MqEnc : MqCore {
    std::uint32_t cx;
    std::uint8_t* start;
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

    NJ2K_FORCE_INLINE void encode(std::uint32_t d) noexcept { MqCore::encode(&ctxs[cx], d); }

    // Codes the n (context << 1 | bit) pairs of the list in order: the same decisions as setcurctx and encode a pair,
    // on a local copy of the coder's registers that is written back once (the copy stays in registers, where the
    // members are read and stored around the byte stores of each decision).
    void encode_pairs(const std::uint8_t* list, std::uint32_t n) noexcept
    {
        MqCore s = *this;
        std::uint8_t* const states = ctxs;
        for (std::uint32_t j = 0; j < n; ++j) {
            const std::uint32_t p = list[j];
            s.encode(states + (p >> 1), p & 1U);
        }
        static_cast<MqCore&>(*this) = s;
        if (n != 0U) {
            cx = list[n - 1U] >> 1;
        }
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

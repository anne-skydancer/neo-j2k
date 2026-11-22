// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The encoder's MQ coder (OpenJPEG 2.5.4's mqc.c and mqc_inl.h, encoding side; their notice is in
// enc_t1.cpp), a header so that the tests can drive it alone. The coder's output is bounded here,
// where OpenJPEG writes without a bound (OD-4).
#pragma once

#include <cstdint>

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
    const MqState* ctxs[19];
    const std::uint8_t* lut_ctxno_zc_orient;

    void setcurctx(std::uint32_t ctxno) noexcept { cx = ctxno; }

    void resetstates() noexcept
    {
        for (std::uint32_t i = 0; i < 19U; i++) {
            ctxs[i] = kMqcStates;
        }
    }
    void setstate(std::uint32_t ctxno, std::uint32_t msb, std::int32_t prob) noexcept
    {
        ctxs[ctxno] = &kMqcStates[msb + static_cast<std::uint32_t>(prob << 1)];
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

    NJ2K_MQ_INLINE void encode(std::uint32_t d) noexcept
    {
        const MqState* st = ctxs[cx];
        const std::uint32_t q = st->qeval;
        a -= q;
        if (st->mps == d) {
            if (a & 0x8000) {
                c += q;
                return;
            }
            if (a < q) {
                a = q;
            } else {
                c += q;
            }
            ctxs[cx] = st->nmps;
        } else {
            if (a < q) {
                c += q;
            } else {
                a = q;
            }
            ctxs[cx] = st->nlps;
        }
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

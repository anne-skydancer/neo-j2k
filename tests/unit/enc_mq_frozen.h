// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// FROZEN REFERENCE (test only): the encoder's MQ coder exactly as it was at bee41d4 (src/enc_t1.cpp,
// before the MQ redesign), OpenJPEG 2.5.4's mqc.c encoder with neo-j2k's output bound (OD-4). The text
// of the struct is that commit's; only its namespace, the include and OpenJPEG's integer names
// (OPJ_UINT32, OPJ_INT32: the std types they are aliases of) differ. Never edit it:
// the redesigned coder (src/enc_mq.h) is tested against it for byte identity.
#pragma once

#include <cstdint>

#include "../../src/t1.h"
#include "../../src/t1_tables.h"

namespace nj2k::frozen {

using nj2k::detail::kMqcStates;
using nj2k::detail::MqState;
constexpr std::uint32_t MQC_NUMCTXS = 19;

struct MqEnc {
    std::uint32_t c, a, ct;
    std::uint8_t* bp;
    std::uint8_t* start;
    std::uint8_t* limit;  // the last byte that may be written
    bool overflow;
    const MqState* ctxs[19];
    const MqState** curctx;
    const std::uint8_t* lut_ctxno_zc_orient;

    void setcurctx(std::uint32_t ctxno) noexcept { curctx = &ctxs[ctxno]; }

    void resetstates() noexcept
    {
        for (std::uint32_t i = 0; i < MQC_NUMCTXS; i++) {
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
        } else {
            if ((c & 0x8000000) == 0) {
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
    }

    void renorme() noexcept
    {
        do {
            a <<= 1;
            c <<= 1;
            ct--;
            if (ct == 0) {
                byteout();
            }
        } while ((a & 0x8000) == 0);
    }

    void encode(std::uint32_t d) noexcept
    {
        const MqState* st = *curctx;
        if (st->mps == d) {
            a -= st->qeval;
            if ((a & 0x8000) == 0) {
                if (a < st->qeval) {
                    a = st->qeval;
                } else {
                    c += st->qeval;
                }
                *curctx = st->nmps;
                renorme();
            } else {
                c += st->qeval;
            }
        } else {
            a -= st->qeval;
            if (a < st->qeval) {
                c += st->qeval;
            } else {
                a = st->qeval;
            }
            *curctx = st->nlps;
            renorme();
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

}  // namespace nj2k::frozen

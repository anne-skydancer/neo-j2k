// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// mq_exhaustive: one MQ decision of the stage-2 coder (clz renormalisation, packed index states, MPS/LPS by
// select: tools/fused_mq.inc with STUDY_CLZ, STUDY_IDX and STUDY_BRANCHLESS) against src/enc_t1.cpp's MqEnc
// (copied as M0 below), over every interval a in [0x8000, 0xffff], every state (94), both bits and every
// bit counter ct in [1, 12], with low words c and previous bytes chosen to reach the carry, the 0xFF
// stuffing and the output limit. Each pair must leave the same a, c, ct, state, bytes and overflow.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <intrin.h>

#include "t1.h"
#include "t1_tables.h"

using nj2k::detail::kMqcStates;
using nj2k::detail::MqState;
using u32 = std::uint32_t;
using u8 = std::uint8_t;

namespace {

// ---- src/enc_t1.cpp's MqEnc (one context) ----
struct Ref {
    u32 c, a, ct;
    u8* bp;
    u8* limit;
    bool overflow;
    const MqState* st;
    void byteout() noexcept
    {
        if (bp >= limit) {
            overflow = true;
            bp = limit - 1;
        }
        if (*bp == 0xff) {
            bp++;
            *bp = static_cast<u8>(c >> 20);
            c &= 0xfffff;
            ct = 7;
        } else {
            if ((c & 0x8000000) == 0) {
                bp++;
                *bp = static_cast<u8>(c >> 19);
                c &= 0x7ffff;
                ct = 8;
            } else {
                (*bp)++;
                if (*bp == 0xff) {
                    c &= 0x7ffffff;
                    bp++;
                    *bp = static_cast<u8>(c >> 20);
                    c &= 0xfffff;
                    ct = 7;
                } else {
                    bp++;
                    *bp = static_cast<u8>(c >> 19);
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
    void encode(u32 d) noexcept
    {
        if (st->mps == d) {
            a -= st->qeval;
            if ((a & 0x8000) == 0) {
                if (a < st->qeval) {
                    a = st->qeval;
                } else {
                    c += st->qeval;
                }
                st = st->nmps;
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
            st = st->nlps;
            renorme();
        }
    }
};

// ---- the stage-2 coder (tools/fused_mq.inc), one context ----
struct Packed {
    std::uint16_t qe;
    u8 nmps, nlps;
};
struct PackedTable {
    Packed t[94];
    constexpr PackedTable() noexcept : t{}
    {
        for (u32 i = 0; i < 94; ++i) {
            t[i].qe = static_cast<std::uint16_t>(kMqcStates[i].qeval);
            t[i].nmps = static_cast<u8>(kMqcStates[i].nmps - kMqcStates);
            t[i].nlps = static_cast<u8>(kMqcStates[i].nlps - kMqcStates);
        }
    }
};
constexpr PackedTable kPacked;

struct New {
    u32 c, a, ct;
    u8* bp;
    u8* limit;
    bool overflow;
    u8 idx;
    void byteout() noexcept
    {
        if (bp >= limit) {
            overflow = true;
            bp = limit - 1;
        }
        if (*bp == 0xff) {
            bp++;
            *bp = static_cast<u8>(c >> 20);
            c &= 0xfffff;
            ct = 7;
        } else if ((c & 0x8000000) == 0) {
            bp++;
            *bp = static_cast<u8>(c >> 19);
            c &= 0x7ffff;
            ct = 8;
        } else {
            (*bp)++;
            if (*bp == 0xff) {
                c &= 0x7ffffff;
                bp++;
                *bp = static_cast<u8>(c >> 20);
                c &= 0xfffff;
                ct = 7;
            } else {
                bp++;
                *bp = static_cast<u8>(c >> 19);
                c &= 0x7ffff;
                ct = 8;
            }
        }
    }
    void encode(u32 d) noexcept
    {
        const u32 i = idx;
        const Packed e = kPacked.t[i];
        const u32 q = e.qe;
        const u32 lps = (i ^ d) & 1U;
        const u32 an = a - q;
        const u32 swap = static_cast<u32>(an < q) ^ lps;
        a = swap ? q : an;
        c += swap ? 0U : q;
        const u32 keep = (lps | (an & 0x8000U)) == 0x8000U ? 1U : 0U;
        const u32 next = lps ? e.nlps : e.nmps;
        idx = static_cast<u8>(keep ? i : next);
        u32 k = _lzcnt_u32(a) - 16U;
        a <<= k;
        while (k >= ct) {
            c <<= ct;
            k -= ct;
            byteout();
        }
        c <<= k;
        ct -= k;
    }
};

}  // namespace

int main()
{
    // low words: zero, one below the carry bit (27), at the carry with stuffing-prone high bytes, a mixed one
    const u32 cs[] = {0x0000000U, 0x7FFFFFFU - 0x5601U, 0x7F80000U, 0x5A5A5A5U & 0x7FFFFFFU};
    const u8 prevs[] = {0x00, 0xFE, 0xFF};
    std::uint64_t cases = 0, bad = 0;
    for (u32 a = 0x8000; a <= 0xFFFF; ++a) {
        for (u32 i = 0; i < 94; ++i) {
            for (u32 d = 0; d < 2; ++d) {
                for (u32 ct = 1; ct <= 12; ++ct) {
                    for (u32 c : cs) {
                        for (u8 prev : prevs) {
                            for (int lim = 0; lim < 2; ++lim) {
                                // lim 1: the previous byte is the last one that may be written (OD-4's clamp)
                                u8 b1[8] = {}, b2[8] = {};
                                b1[1] = b2[1] = prev;
                                Ref r{c, a, ct, b1 + 1, lim ? b1 + 1 : b1 + 7, false, &kMqcStates[i]};
                                New n{c, a, ct, b2 + 1, lim ? b2 + 1 : b2 + 7, false, static_cast<u8>(i)};
                                r.encode(d);
                                n.encode(d);
                                ++cases;
                                if (r.a != n.a || r.c != n.c || r.ct != n.ct || (r.bp - b1) != (n.bp - b2) ||
                                    r.overflow != n.overflow || (r.st - kMqcStates) != n.idx ||
                                    std::memcmp(b1, b2, sizeof(b1)) != 0) {
                                    if (++bad <= 5) {
                                        std::printf("differs: a=%04x state=%u d=%u ct=%u c=%07x prev=%02x lim=%d\n", a,
                                                    i, d, ct, c, prev, lim);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    std::printf("%llu decisions compared, %llu differ\n", static_cast<unsigned long long>(cases),
                static_cast<unsigned long long>(bad));
    return bad ? 1 : 0;
}

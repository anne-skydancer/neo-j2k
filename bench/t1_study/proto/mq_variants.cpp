// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The MQ coder variants (mq_variants.h). M0 is src/enc_t1.cpp's MqEnc as it is; each other one changes one
// thing, and t1_replay checks every variant's bytes and raw rates against the library on every code-block.
#include "mq_variants.h"

#include <bit>
#include <cstring>

#include "t1.h"
#include "t1_flags.h"
#include "t1_tables.h"

namespace nj2k {

namespace {

using u32 = std::uint32_t;
using u8 = std::uint8_t;
using detail::kMqcStates;
using detail::MqState;

// ---- M0: the library's MqEnc, member for member ----
struct M0 {
    u32 c, a, ct;
    u8* bp;
    u8* start;
    u8* limit;
    bool overflow;
    const MqState* ctxs[19];
    const MqState** curctx;

    void setcurctx(u32 ctxno) noexcept { curctx = &ctxs[ctxno]; }
    void resetstates() noexcept
    {
        for (u32 i = 0; i < 19; i++) {
            ctxs[i] = kMqcStates;
        }
    }
    void setstate(u32 ctxno, u32 msb, std::int32_t prob) noexcept
    {
        ctxs[ctxno] = &kMqcStates[msb + static_cast<u32>(prob << 1)];
    }
    void init_enc(u8* buf, u8* last) noexcept
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
    u32 numbytes() const noexcept { return static_cast<u32>(bp - start); }
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
    // M1's renormalisation: every shift at once, the byte-outs where the one-bit loop had them
    void renorme_clz() noexcept
    {
        u32 n = static_cast<u32>(std::countl_zero(a)) - 16U;
        a <<= n;
        while (n >= ct) {
            c <<= ct;
            n -= ct;
            byteout();
        }
        c <<= n;
        ct -= n;
    }
    template <bool kClz>
    void encode(u32 d) noexcept
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
                kClz ? renorme_clz() : renorme();
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
            kClz ? renorme_clz() : renorme();
        }
    }
    void setbits() noexcept
    {
        const u32 tempc = c + a;
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
        if (*bp != 0xff) {
            bp++;
        }
    }
};

template <bool kClz>
void mq_member(const u8* syms, const u32* ends, u32 n, u8* buf, u32 cap, MqResult& r)
{
    M0 m;
    m.resetstates();
    m.setstate(T1_CTXNO_UNI, 0, 46);
    m.setstate(T1_CTXNO_AGG, 0, 3);
    m.setstate(T1_CTXNO_ZC, 0, 4);
    m.init_enc(buf, buf + cap - 1);
    const u8* s = syms;
    for (u32 p = 0; p < n; ++p) {
        const u8* const end = syms + ends[p];
        while (s < end) {
            const u32 x = *s++;
            m.setcurctx(x >> 1);
            m.template encode<kClz>(x & 1);
        }
        if (p + 1 == n) {
            m.flush();
            r.rates[p] = m.numbytes();
        } else {
            r.rates[p] = m.numbytes() + 3;
        }
    }
    r.nbytes = m.numbytes();
    r.overflow = m.overflow;
}

// ---- M2..M5: the coder's registers in locals for the whole block ----
// kReg: the last byte written held in a register (no reload of *bp in byteout)
// kIdx: a context's state as a byte index into a 4-byte table (qe, nmps, nlps; mps = index & 1)
struct Packed {
    std::uint16_t qe;
    u8 nmps, nlps;
};
struct PackedTable {
    Packed t[94];
    PackedTable() noexcept
    {
        for (u32 i = 0; i < 94; ++i) {
            t[i].qe = static_cast<std::uint16_t>(kMqcStates[i].qeval);
            t[i].nmps = static_cast<u8>(kMqcStates[i].nmps - kMqcStates);
            t[i].nlps = static_cast<u8>(kMqcStates[i].nlps - kMqcStates);
        }
    }
};
const PackedTable kPacked;

template <bool kReg, bool kIdx>
void mq_local(const u8* syms, const u32* ends, u32 n, u8* buf, u32 cap, MqResult& r)
{
    u32 a = 0x8000, c = 0, ct = 12;
    u8* bp = buf - 1;
    u8* const limit = buf + cap - 1;
    u32 last = buf[-1];  // kReg: *bp
    bool overflow = false;
    const MqState* ptr[19];
    u8 idx[19];
    for (u32 i = 0; i < 19; ++i) {
        ptr[i] = kMqcStates;
        idx[i] = 0;
    }
    ptr[T1_CTXNO_UNI] = &kMqcStates[46 << 1];
    ptr[T1_CTXNO_AGG] = &kMqcStates[3 << 1];
    ptr[T1_CTXNO_ZC] = &kMqcStates[4 << 1];
    idx[T1_CTXNO_UNI] = 46 << 1;
    idx[T1_CTXNO_AGG] = 3 << 1;
    idx[T1_CTXNO_ZC] = 4 << 1;

    auto byteout = [&]() noexcept {
        if (bp >= limit) {
            overflow = true;
            bp = limit - 1;
            last = *bp;
        }
        const u32 prev = kReg ? last : *bp;
        if (prev == 0xff) {
            bp++;
            last = (c >> 20) & 0xff;
            *bp = static_cast<u8>(last);
            c &= 0xfffff;
            ct = 7;
        } else if ((c & 0x8000000) == 0) {
            bp++;
            last = (c >> 19) & 0xff;
            *bp = static_cast<u8>(last);
            c &= 0x7ffff;
            ct = 8;
        } else {
            const u32 inc = prev + 1;
            *bp = static_cast<u8>(inc);
            if (inc == 0xff) {
                c &= 0x7ffffff;
                bp++;
                last = (c >> 20) & 0xff;
                *bp = static_cast<u8>(last);
                c &= 0xfffff;
                ct = 7;
            } else {
                bp++;
                last = (c >> 19) & 0xff;
                *bp = static_cast<u8>(last);
                c &= 0x7ffff;
                ct = 8;
            }
        }
    };

    const u8* s = syms;
    for (u32 p = 0; p < n; ++p) {
        const u8* const end = syms + ends[p];
        while (s < end) {
            const u32 x = *s++;
            const u32 cx = x >> 1, d = x & 1;
            u32 q, mps;
            if (kIdx) {
                const u32 i = idx[cx];
                q = kPacked.t[i].qe;
                mps = i & 1;
                a -= q;
                if (mps == d) {
                    if (a & 0x8000) {
                        c += q;
                        continue;
                    }
                    if (a < q) {
                        a = q;
                    } else {
                        c += q;
                    }
                    idx[cx] = kPacked.t[i].nmps;
                } else {
                    if (a < q) {
                        c += q;
                    } else {
                        a = q;
                    }
                    idx[cx] = kPacked.t[i].nlps;
                }
            } else {
                const MqState* st = ptr[cx];
                q = st->qeval;
                mps = st->mps;
                a -= q;
                if (mps == d) {
                    if (a & 0x8000) {
                        c += q;
                        continue;
                    }
                    if (a < q) {
                        a = q;
                    } else {
                        c += q;
                    }
                    ptr[cx] = st->nmps;
                } else {
                    if (a < q) {
                        c += q;
                    } else {
                        a = q;
                    }
                    ptr[cx] = st->nlps;
                }
            }
            u32 k = static_cast<u32>(std::countl_zero(a)) - 16U;
            a <<= k;
            while (k >= ct) {
                c <<= ct;
                k -= ct;
                byteout();
            }
            c <<= k;
            ct -= k;
        }
        if (p + 1 == n) {
            const u32 tempc = c + a;
            c |= 0xffff;
            if (c >= tempc) {
                c -= 0x8000;
            }
            c <<= ct;
            byteout();
            c <<= ct;
            byteout();
            if ((kReg ? last : *bp) != 0xff) {
                bp++;
            }
            r.rates[p] = static_cast<u32>(bp - buf);
        } else {
            r.rates[p] = static_cast<u32>(bp - buf) + 3;
        }
    }
    r.nbytes = static_cast<u32>(bp - buf);
    r.overflow = overflow;
}

// ---- M6, M7: the encoder knows its bit, so the MPS/LPS choice is a select, not a branch ----
// With a' = a - qe and x = (a' < qe): the MPS path takes qe as the interval when x (the conditional
// exchange) and otherwise adds qe to c; the LPS path does the opposite. So with lps = (d != mps):
//   swap = x != lps;  a = swap ? qe : a';  c += swap ? 0 : qe;  state = lps ? nlps : nmps
// and the renormalisation shifts by clz (0 when a' kept bit 15: the MPS without renormalisation).
// The state advances on every MPS too, which the library only does with a renormalisation: an MPS
// without one must keep its state, so the MPS-without-renorm case selects the old state.
template <bool kIdx>
void mq_branchless(const u8* syms, const u32* ends, u32 n, u8* buf, u32 cap, MqResult& r)
{
    u32 a = 0x8000, c = 0, ct = 12;
    u8* bp = buf - 1;
    u8* const limit = buf + cap - 1;
    bool overflow = false;
    const MqState* ptr[19];
    u8 idx[19];
    for (u32 i = 0; i < 19; ++i) {
        ptr[i] = kMqcStates;
        idx[i] = 0;
    }
    ptr[T1_CTXNO_UNI] = &kMqcStates[46 << 1];
    ptr[T1_CTXNO_AGG] = &kMqcStates[3 << 1];
    ptr[T1_CTXNO_ZC] = &kMqcStates[4 << 1];
    idx[T1_CTXNO_UNI] = 46 << 1;
    idx[T1_CTXNO_AGG] = 3 << 1;
    idx[T1_CTXNO_ZC] = 4 << 1;

    auto byteout = [&]() noexcept {
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
    };

    const u8* s = syms;
    for (u32 p = 0; p < n; ++p) {
        const u8* const end = syms + ends[p];
        while (s < end) {
            const u32 x = *s++;
            const u32 cx = x >> 1, d = x & 1;
            u32 q, lps;
            if (kIdx) {
                const u32 i = idx[cx];
                const Packed e = kPacked.t[i];
                q = e.qe;
                lps = (i ^ d) & 1;
                const u32 an = a - q;
                const u32 swap = static_cast<u32>(an < q) ^ lps;
                a = swap ? q : an;
                c += swap ? 0 : q;
                const u32 keep = (lps | (an & 0x8000)) == 0x8000 ? 1U : 0U;  // an MPS that needs no renorm
                const u32 next = lps ? e.nlps : e.nmps;
                idx[cx] = static_cast<u8>(keep ? i : next);
            } else {
                const MqState* st = ptr[cx];
                q = st->qeval;
                lps = st->mps ^ d;
                const u32 an = a - q;
                const u32 swap = static_cast<u32>(an < q) ^ lps;
                a = swap ? q : an;
                c += swap ? 0 : q;
                const bool keep = !lps && (an & 0x8000);
                const MqState* next = lps ? st->nlps : st->nmps;
                ptr[cx] = keep ? st : next;
            }
            u32 k = static_cast<u32>(std::countl_zero(a)) - 16U;
            a <<= k;
            while (k >= ct) {
                c <<= ct;
                k -= ct;
                byteout();
            }
            c <<= k;
            ct -= k;
        }
        if (p + 1 == n) {
            const u32 tempc = c + a;
            c |= 0xffff;
            if (c >= tempc) {
                c -= 0x8000;
            }
            c <<= ct;
            byteout();
            c <<= ct;
            byteout();
            if (*bp != 0xff) {
                bp++;
            }
            r.rates[p] = static_cast<u32>(bp - buf);
        } else {
            r.rates[p] = static_cast<u32>(bp - buf) + 3;
        }
    }
    r.nbytes = static_cast<u32>(bp - buf);
    r.overflow = overflow;
}

}  // namespace

const MqVariant kMqVariants[] = {
    {"M0 library MqEnc", mq_member<false>},
    {"M1 +clz renorm", mq_member<true>},
    {"M2 locals+clz", mq_local<false, false>},
    {"M3 M2+last byte reg", mq_local<true, false>},
    {"M4 M2+packed idx", mq_local<false, true>},
    {"M5 M3+packed idx", mq_local<true, true>},
    {"M6 branchless ptr", mq_branchless<false>},
    {"M7 branchless idx", mq_branchless<true>},
};
const int kMqVariantCount = static_cast<int>(sizeof(kMqVariants) / sizeof(kMqVariants[0]));

}  // namespace nj2k

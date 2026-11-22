// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The MQ decoder alone: the recorded decisions (contexts as recorded) decoded again from the recorded
// segments' bytes, each decision checked against the recorded symbol. No context modelling.
//   M0 the library's decoder (pointer states, mqc_inl.h with stage 4's renormalisation)
//   M1 packed byte states, today's branches;  M2 packed, selects;  M3 state words, selects
#include "common.h"

namespace mqs {

namespace alone_detail {
using OPJ_BYTE = std::uint8_t;
using OPJ_INT32 = std::int32_t;
using OPJ_UINT32 = std::uint32_t;
constexpr OPJ_UINT32 OPJ_COMMON_CBLK_DATA_EXTRA = 2;
constexpr OPJ_UINT32 MQC_NUMCTXS = 19;
}  // namespace alone_detail

namespace alone_byte {
using namespace alone_detail;
using opj_mqc_t = MqcV<std::uint8_t>;
#define STUDY_SLOT_T std::uint8_t
#define STUDY_SLOT_OF(i) static_cast<std::uint8_t>(i)
#include "mq_decide.inc"
#include "mq_packed_common.inc"
#undef STUDY_SLOT_T
#undef STUDY_SLOT_OF
#undef opj_mqc_setcurctx
#undef opj_t1_setcurctx
#undef DOWNLOAD_MQC_VARIABLES
}  // namespace alone_byte

namespace alone_word {
using namespace alone_detail;
using opj_mqc_t = MqcV<std::uint32_t>;
#define STUDY_SLOT_T std::uint32_t
#define STUDY_SLOT_OF(i) kWords.w[(i)]
#define DOWNLOAD_MQC_VARIABLES(mqc, curctx, a, c, ct) \
        STUDY_SLOT_T *curctx = mqc->curctx; \
        OPJ_UINT32 c = mqc->c; \
        OPJ_UINT32 a = mqc->a; \
        OPJ_UINT32 ct = mqc->ct
// the functions again, for the word slots (the macros are shared)
inline OPJ_UINT32 mq_shifts(OPJ_UINT32 a) noexcept { return _lzcnt_u32(a) - 16U; }
static void opj_mqc_init_dec(opj_mqc_t* mqc, OPJ_BYTE* bp, OPJ_UINT32 len)
{
    mqc->start = bp;
    mqc->end = bp + len;
    mqc->bp = bp;
    mqc->curctx = &mqc->st[0];
    mqc->end_of_byte_stream_counter = 0;
    mqc->c = len == 0 ? 0xffU << 16 : (OPJ_UINT32)(*mqc->bp << 16);
    opj_mqc_bytein_macro(mqc, mqc->c, mqc->ct);
    mqc->c <<= 7;
    mqc->ct -= 7;
    mqc->a = 0x8000;
}
}  // namespace alone_word

namespace {

template <class Fn>
std::size_t replay_byte(Recording& r, Fn&& decide) noexcept
{
    using namespace alone_byte;
    std::size_t bad = 0;
    for (const RecSeg& s : r.segs) {
        opj_mqc_t m{};
        std::memcpy(m.st, s.states, 19);
        opj_mqc_init_dec(&m, r.bytes.data() + s.bytes, s.len, 2);
        opj_mqc_t* mqc = &m;
        std::uint8_t* curctx = mqc->curctx;
        std::uint32_t c = mqc->c, a = mqc->a, ct = mqc->ct;
        const std::uint8_t* cx = r.ctx.data() + s.first;
        const std::uint8_t* sym = r.sym.data() + s.first;
        for (std::size_t i = 0; i < s.count; ++i) {
            curctx = &mqc->st[cx[i]];
            std::uint32_t d;
            decide(d, mqc, curctx, a, c, ct);
            bad += d != sym[i];
        }
        (void)curctx;
    }
    return bad;
}

}  // namespace

std::size_t alone_m1(Recording& r) noexcept
{
    using namespace alone_byte;
    return replay_byte(r, [](std::uint32_t& d, opj_mqc_t* mqc, std::uint8_t* curctx, std::uint32_t& a, std::uint32_t& c,
                             std::uint32_t& ct) { mq_decide_branchy(d, mqc, curctx, a, c, ct); });
}

std::size_t alone_m2(Recording& r) noexcept
{
    using namespace alone_byte;
    return replay_byte(r, [](std::uint32_t& d, opj_mqc_t* mqc, std::uint8_t* curctx, std::uint32_t& a, std::uint32_t& c,
                             std::uint32_t& ct) { mq_decide_select(d, mqc, curctx, a, c, ct); });
}

std::size_t alone_m3(Recording& r) noexcept
{
    using namespace alone_word;
    std::size_t bad = 0;
    for (const RecSeg& s : r.segs) {
        opj_mqc_t m{};
        for (int k = 0; k < 19; ++k) {
            m.st[k] = kWords.w[s.states[k]];
        }
        opj_mqc_init_dec(&m, r.bytes.data() + s.bytes, s.len);
        opj_mqc_t* mqc = &m;
        std::uint32_t* curctx = mqc->curctx;
        std::uint32_t c = mqc->c, a = mqc->a, ct = mqc->ct;
        const std::uint8_t* cx = r.ctx.data() + s.first;
        const std::uint8_t* sym = r.sym.data() + s.first;
        for (std::size_t i = 0; i < s.count; ++i) {
            curctx = &mqc->st[cx[i]];
            std::uint32_t d;
            mq_decide_word_select(d, mqc, curctx, a, c, ct);
            bad += d != sym[i];
        }
    }
    return bad;
}

// M0: the library's operations on pointer states
std::size_t alone_m0(Recording& r) noexcept
{
    using nj2k::detail::MqState;
    using nj2k::detail::kMqcStates;
    std::size_t bad = 0;
    for (const RecSeg& s : r.segs) {
        const MqState* st[19];
        for (int k = 0; k < 19; ++k) {
            st[k] = &kMqcStates[s.states[k]];
        }
        std::uint8_t* bp = r.bytes.data() + s.bytes;
        std::uint32_t c = s.len == 0 ? 0xffU << 16 : std::uint32_t(*bp) << 16, a, ct = 0;
        auto bytein = [&]() {
            const std::uint32_t l_c = bp[1];
            if (*bp == 0xff) {
                if (l_c > 0x8f) {
                    c += 0xff00;
                    ct = 8;
                } else {
                    bp++;
                    c += l_c << 9;
                    ct = 7;
                }
            } else {
                bp++;
                c += l_c << 8;
                ct = 8;
            }
        };
        auto renorm = [&]() {
            std::uint32_t shifts = _lzcnt_u32(a) - 16U;
            do {
                if (ct == 0) {
                    bytein();
                }
                const std::uint32_t k = shifts < ct ? shifts : ct;
                a <<= k;
                c <<= k;
                ct -= k;
                shifts -= k;
            } while (shifts != 0);
        };
        bytein();
        c <<= 7;
        ct -= 7;
        a = 0x8000;
        const std::uint8_t* cx = r.ctx.data() + s.first;
        const std::uint8_t* sym = r.sym.data() + s.first;
        for (std::size_t i = 0; i < s.count; ++i) {
            const MqState** curctx = &st[cx[i]];
            std::uint32_t d;
            a -= (*curctx)->qeval;
            if ((c >> 16) < (*curctx)->qeval) {
                if (a < (*curctx)->qeval) {
                    a = (*curctx)->qeval;
                    d = (*curctx)->mps;
                    *curctx = (*curctx)->nmps;
                } else {
                    a = (*curctx)->qeval;
                    d = !((*curctx)->mps);
                    *curctx = (*curctx)->nlps;
                }
                renorm();
            } else {
                c -= (*curctx)->qeval << 16;
                if ((a & 0x8000) == 0) {
                    if (a < (*curctx)->qeval) {
                        d = !((*curctx)->mps);
                        *curctx = (*curctx)->nlps;
                    } else {
                        d = (*curctx)->mps;
                        *curctx = (*curctx)->nmps;
                    }
                    renorm();
                } else {
                    d = (*curctx)->mps;
                }
            }
            bad += d != sym[i];
        }
    }
    return bad;
}

}  // namespace mqs

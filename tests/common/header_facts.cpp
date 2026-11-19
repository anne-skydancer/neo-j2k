// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
#include <cstdio>
#include <cstring>

#include "common.h"
#include "decoder.h"

namespace nj2k::test {

bool is_jp2_file(const std::uint8_t* data, std::size_t size) noexcept
{
    static const std::uint8_t sig[12] = {0x00, 0x00, 0x00, 0x0C, 0x6A, 0x50, 0x20, 0x20, 0x0D, 0x0A, 0x87, 0x0A};
    return data && size >= sizeof(sig) && std::memcmp(data, sig, sizeof(sig)) == 0;
}

std::uint64_t HeaderFacts::hash() const
{
    Fnv64 h;
    h.u32(ok ? 1 : 0);
    if (!ok) {
        return h.value();
    }
    for (std::uint32_t v : {x0, y0, x1, y1, numcomps, std::uint32_t(color_space), std::uint32_t(icc_buffer),
                            std::uint32_t(icc.size()), tx0, ty0, tdx, tdy, tw, th, nbcomps, csty, std::uint32_t(prg),
                            numlayers, mct}) {
        h.u32(v);
    }
    h.bytes(icc.data(), icc.size());
    for (const Comp& c : comps) {
        for (std::uint32_t v : {c.dx, c.dy, c.w, c.h, c.x0, c.y0, c.prec, c.sgnd, c.factor, c.resno_decoded, c.alpha}) {
            h.u32(v);
        }
    }
    for (const Tccp& t : tccp) {
        for (std::uint32_t v : {t.csty, t.numresolutions, t.cblkw, t.cblkh, t.cblksty, t.qmfbid, t.qntsty, t.numgbits,
                                std::uint32_t(t.roishift), std::uint32_t(t.mant.size())}) {
            h.u32(v);
        }
        for (std::size_t b = 0; b < t.mant.size(); ++b) {
            h.u32(t.mant[b]);
            h.u32(t.expn[b]);
        }
        h.bytes(t.prc.data(), t.prc.size());
    }
    return h.value();
}

std::string header_difference(const HeaderFacts& a, const HeaderFacts& b)
{
    char text[256];
    if (a.ok != b.ok) {
        std::snprintf(text, sizeof(text), "outcome: %s / %s", a.ok ? "read" : ("refused at " + a.stage).c_str(),
                      b.ok ? "read" : ("refused at " + b.stage).c_str());
        return text;
    }
    if (!a.ok) {
        return {};
    }
#define NJ2K_FACT(field)                                                                                \
    if (a.field != b.field) {                                                                           \
        std::snprintf(text, sizeof(text), #field ": %lld / %lld", (long long)a.field, (long long)b.field); \
        return text;                                                                                    \
    }
    NJ2K_FACT(x0) NJ2K_FACT(y0) NJ2K_FACT(x1) NJ2K_FACT(y1) NJ2K_FACT(numcomps) NJ2K_FACT(color_space)
    NJ2K_FACT(icc_buffer) NJ2K_FACT(tx0) NJ2K_FACT(ty0) NJ2K_FACT(tdx) NJ2K_FACT(tdy) NJ2K_FACT(tw) NJ2K_FACT(th)
    NJ2K_FACT(nbcomps) NJ2K_FACT(csty) NJ2K_FACT(prg) NJ2K_FACT(numlayers) NJ2K_FACT(mct)
#undef NJ2K_FACT
    if (a.icc != b.icc) {
        return "icc profile bytes";
    }
    if (a.comps.size() != b.comps.size() || a.tccp.size() != b.tccp.size()) {
        return "component count";
    }
    for (std::size_t c = 0; c < a.comps.size(); ++c) {
        const auto &p = a.comps[c], &q = b.comps[c];
#define NJ2K_COMP(field)                                                                                     \
    if (p.field != q.field) {                                                                                \
        std::snprintf(text, sizeof(text), "comps[%zu]." #field ": %u / %u", c, unsigned(p.field), unsigned(q.field)); \
        return text;                                                                                         \
    }
        NJ2K_COMP(dx) NJ2K_COMP(dy) NJ2K_COMP(w) NJ2K_COMP(h) NJ2K_COMP(x0) NJ2K_COMP(y0) NJ2K_COMP(prec)
        NJ2K_COMP(sgnd) NJ2K_COMP(factor) NJ2K_COMP(resno_decoded) NJ2K_COMP(alpha)
#undef NJ2K_COMP
    }
    for (std::size_t c = 0; c < a.tccp.size(); ++c) {
        const auto &p = a.tccp[c], &q = b.tccp[c];
#define NJ2K_TCCP(field)                                                                                     \
    if (p.field != q.field) {                                                                                \
        std::snprintf(text, sizeof(text), "tccp[%zu]." #field ": %lld / %lld", c, (long long)p.field, (long long)q.field); \
        return text;                                                                                         \
    }
        NJ2K_TCCP(csty) NJ2K_TCCP(numresolutions) NJ2K_TCCP(cblkw) NJ2K_TCCP(cblkh) NJ2K_TCCP(cblksty) NJ2K_TCCP(qmfbid)
        NJ2K_TCCP(qntsty) NJ2K_TCCP(numgbits) NJ2K_TCCP(roishift)
#undef NJ2K_TCCP
        if (p.mant != q.mant || p.expn != q.expn) {
            std::snprintf(text, sizeof(text), "tccp[%zu] step sizes", c);
            return text;
        }
        if (p.prc != q.prc) {
            std::snprintf(text, sizeof(text), "tccp[%zu] precinct sizes", c);
            return text;
        }
    }
    return {};
}

}  // namespace nj2k::test

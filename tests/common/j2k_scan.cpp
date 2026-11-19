// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
#include "j2k_scan.h"

#include <algorithm>

namespace nj2k::test {

namespace {

constexpr std::uint16_t SOC = 0xFF4F, SIZ = 0xFF51, COD = 0xFF52, COC = 0xFF53, TLM = 0xFF55, PLM = 0xFF57,
                        PLT = 0xFF58, QCC = 0xFF5D, POC = 0xFF5F, PPM = 0xFF60, PPT = 0xFF61, COM = 0xFF64,
                        SOT = 0xFF90, SOD = 0xFF93, EOC = 0xFFD9;

struct Reader {
    std::span<const std::uint8_t> b;
    bool has(std::size_t pos, std::size_t n) const { return pos <= b.size() && n <= b.size() - pos; }
    std::uint16_t u16(std::size_t p) const { return std::uint16_t(b[p] << 8 | b[p + 1]); }
    std::uint32_t u32(std::size_t p) const
    {
        return std::uint32_t(b[p]) << 24 | std::uint32_t(b[p + 1]) << 16 | std::uint32_t(b[p + 2]) << 8 | b[p + 3];
    }
};

bool fail(std::string* why, const char* text)
{
    if (why) {
        *why = text;
    }
    return false;
}

std::uint64_t ceil_div(std::uint64_t a, std::uint64_t b)
{
    return b ? (a + b - 1) / b : 0;
}

}  // namespace

std::uint64_t MainHeader::tiles_x() const noexcept
{
    return xtsiz && xsiz > xtosiz ? ceil_div(std::uint64_t(xsiz) - xtosiz, xtsiz) : 0;
}

std::uint64_t MainHeader::tiles_y() const noexcept
{
    return ytsiz && ysiz > ytosiz ? ceil_div(std::uint64_t(ysiz) - ytosiz, ytsiz) : 0;
}

std::optional<MainHeader> parse_main_header(std::span<const std::uint8_t> bytes, std::string* why)
{
    const Reader r{bytes};
    MainHeader mh;
    if (!r.has(0, 2) || r.u16(0) != SOC) {
        fail(why, "no SOC marker");
        return std::nullopt;
    }
    bool siz = false, cod = false;
    std::size_t pos = 2;
    int coc_min = 0;
    while (true) {
        if (!r.has(pos, 2)) {
            fail(why, "main header cut");
            return std::nullopt;
        }
        const std::uint16_t marker = r.u16(pos);
        if (marker == SOT) {
            mh.end = pos;
            break;
        }
        if (!r.has(pos + 2, 2)) {
            fail(why, "main header cut");
            return std::nullopt;
        }
        const std::uint16_t len = r.u16(pos + 2);
        if (len < 2 || !r.has(pos + 2, len)) {
            fail(why, "marker segment past the end");
            return std::nullopt;
        }
        const std::size_t body = pos + 4, blen = len - 2u;
        switch (marker) {
        case SIZ:
            if (blen < 38) {
                fail(why, "short SIZ");
                return std::nullopt;
            }
            mh.rsiz = r.u16(body);
            mh.xsiz = r.u32(body + 2);
            mh.ysiz = r.u32(body + 6);
            mh.xosiz = r.u32(body + 10);
            mh.yosiz = r.u32(body + 14);
            mh.xtsiz = r.u32(body + 18);
            mh.ytsiz = r.u32(body + 22);
            mh.xtosiz = r.u32(body + 26);
            mh.ytosiz = r.u32(body + 30);
            mh.csiz = r.u16(body + 34);
            if (mh.csiz == 0 || blen != 36u + 3u * mh.csiz || mh.xsiz <= mh.xosiz || mh.ysiz <= mh.yosiz ||
                    mh.xtsiz == 0 || mh.ytsiz == 0) {
                fail(why, "bad SIZ");
                return std::nullopt;
            }
            for (std::uint16_t c = 0; c < mh.csiz; ++c) {
                mh.ssiz.push_back(bytes[body + 36 + 3u * c]);
                mh.xrsiz.push_back(bytes[body + 37 + 3u * c]);
                mh.yrsiz.push_back(bytes[body + 38 + 3u * c]);
            }
            siz = true;
            break;
        case COD:
            if (blen < 10) {
                fail(why, "short COD");
                return std::nullopt;
            }
            mh.scod = bytes[body];
            mh.progression = bytes[body + 1];
            mh.layers = r.u16(body + 2);
            mh.mct = bytes[body + 4];
            mh.levels = bytes[body + 5];
            mh.cblk_w_exp = bytes[body + 6];
            mh.cblk_h_exp = bytes[body + 7];
            mh.cblk_style = bytes[body + 8];
            mh.transform = bytes[body + 9];
            mh.precincts.clear();
            if (mh.scod & 1) {
                for (std::size_t i = 10; i < blen; ++i) {
                    mh.precincts.push_back(bytes[body + i]);
                }
            }
            cod = true;
            break;
        case COC: {
            mh.has_coc = true;
            const std::size_t at = mh.csiz >= 257 ? 3 : 2;  // Ccoc takes two bytes past 256 components
            if (blen > at) {
                const int res = bytes[body + at] + 1;
                coc_min = coc_min ? std::min(coc_min, res) : res;
            }
            break;
        }
        case QCC: mh.has_qcc = true; break;
        case POC: mh.has_poc = true; break;
        case PPM: mh.has_ppm = true; break;
        case TLM: mh.has_tlm = true; break;
        case PLM: mh.has_plm = true; break;
        case COM: mh.has_com = true; break;
        default: break;
        }
        pos += 2u + len;
    }
    if (!siz || !cod) {
        fail(why, "no SIZ or COD before the first SOT");
        return std::nullopt;
    }
    mh.min_resolutions = mh.levels + 1;
    if (coc_min) {
        mh.min_resolutions = std::min(mh.min_resolutions, coc_min);
    }
    return mh;
}

std::vector<TilePart> scan_tile_parts(std::span<const std::uint8_t> bytes, const MainHeader& mh)
{
    const Reader r{bytes};
    std::vector<TilePart> out;
    std::size_t pos = mh.end;
    while (r.has(pos, 12) && r.u16(pos) == SOT && r.u16(pos + 2) == 10) {
        TilePart tp;
        tp.sot = pos;
        tp.tile = r.u16(pos + 4);
        tp.psot = r.u32(pos + 6);
        tp.tpsot = bytes[pos + 10];
        tp.tnsot = bytes[pos + 11];
        std::size_t p = pos + 12;
        bool sod = false;
        while (r.has(p, 2)) {
            const std::uint16_t m = r.u16(p);
            if (m == SOD) {
                tp.data = p + 2;
                sod = true;
                break;
            }
            if (!r.has(p + 2, 2)) {
                break;
            }
            const std::uint16_t len = r.u16(p + 2);
            if (len < 2 || !r.has(p + 2, len)) {
                break;
            }
            if (m == PPT) {
                tp.has_ppt = true;
            } else if (m == PLT && len >= 3) {
                tp.has_plt = true;
                // Zplt, then lengths: 7 bits a byte, the high bit set on every byte but the last
                std::uint32_t v = 0;
                for (std::size_t i = p + 5; i < p + 2u + len; ++i) {
                    v = (v << 7) | (bytes[i] & 0x7Fu);
                    if (!(bytes[i] & 0x80u)) {
                        tp.plt.push_back(v);
                        v = 0;
                    }
                }
            }
            p += 2u + len;
        }
        if (!sod) {
            break;
        }
        if (tp.psot) {
            tp.end = std::min<std::size_t>(tp.sot + tp.psot, bytes.size());
        } else {
            tp.end = bytes.size();
            if (tp.end >= 2 && r.u16(tp.end - 2) == EOC) {
                tp.end -= 2;
            }
        }
        out.push_back(tp);
        if (!tp.psot || tp.sot + tp.psot >= bytes.size()) {
            break;
        }
        pos = tp.sot + tp.psot;
    }
    return out;
}

std::optional<std::vector<std::size_t>> packet_starts(std::span<const std::uint8_t> bytes, const MainHeader& mh,
                                                      const TilePart& tp)
{
    std::vector<std::size_t> starts;
    if (mh.scod & 2) {
        // SOP: FF91 0004 Nsop. Coded data never holds FF followed by a byte above 0x8F.
        for (std::size_t p = tp.data; p + 6 <= tp.end; ++p) {
            if (bytes[p] == 0xFF && bytes[p + 1] == 0x91 && bytes[p + 2] == 0 && bytes[p + 3] == 4) {
                starts.push_back(p);
                p += 5;
            }
        }
        return starts;
    }
    if (tp.has_plt && !mh.has_ppm && !tp.has_ppt) {
        std::size_t p = tp.data;
        for (std::uint32_t len : tp.plt) {
            starts.push_back(p);
            p += len;
        }
        if (p != tp.end) {
            return std::nullopt;  // the lengths do not cover the tile-part
        }
        return starts;
    }
    return std::nullopt;
}

std::optional<std::vector<std::size_t>> layer_boundaries(std::span<const std::uint8_t> bytes)
{
    const auto mh = parse_main_header(bytes);
    if (!mh || mh->progression != 0 /* LRCP */ || mh->layers < 2 || mh->has_poc ||
            mh->tiles_x() * mh->tiles_y() != 1) {
        return std::nullopt;
    }
    const auto tps = scan_tile_parts(bytes, *mh);
    if (tps.size() != 1) {
        return std::nullopt;
    }
    const auto starts = packet_starts(bytes, *mh, tps[0]);
    if (!starts || starts->empty() || starts->size() % mh->layers != 0) {
        return std::nullopt;
    }
    const std::size_t per_layer = starts->size() / mh->layers;
    std::vector<std::size_t> out;
    for (std::size_t k = 1; k < mh->layers; ++k) {
        out.push_back((*starts)[k * per_layer]);
    }
    return out;
}

bool within_limits(const MainHeader& mh, int reduce, const Limits& limits, std::string* why)
{
    if (reduce < 0 || reduce > 31) {
        return fail(why, "reduce out of range");
    }
    const std::uint64_t tiles = mh.tiles_x() * mh.tiles_y();
    if (tiles == 0 || tiles > limits.max_tiles) {
        return fail(why, "too many tiles");
    }
    if (mh.csiz > limits.max_components) {
        return fail(why, "too many components");
    }
    if (mh.layers == 0 || mh.layers > limits.max_layers) {
        return fail(why, "too many layers");
    }
    const std::uint64_t w = ceil_div(mh.width(), 1ull << reduce), h = ceil_div(mh.height(), 1ull << reduce);
    if (w > limits.max_side || h > limits.max_side) {
        return fail(why, "too large at the decoded level");
    }
    if (w * h * mh.csiz > limits.max_samples) {
        return fail(why, "too many samples at the decoded level");
    }
    // packets of the main COD: layers x components x tiles x precincts, summed over resolutions
    double precincts = 0;  // double: an estimate that must not wrap on hostile values
    const std::uint64_t tw = std::min<std::uint64_t>(mh.xtsiz, mh.width());
    const std::uint64_t th = std::min<std::uint64_t>(mh.ytsiz, mh.height());
    for (int res = 0; res <= mh.levels; ++res) {
        const int shift = mh.levels - res;
        const std::uint64_t rw = ceil_div(tw, 1ull << std::min(shift, 62));
        const std::uint64_t rh = ceil_div(th, 1ull << std::min(shift, 62));
        int ppx = 15, ppy = 15;
        if ((mh.scod & 1) && std::size_t(res) < mh.precincts.size()) {
            ppx = mh.precincts[res] & 15;
            ppy = mh.precincts[res] >> 4;
        }
        precincts += double(ceil_div(rw, 1ull << ppx) + 1) * double(ceil_div(rh, 1ull << ppy) + 1);
    }
    if (precincts * mh.layers * mh.csiz * double(tiles) > double(limits.max_packets)) {
        return fail(why, "too many packets");
    }
    return true;
}

}  // namespace nj2k::test

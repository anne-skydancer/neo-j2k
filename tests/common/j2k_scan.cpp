// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
#include "j2k_scan.h"

#include <algorithm>
#include <cmath>

namespace nj2k::test {

namespace {

constexpr std::uint16_t SOC = 0xFF4F, SIZ = 0xFF51, COD = 0xFF52, COC = 0xFF53, TLM = 0xFF55, PLM = 0xFF57,
                        PLT = 0xFF58, QCD = 0xFF5C, QCC = 0xFF5D, RGN = 0xFF5E, POC = 0xFF5F, PPM = 0xFF60,
                        PPT = 0xFF61, CRG = 0xFF63, COM = 0xFF64, CAP = 0xFF50, CPF = 0xFF59, MCT = 0xFF74,
                        MCC = 0xFF75, MCO = 0xFF77, CBD = 0xFF78, SOT = 0xFF90, SOD = 0xFF93, EOC = 0xFFD9;

// The markers OpenJPEG reads in a main header after SIZ (j2k.c, j2k_memory_marker_handler_tab:
// the ones whose states include J2K_STATE_MH).
bool main_header_marker(std::uint16_t m)
{
    switch (m) {
    case COD: case COC: case RGN: case QCD: case QCC: case POC: case TLM: case PLM: case PPM: case CRG:
    case COM: case MCT: case CBD: case CAP: case CPF: case MCC: case MCO:
        return true;
    default:
        return false;
    }
}

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

// COD: Scod, progression, layers (2 bytes), MCT, then SPcod: levels, xcb, ycb, style, transform and,
// when Scod & 1, one precinct byte per resolution. false when the segment is too short.
bool read_cod(std::span<const std::uint8_t> body, Coding& c)
{
    if (body.size() < 10) {
        return false;
    }
    c.is_cod = true;
    c.layers = std::uint16_t(body[2] << 8 | body[3]);
    c.levels = body[5];
    c.xcb = body[6];
    c.ycb = body[7];
    c.precincts.clear();
    if (body[0] & 1) {
        c.precincts.assign(body.begin() + 10, body.end());
    }
    return true;
}

// COC: Ccoc (two bytes past 256 components), Scoc, then SPcoc as in COD.
bool read_coc(std::span<const std::uint8_t> body, std::uint16_t csiz, Coding& c)
{
    const std::size_t at = csiz >= 257 ? 2 : 1;  // Scoc
    if (body.size() < at + 6) {
        return false;
    }
    c.is_cod = false;
    c.levels = body[at + 1];
    c.xcb = body[at + 2];
    c.ycb = body[at + 3];
    c.precincts.clear();
    if (body[at] & 1) {
        c.precincts.assign(body.begin() + static_cast<std::ptrdiff_t>(at + 6), body.end());
    }
    return true;
}

// Precincts and code-blocks one coding makes in one tile-component of tw x th, over every
// resolution (an estimate that only errs high).
struct Structures {
    double precincts = 0, codeblocks = 0;
};

Structures structures(const Coding& c, std::uint64_t tw, std::uint64_t th)
{
    Structures s;
    for (int res = 0; res <= c.levels; ++res) {
        const int shift = std::min(c.levels - res, 62);
        const double rw = double(ceil_div(tw, 1ull << shift)), rh = double(ceil_div(th, 1ull << shift));
        int ppx = 15, ppy = 15;
        if (std::size_t(res) < c.precincts.size()) {
            ppx = c.precincts[std::size_t(res)] & 15;
            ppy = c.precincts[std::size_t(res)] >> 4;
        }
        s.precincts += (std::ceil(rw / std::ldexp(1.0, ppx)) + 1) * (std::ceil(rh / std::ldexp(1.0, ppy)) + 1);
        // A code-block never crosses a precinct, so its size is clipped to the precinct's in the
        // band, which above resolution 0 is half the resolution's. The bands of a resolution:
        // LL at resolution 0, HL, LH and HH above it, each about half its size.
        const int bx = std::max(0, std::min(c.xcb + 2, res ? ppx - 1 : ppx));
        const int by = std::max(0, std::min(c.ycb + 2, res ? ppy - 1 : ppy));
        const double bw = res ? std::ceil(rw / 2) : rw, bh = res ? std::ceil(rh / 2) : rh;
        s.codeblocks += (res ? 3.0 : 1.0) * (std::ceil(bw / std::ldexp(1.0, bx)) + 1) *
                        (std::ceil(bh / std::ldexp(1.0, by)) + 1);
    }
    return s;
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
        const auto seg = bytes.subspan(body, blen);
        // SIZ first, then the markers OpenJPEG reads in a main header
        if (siz ? !main_header_marker(marker) : marker != SIZ) {
            mh.unknown_marker = true;
        }
        switch (marker) {
        case SIZ:
            if (siz) {
                break;  // a second SIZ: OpenJPEG refuses it (marked above)
            }
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
        case COD: {
            Coding c;
            if (!read_cod(seg, c)) {
                fail(why, "short COD");
                return std::nullopt;
            }
            mh.scod = bytes[body];
            mh.progression = bytes[body + 1];
            mh.layers = c.layers;
            mh.mct = bytes[body + 4];
            mh.levels = c.levels;
            mh.cblk_w_exp = c.xcb;
            mh.cblk_h_exp = c.ycb;
            mh.cblk_style = bytes[body + 8];
            mh.transform = bytes[body + 9];
            mh.precincts = c.precincts;
            mh.codings.push_back(std::move(c));
            cod = true;
            break;
        }
        case COC: {
            mh.has_coc = true;
            Coding c;
            if (siz && read_coc(seg, mh.csiz, c)) {
                const int res = c.levels + 1;
                coc_min = coc_min ? std::min(coc_min, res) : res;
                mh.codings.push_back(std::move(c));
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
            const auto seg = bytes.subspan(p + 4, len - 2u);
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
            } else if (m == COD || m == COC) {
                // a tile's COD and COC replace the main header's for that tile (OpenJPEG reads
                // them in any tile-part header, and a later one replaces an earlier one)
                Coding c;
                if (m == COD ? read_cod(seg, c) : read_coc(seg, mh.csiz, c)) {
                    tp.codings.push_back(std::move(c));
                }
            }
            p += 2u + len;
        }
        if (!sod) {
            break;  // a cut header: OpenJPEG stops there too
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
    if (tps.size() != 1 || !tps[0].codings.empty()) {
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

bool within_limits(const MainHeader& mh, std::span<const TilePart> tile_parts, int reduce, const Limits& limits,
                   std::string* why)
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
    const std::uint64_t w = ceil_div(mh.width(), 1ull << reduce), h = ceil_div(mh.height(), 1ull << reduce);
    if (w > limits.max_side || h > limits.max_side) {
        return fail(why, "too large at the decoded level");
    }
    if (w * h * mh.csiz > limits.max_samples) {
        return fail(why, "too many samples at the decoded level");
    }
    // The worst of every COD and COC, wherever it stands, as if it applied to every tile and
    // component: the layers of any COD, the structures of any coding in a tile of full size.
    std::vector<const Coding*> codings;
    for (const auto& c : mh.codings) {
        codings.push_back(&c);
    }
    for (const auto& tp : tile_parts) {
        for (const auto& c : tp.codings) {
            codings.push_back(&c);
        }
    }
    std::uint32_t layers = 0;
    Structures worst;
    const std::uint64_t tw = std::min<std::uint64_t>(mh.xtsiz, mh.width());
    const std::uint64_t th = std::min<std::uint64_t>(mh.ytsiz, mh.height());
    for (const Coding* c : codings) {
        if (c->is_cod) {
            if (c->layers == 0) {
                return fail(why, "no layers");
            }
            layers = std::max<std::uint32_t>(layers, c->layers);
        }
        const Structures s = structures(*c, tw, th);
        worst.precincts = std::max(worst.precincts, s.precincts);
        worst.codeblocks = std::max(worst.codeblocks, s.codeblocks);
    }
    if (layers == 0 || layers > limits.max_layers) {
        return fail(why, "too many layers");
    }
    const double per_tile = double(mh.csiz) * double(tiles);
    if (worst.precincts * layers * per_tile > double(limits.max_packets)) {
        return fail(why, "too many packets");
    }
    if (worst.codeblocks * per_tile > double(limits.max_codeblocks)) {
        return fail(why, "too many code-blocks");
    }
    return true;
}

}  // namespace nj2k::test

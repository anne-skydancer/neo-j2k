// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The departures (doc/departures.md) a codestream shows, which let neo-j2k's header read or decode
// differ from OpenJPEG's on purpose: the differential fuzz targets accept a difference only when
// one of these explains it. Each check is as narrow as the departure: it names the markers (or the
// bytes) that make the two decoders differ, not merely a marker that could.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "j2k_scan.h"

namespace nj2k::fuzz {

inline std::uint32_t be16(std::span<const std::uint8_t> b, std::size_t p)
{
    return std::uint32_t(b[p]) << 8 | b[p + 1];
}

inline std::uint32_t be32(std::span<const std::uint8_t> b, std::size_t p)
{
    return be16(b, p) << 16 | be16(b, p + 2);
}

// OpenJPEG's markers of a main header (j2k.c, the handlers whose states include the main header's),
// SIZ apart: it reads them, scans past any other marker for the next one it knows, and refuses
// PLT, PPT, SOP and a second SIZ there.
inline bool openjpeg_main_header_marker(std::uint32_t m)
{
    switch (m) {
    case 0xFF52: case 0xFF53: case 0xFF5E: case 0xFF5C: case 0xFF5D: case 0xFF5F: case 0xFF55: case 0xFF57:
    case 0xFF60: case 0xFF63: case 0xFF64: case 0xFF74: case 0xFF78: case 0xFF50: case 0xFF59: case 0xFF75:
    case 0xFF77:
        return true;
    default:
        return false;
    }
}

// Every marker OpenJPEG's header reader has a handler for, whatever the header (SIZ, SOT, PLT, PPT
// and SOP too): one it stops at when it scans past a marker it does not know.
inline bool openjpeg_knows(std::uint32_t m)
{
    return m == 0xFF51 || m == 0xFF90 || m == 0xFF58 || m == 0xFF61 || m == 0xFF91 || openjpeg_main_header_marker(m);
}

// The departures a main header shows. header: one that makes neo-j2k's header read differ from
// OpenJPEG's (DP-5 to DP-8), or null; decode: one that changes only the decode (DP-12: a POC read
// before the COD it depends on; DP-14: HT code-blocks), or null. Walked by the segment lengths, as
// neo-j2k reads it, up to the first SOT or to where the bytes stop making segments (where both
// refuse).
struct MainHeaderDepartures {
    const char* header = nullptr;
    const char* decode = nullptr;
};

inline MainHeaderDepartures main_header_departures(std::span<const std::uint8_t> cs, std::uint32_t reduce)
{
    MainHeaderDepartures out;
    if (cs.size() < 4 || cs[0] != 0xFF || cs[1] != 0x4F) {
        return out;
    }
    std::size_t pos = 2;
    bool siz = false, coc = false, qcc = false, poc = false;
    std::uint32_t numcomps = 0, codings = 0, most_resolutions = 0, fewest_resolutions = UINT32_MAX,
                  fewest_steps = UINT32_MAX;
    auto header = [&](const char* why) {
        out.header = why;
        return out;
    };
    auto decode = [&](const char* why) {
        if (!out.decode) {
            out.decode = why;
        }
    };
    while (cs.size() - pos >= 2) {
        const std::uint32_t m = be16(cs, pos);
        if (m == 0xFF90 || m < 0xFF00) {
            break;  // SOT: the main header ends; no marker: both refuse
        }
        if (!siz && m != 0xFF51) {
            return header("DP-7: SIZ does not follow SOC");
        }
        if (m >= 0xFF30 && m <= 0xFF3F) {
            // no segment: neo-j2k goes on to the next marker; OpenJPEG does not know the marker and
            // scans ahead for one it knows, which comes to the same only when one follows at once
            if (cs.size() - pos >= 4 && !openjpeg_knows(be16(cs, pos + 2))) {
                return header("DP-7: a marker without a segment, which OpenJPEG scans past");
            }
            pos += 2;
            continue;
        }
        if (m == 0xFFD9 || m == 0xFF93 || m == 0xFF92) {
            return header("DP-7: EOC, SOD or EPH in the main header");
        }
        if (m >= 0xFF70 && m <= 0xFF7F) {
            return header("DP-7: a JPEG 2000 Part 2 marker");
        }
        if (m == 0xFF58 || m == 0xFF61 || m == 0xFF91 || (m == 0xFF51 && siz)) {
            break;  // both refuse
        }
        if (m != 0xFF51 && !openjpeg_main_header_marker(m)) {
            return header("DP-7: a marker OpenJPEG does not know");
        }
        if (cs.size() - pos < 4) {
            break;
        }
        const std::uint32_t length = be16(cs, pos + 2);
        if (length < 2 || cs.size() - pos - 2 < length) {
            break;
        }
        const std::size_t body = pos + 4, n = length - 2;
        const std::size_t room = numcomps <= 256 ? 1 : 2;
        // step sizes a QCD or QCC gives (its body from Sqcd on)
        auto steps = [&](std::size_t at, std::size_t bytes) -> const char* {
            if (bytes < 1) {
                return nullptr;
            }
            const std::uint32_t style = cs[at] & 0x1F;
            if (style > 2) {
                return "DP-8: a reserved quantization style";
            }
            if (style != 1) {
                const std::uint32_t count = std::uint32_t(style == 0 ? bytes - 1 : (bytes - 1) / 2);
                fewest_steps = count < fewest_steps ? count : fewest_steps;
            }
            return nullptr;
        };
        auto resolutions = [&](std::uint32_t r) {
            most_resolutions = r > most_resolutions ? r : most_resolutions;
            fewest_resolutions = r < fewest_resolutions ? r : fewest_resolutions;
        };
        switch (m) {
        case 0xFF51:  // SIZ
            siz = true;
            numcomps = n >= 36 ? be16(cs, body + 34) : 0;
            break;
        case 0xFF52:  // COD
            ++codings;
            if (coc) {
                return header("DP-5: a COD after a COC");
            }
            if (n >= 2 && cs[body + 1] > 4) {
                return header("DP-8: a progression order above 4");
            }
            if (n >= 6) {
                resolutions(cs[body + 5] + 1u);
            }
            if (n >= 9 && (cs[body + 8] & 0x40)) {
                decode("DP-14: HT code-blocks (JPEG 2000 Part 15)");
            }
            if (poc) {
                // OpenJPEG bounds a POC's layer end by the layers known when it reads the POC
                decode("DP-12: a POC before the COD of the main header");
            }
            break;
        case 0xFF53:  // COC
            ++codings;
            coc = true;
            if (n >= room + 2) {
                resolutions(cs[body + room + 1] + 1u);
            }
            if (n >= room + 5 && (cs[body + room + 4] & 0x40)) {
                decode("DP-14: HT code-blocks (JPEG 2000 Part 15)");
            }
            break;
        case 0xFF5C:  // QCD
            if (qcc) {
                return header("DP-5: a QCD after a QCC");
            }
            if (const char* why = steps(body, n)) {
                return header(why);
            }
            break;
        case 0xFF5D:  // QCC
            qcc = true;
            if (n > room) {
                if (const char* why = steps(body + room, n - room)) {
                    return header(why);
                }
            }
            break;
        case 0xFF5F:  // POC
            poc = true;
            break;
        default:
            break;
        }
        pos += 2 + length;
    }
    if (codings > 1 && fewest_resolutions <= reduce) {
        // OpenJPEG refuses the factor at the COD or COC that has no more resolutions, as it reads
        // it; neo-j2k checks the components' parameters after precedence
        return header("DP-5: the reduce factor checked after precedence, not against each COD and COC");
    }
    if (most_resolutions > 0 && fewest_steps < 3 * most_resolutions - 2) {
        return header("DP-6: fewer step sizes than sub-bands");
    }
    return out;
}

// The departure a main header shows for a header read (DP-5 to DP-8), or null.
inline const char* header_departure(std::span<const std::uint8_t> cs, std::uint32_t reduce)
{
    return main_header_departures(cs, reduce).header;
}

// What the tile-part headers of one tile have shown so far (DP-12 depends on the order of a tile's
// POC and COD, and on the main header's POC).
struct TileMarkers {
    bool poc = false;
    bool cod = false;
    std::uint32_t cod_layers = 0;
};

// The departure a tile-part header shows, or null: walked by the segment lengths from after SOT to
// SOD, as neo-j2k reads it. DP-7: a marker OpenJPEG does not take in a tile-part header where
// neo-j2k does (a marker segment it does not know, skipped by its length; a marker without a
// segment, 0xFF30-0xFF3F; a Part 2 marker OpenJPEG reads); DP-5: a COD after a COC or a QCD after a
// QCC of the same tile, or a COD or COC with no more resolutions than the reduce factor (OpenJPEG
// refuses it as it reads it, neo-j2k checks after precedence); DP-8: a progression order above 4 or
// a reserved quantization style; DP-6: fewer step sizes than sub-bands; DP-12: a POC in a tile's
// headers and in the main header's (the tile's replaces the main header's), a tile's POC before its
// COD, or the main header's POC with a tile COD of other layers (each layer end is bounded by the
// tile's layers); DP-14: HT code-blocks. Markers both refuse there (SOC, EOC, EPH, SOT, SOP and the
// main header's) end the walk with no departure.
inline const char* tile_part_departure(std::span<const std::uint8_t> cs, const test::MainHeader& mh,
                                       const test::TilePart& tp, std::uint32_t reduce, TileMarkers& tile)
{
    std::size_t pos = tp.sot + 12;
    const std::size_t end = tp.data >= 2 ? tp.data - 2 : 0;
    bool coc = false, qcc = false;
    std::uint32_t most_resolutions = mh.levels + 1u, fewest_resolutions = UINT32_MAX, fewest_steps = UINT32_MAX;
    const std::size_t room = mh.csiz <= 256 ? 1 : 2;
    while (pos + 2 <= end && pos + 2 <= cs.size()) {
        const std::uint32_t m = be16(cs, pos);
        if (m >= 0xFF30 && m <= 0xFF3F) {
            return "DP-7: a marker without a segment in a tile-part header (OpenJPEG fails on it)";
        }
        if (m == 0xFF4F || m == 0xFFD9 || m == 0xFF92 || m == 0xFF93) {
            return nullptr;  // SOC, EOC, EPH (SOD ends the header): both refuse
        }
        if (pos + 4 > end || pos + 4 > cs.size()) {
            break;
        }
        const std::uint32_t length = be16(cs, pos + 2);
        if (m < 0xFF00 || length < 2 || cs.size() - pos - 2 < length) {
            break;
        }
        const std::size_t body = pos + 4, n = length - 2;
        auto steps = [&](std::size_t at, std::size_t bytes) -> const char* {
            if (bytes < 1) {
                return nullptr;
            }
            const std::uint32_t style = cs[at] & 0x1F;
            if (style > 2) {
                return "DP-8: a reserved quantization style in a tile-part header";
            }
            if (style != 1) {
                const std::uint32_t count = std::uint32_t(style == 0 ? bytes - 1 : (bytes - 1) / 2);
                fewest_steps = count < fewest_steps ? count : fewest_steps;
            }
            return nullptr;
        };
        auto resolutions = [&](std::uint32_t r) {
            most_resolutions = r > most_resolutions ? r : most_resolutions;
            fewest_resolutions = r < fewest_resolutions ? r : fewest_resolutions;
        };
        switch (m) {
        case 0xFF52:  // COD
            if (coc) {
                return "DP-5: a COD after a COC in a tile-part header";
            }
            if (n >= 2 && cs[body + 1] > 4) {
                return "DP-8: a progression order above 4 in a tile-part header";
            }
            if (n >= 6) {
                resolutions(cs[body + 5] + 1u);
            }
            if (n >= 9 && (cs[body + 8] & 0x40)) {
                return "DP-14: HT code-blocks (JPEG 2000 Part 15) in a tile-part header";
            }
            if (tile.poc) {
                return "DP-12: a tile's POC before its COD";
            }
            tile.cod = true;
            tile.cod_layers = n >= 4 ? be16(cs, body + 2) : 0;
            if (mh.has_poc && tile.cod_layers != mh.layers) {
                return "DP-12: the main header's POC with a tile COD of other layers";
            }
            break;
        case 0xFF53:  // COC
            coc = true;
            if (n >= room + 2) {
                resolutions(cs[body + room + 1] + 1u);
            }
            if (n >= room + 5 && (cs[body + room + 4] & 0x40)) {
                return "DP-14: HT code-blocks (JPEG 2000 Part 15) in a tile-part header";
            }
            break;
        case 0xFF5C:  // QCD
            if (qcc) {
                return "DP-5: a QCD after a QCC in a tile-part header";
            }
            if (const char* why = steps(body, n)) {
                return why;
            }
            break;
        case 0xFF5D:  // QCC
            qcc = true;
            if (n > room) {
                if (const char* why = steps(body + room, n - room)) {
                    return why;
                }
            }
            break;
        case 0xFF5F:  // POC
            if (mh.has_poc) {
                return "DP-12: a POC in a tile's headers and in the main header";
            }
            tile.poc = true;
            break;
        case 0xFF5E:  // RGN
        case 0xFF58:  // PLT
        case 0xFF61:  // PPT
        case 0xFF64:  // COM
            break;
        case 0xFF74:  // MCT
        case 0xFF75:  // MCC
        case 0xFF77:  // MCO
            return "DP-7: a JPEG 2000 Part 2 marker in a tile-part header (OpenJPEG reads it)";
        case 0xFF90:  // SOT
        case 0xFF91:  // SOP
        case 0xFF50:  // CAP
        case 0xFF51:  // SIZ
        case 0xFF55:  // TLM
        case 0xFF57:  // PLM
        case 0xFF59:  // CPF
        case 0xFF60:  // PPM
        case 0xFF63:  // CRG
            return nullptr;  // markers of another place: both refuse
        default:
            if (m >= 0xFF70 && m <= 0xFF7F) {
                return nullptr;  // the other Part 2 markers: both refuse
            }
            return "DP-7: a marker segment OpenJPEG does not know, in a tile-part header";
        }
        pos += 2 + length;
    }
    if (fewest_resolutions <= reduce) {
        return "DP-5: a tile-part COD or COC with no more resolutions than the reduce factor";
    }
    if (fewest_steps < 3 * most_resolutions - 2) {
        return "DP-6: fewer step sizes than sub-bands in a tile-part header";
    }
    return nullptr;
}

// The departure a whole codestream shows for a decode, or null: the main header's, any tile-part
// header's, or DP-3, a tile whose tile-parts arrived without data bytes and whose packet headers
// are packed in PPM or PPT (neo-j2k decodes it, OpenJPEG refuses it).
inline const char* decode_departure(std::span<const std::uint8_t> cs, std::uint32_t reduce)
{
    const MainHeaderDepartures main = main_header_departures(cs, reduce);
    if (main.header) {
        return main.header;
    }
    if (main.decode) {
        return main.decode;
    }
    const auto mh = test::parse_main_header(cs);
    if (!mh) {
        return nullptr;
    }
    const auto tps = test::scan_tile_parts(cs, *mh);
    const std::uint64_t tiles = mh->tiles_x() * mh->tiles_y();
    std::vector<std::uint64_t> bytes(std::size_t(tiles <= 65536 ? tiles : 0), 0);
    std::vector<bool> seen(bytes.size(), false), packed(bytes.size(), mh->has_ppm);
    std::vector<TileMarkers> markers(bytes.size());
    TileMarkers spare;
    for (const auto& tp : tps) {
        const bool known = tp.tile < bytes.size();
        if (const char* why = tile_part_departure(cs, *mh, tp, reduce, known ? markers[tp.tile] : spare)) {
            return why;
        }
        if (known) {
            seen[tp.tile] = true;
            bytes[tp.tile] += tp.end - tp.data;
            packed[tp.tile] = packed[tp.tile] || tp.has_ppt;
        }
    }
    for (std::size_t t = 0; t < bytes.size(); ++t) {
        if (seen[t] && bytes[t] == 0 && packed[t]) {
            return "DP-3: a tile whose tile-parts hold no data bytes, its packet headers in PPM or PPT";
        }
    }
    return nullptr;
}

}  // namespace nj2k::fuzz

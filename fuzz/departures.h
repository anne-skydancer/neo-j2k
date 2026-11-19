// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The departures (doc/departures.md) a codestream shows, which let neo-j2k's header read or decode
// differ from OpenJPEG's on purpose: the differential fuzz targets accept a difference only when
// one of these explains it.
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

// The departure (doc/departures.md) a main header shows that lets neo-j2k's header read differ from
// OpenJPEG's, or null when it shows none. Walked by the segment lengths, as neo-j2k reads it, up to
// the first SOT or to where the bytes stop making segments (where both refuse).
inline const char* header_departure(std::span<const std::uint8_t> cs, std::uint32_t reduce)
{
    if (cs.size() < 4 || cs[0] != 0xFF || cs[1] != 0x4F) {
        return nullptr;
    }
    std::size_t pos = 2;
    bool siz = false, coc = false, qcc = false;
    std::uint32_t numcomps = 0, codings = 0, most_resolutions = 0, fewest_steps = UINT32_MAX;
    while (cs.size() - pos >= 2) {
        const std::uint32_t m = be16(cs, pos);
        if (m == 0xFF90 || m < 0xFF00) {
            break;  // SOT: the main header ends; no marker: both refuse
        }
        if (!siz && m != 0xFF51) {
            return "DP-7: SIZ does not follow SOC";
        }
        if (m >= 0xFF30 && m <= 0xFF3F) {
            // no segment: neo-j2k goes on to the next marker; OpenJPEG does not know the marker and
            // scans ahead for one it knows, which comes to the same only when one follows at once
            if (cs.size() - pos >= 4 && !openjpeg_knows(be16(cs, pos + 2))) {
                return "DP-7: a marker without a segment, which OpenJPEG scans past";
            }
            pos += 2;
            continue;
        }
        if (m == 0xFFD9 || m == 0xFF93 || m == 0xFF92) {
            return "DP-7: EOC, SOD or EPH in the main header";
        }
        if (m >= 0xFF70 && m <= 0xFF7F) {
            return "DP-7: a JPEG 2000 Part 2 marker";
        }
        if (m == 0xFF58 || m == 0xFF61 || m == 0xFF91 || (m == 0xFF51 && siz)) {
            break;  // both refuse
        }
        if (m != 0xFF51 && !openjpeg_main_header_marker(m)) {
            return "DP-7: a marker OpenJPEG does not know";
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
        switch (m) {
        case 0xFF51:  // SIZ
            siz = true;
            numcomps = n >= 36 ? be16(cs, body + 34) : 0;
            break;
        case 0xFF52:  // COD
            ++codings;
            if (coc) {
                return "DP-5: a COD after a COC";
            }
            if (n >= 2 && cs[body + 1] > 4) {
                return "DP-8: a progression order above 4";
            }
            if (n >= 6 && cs[body + 5] + 1u > most_resolutions) {
                most_resolutions = cs[body + 5] + 1u;
            }
            if (n >= 9 && (cs[body + 8] & 0x40)) {
                return "DP-14: HT code-blocks (JPEG 2000 Part 15)";
            }
            break;
        case 0xFF53:  // COC
            ++codings;
            coc = true;
            if (n >= room + 2 && cs[body + room + 1] + 1u > most_resolutions) {
                most_resolutions = cs[body + room + 1] + 1u;
            }
            if (n >= room + 5 && (cs[body + room + 4] & 0x40)) {
                return "DP-14: HT code-blocks (JPEG 2000 Part 15)";
            }
            break;
        case 0xFF5C:  // QCD
            if (qcc) {
                return "DP-5: a QCD after a QCC";
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
        default:
            break;
        }
        pos += 2 + length;
    }
    if (reduce > 0 && codings > 1) {
        return "DP-5: the reduce factor checked after precedence, not against each COD and COC";
    }
    if (most_resolutions > 0 && fewest_steps < 3 * most_resolutions - 2) {
        return "DP-6: fewer step sizes than sub-bands";
    }
    return nullptr;
}

// The departure a tile-part header shows, or null: walked by the segment lengths from after SOT to
// SOD, as neo-j2k reads it. DP-7: a marker OpenJPEG does not take in a tile-part header (it fails
// the tile-part; neo-j2k skips a segment it does not know by its length); DP-5: a COD after a COC
// or a QCD after a QCC of the same tile, or a reduce factor with tile coding styles (checked after
// precedence); DP-8: a progression order above 4 or a reserved quantization style; DP-6: fewer step
// sizes than sub-bands; DP-12: a POC in a tile's header (it replaces the main header's, and its
// layer end is bounded by the tile's layers); DP-14: HT code-blocks.
inline const char* tile_part_departure(std::span<const std::uint8_t> cs, const test::MainHeader& mh,
                                       const test::TilePart& tp, std::uint32_t reduce)
{
    std::size_t pos = tp.sot + 12;
    const std::size_t end = tp.data >= 2 ? tp.data - 2 : 0;
    bool coc = false, qcc = false, coding = false;
    std::uint32_t most_resolutions = mh.levels + 1u, fewest_steps = UINT32_MAX;
    const std::size_t room = mh.csiz <= 256 ? 1 : 2;
    while (pos + 4 <= end && pos + 4 <= cs.size()) {
        const std::uint32_t m = be16(cs, pos);
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
        switch (m) {
        case 0xFF52:  // COD
            coding = true;
            if (coc) {
                return "DP-5: a COD after a COC in a tile-part header";
            }
            if (n >= 2 && cs[body + 1] > 4) {
                return "DP-8: a progression order above 4 in a tile-part header";
            }
            if (n >= 6 && cs[body + 5] + 1u > most_resolutions) {
                most_resolutions = cs[body + 5] + 1u;
            }
            if (n >= 9 && (cs[body + 8] & 0x40)) {
                return "DP-14: HT code-blocks (JPEG 2000 Part 15) in a tile-part header";
            }
            break;
        case 0xFF53:  // COC
            coding = true;
            coc = true;
            if (n >= room + 2 && cs[body + room + 1] + 1u > most_resolutions) {
                most_resolutions = cs[body + room + 1] + 1u;
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
            return "DP-12: a POC in a tile-part header";
        case 0xFF5E:  // RGN
        case 0xFF58:  // PLT
        case 0xFF61:  // PPT
        case 0xFF64:  // COM
            break;
        default:
            return "DP-7: a marker OpenJPEG does not take in a tile-part header";
        }
        pos += 2 + length;
    }
    if (reduce > 0 && coding) {
        return "DP-5: the reduce factor checked after precedence, with tile coding styles";
    }
    if (fewest_steps < 3 * most_resolutions - 2) {
        return "DP-6: fewer step sizes than sub-bands in a tile-part header";
    }
    return nullptr;
}

// The departure a whole codestream shows for a decode, or null: the main header's, any tile-part
// header's, or DP-3, a tile whose tile-parts arrived and hold no data bytes (neo-j2k decodes it,
// OpenJPEG refuses it).
inline const char* decode_departure(std::span<const std::uint8_t> cs, std::uint32_t reduce)
{
    if (const char* why = header_departure(cs, reduce)) {
        return why;
    }
    const auto mh = test::parse_main_header(cs);
    if (!mh) {
        return nullptr;
    }
    const auto tps = test::scan_tile_parts(cs, *mh);
    std::vector<std::uint64_t> bytes(std::size_t(mh->tiles_x() * mh->tiles_y() <= 65536 ? mh->tiles_x() * mh->tiles_y() : 0), 0);
    std::vector<bool> seen(bytes.size(), false);
    for (const auto& tp : tps) {
        if (const char* why = tile_part_departure(cs, *mh, tp, reduce)) {
            return why;
        }
        if (tp.tile < bytes.size()) {
            seen[tp.tile] = true;
            bytes[tp.tile] += tp.end - tp.data;
        }
    }
    for (std::size_t t = 0; t < bytes.size(); ++t) {
        if (seen[t] && bytes[t] == 0) {
            return "DP-3: a tile whose tile-parts hold no data bytes";
        }
    }
    return nullptr;
}

}  // namespace nj2k::fuzz

// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// A minimal codestream reader for the tests and tools: the main header's SIZ and COD facts, the
// tile-parts, the coding parameters of every COD and COC marker, and packet boundaries from SOP
// markers or PLT. It never decodes; it lets the tests pick cut points, and the fuzz targets name the
// departures that explain a difference from the oracle.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nj2k::test {

// The coding parameters of one COD or COC marker segment: what sizes the decoder's code-block and
// precinct structures and its packet walk.
struct Coding {
    bool is_cod = false;               // COD (it sets the layers) or COC
    std::uint16_t layers = 0;          // COD only
    std::uint8_t levels = 0;           // decomposition levels: resolutions less one
    std::uint8_t xcb = 0, ycb = 0;     // code-block size exponents less 2, as coded
    std::vector<std::uint8_t> precincts;  // PPx | PPy << 4 per resolution, when the marker gives them
};

struct MainHeader {
    std::uint32_t xsiz = 0, ysiz = 0, xosiz = 0, yosiz = 0;
    std::uint32_t xtsiz = 0, ytsiz = 0, xtosiz = 0, ytosiz = 0;
    std::uint16_t rsiz = 0;
    std::uint16_t csiz = 0;
    std::vector<std::uint8_t> ssiz, xrsiz, yrsiz;
    // COD (the last one: OpenJPEG lets a later COD of the main header replace an earlier one)
    std::uint8_t scod = 0, progression = 0, mct = 0, levels = 0, cblk_w_exp = 0, cblk_h_exp = 0, cblk_style = 0,
                 transform = 0;
    std::uint16_t layers = 0;
    std::vector<std::uint8_t> precincts;  // PPx | PPy << 4 per resolution, when Scod & 1
    // every COD and COC of the main header, in order
    std::vector<Coding> codings;
    // the fewest resolutions of any component in the main header (COD, COC)
    int min_resolutions = 0;
    bool has_coc = false, has_qcc = false, has_poc = false, has_ppm = false, has_tlm = false, has_plm = false,
         has_com = false;
    // A marker OpenJPEG does not read in a main header. It skips an unknown marker by scanning for
    // the next marker it knows, ignoring the length, so what follows may differ from what the
    // lengths say (this reader follows them); a known marker out of place makes it fail.
    bool unknown_marker = false;
    std::size_t end = 0;  // offset of the first SOT: the main header's end
    std::uint64_t tiles_x() const noexcept;
    std::uint64_t tiles_y() const noexcept;
    std::uint64_t width() const noexcept { return std::uint64_t(xsiz) - xosiz; }
    std::uint64_t height() const noexcept { return std::uint64_t(ysiz) - yosiz; }
};

// Parses SOC to the first SOT. nullopt (with *why) when the bytes do not hold a complete main
// header with SIZ and COD.
std::optional<MainHeader> parse_main_header(std::span<const std::uint8_t> bytes, std::string* why = nullptr);

struct TilePart {
    std::size_t sot = 0;   // offset of the SOT marker
    std::size_t data = 0;  // first byte after SOD
    std::size_t end = 0;   // one past the last byte of the tile-part (clamped to the bytes)
    std::uint16_t tile = 0;
    std::uint32_t psot = 0;
    std::uint8_t tpsot = 0, tnsot = 0;
    bool has_ppt = false, has_plt = false;
    std::vector<std::uint32_t> plt;  // packet lengths, in order, when PLT is present
    std::vector<Coding> codings;     // the COD and COC markers of the tile-part header
};

// Every tile-part, following Psot (Psot = 0: to the end, less an EOC), as OpenJPEG does: after a
// tile-part it reads a SOT or the end, nothing else.
std::vector<TilePart> scan_tile_parts(std::span<const std::uint8_t> bytes, const MainHeader& mh);

// Packet start offsets of a stream with one tile-part, from SOP markers or else from PLT;
// nullopt when neither gives them.
std::optional<std::vector<std::size_t>> packet_starts(std::span<const std::uint8_t> bytes, const MainHeader& mh,
                                                      const TilePart& tp);

// Offsets where layer k (1..layers-1) starts, for a one-tile, one-tile-part LRCP stream whose
// packets can be located: every prefix that ends there holds exactly the first k layers.
std::optional<std::vector<std::size_t>> layer_boundaries(std::span<const std::uint8_t> bytes);

}  // namespace nj2k::test

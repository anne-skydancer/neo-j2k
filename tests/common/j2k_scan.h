// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// A minimal codestream reader for the tests and tools: the main header's SIZ and COD facts, the
// tile-parts, and packet boundaries from SOP markers or PLT. It never decodes; it lets the tests
// pick cut points and keep known resource bombs away from the oracle.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nj2k::test {

struct MainHeader {
    std::uint32_t xsiz = 0, ysiz = 0, xosiz = 0, yosiz = 0;
    std::uint32_t xtsiz = 0, ytsiz = 0, xtosiz = 0, ytosiz = 0;
    std::uint16_t rsiz = 0;
    std::uint16_t csiz = 0;
    std::vector<std::uint8_t> ssiz, xrsiz, yrsiz;
    // COD
    std::uint8_t scod = 0, progression = 0, mct = 0, levels = 0, cblk_w_exp = 0, cblk_h_exp = 0, cblk_style = 0,
                 transform = 0;
    std::uint16_t layers = 0;
    std::vector<std::uint8_t> precincts;  // PPx | PPy << 4 per resolution, when Scod & 1
    // the fewest resolutions of any component in the main header (COD, COC)
    int min_resolutions = 0;
    bool has_coc = false, has_qcc = false, has_poc = false, has_ppm = false, has_tlm = false, has_plm = false,
         has_com = false;
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
};

// Every tile-part, following Psot (Psot = 0: to the end, less an EOC).
std::vector<TilePart> scan_tile_parts(std::span<const std::uint8_t> bytes, const MainHeader& mh);

// Packet start offsets of a stream with one tile-part, from SOP markers or else from PLT;
// nullopt when neither gives them.
std::optional<std::vector<std::size_t>> packet_starts(std::span<const std::uint8_t> bytes, const MainHeader& mh,
                                                      const TilePart& tp);

// Offsets where layer k (1..layers-1) starts, for a one-tile, one-tile-part LRCP stream whose
// packets can be located: every prefix that ends there holds exactly the first k layers.
std::optional<std::vector<std::size_t>> layer_boundaries(std::span<const std::uint8_t> bytes);

// Resource limits the tests apply before they hand a stream to the oracle, which has none (a
// 102-byte header commits 779 MB in OpenJPEG: fork audit S2, S3). Stage 1b's profiles replace it.
struct Limits {
    std::uint64_t max_tiles = 1024;
    std::uint32_t max_components = 8;
    std::uint32_t max_layers = 64;
    std::uint64_t max_side = 16384;          // at the decoded level
    std::uint64_t max_samples = 1ull << 28;  // all components at the decoded level
    std::uint64_t max_packets = 1ull << 20;  // layers x precincts x components x tiles (main COD)
};
bool within_limits(const MainHeader& mh, int reduce, const Limits& limits, std::string* why = nullptr);

}  // namespace nj2k::test

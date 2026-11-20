// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The codestream header model: the main header and the tile-part headers, read once into an arena
// and not changed after. It owns its bytes (packed packet headers included), so it never points into
// a buffer the host may free. Per-tile parameters exist only for tiles whose headers carry markers
// (fork audit S2: no per-tile allocation from what SIZ declares), and every limit is checked before
// what it bounds is allocated (owner decision D10).
//
// Marker precedence is applied when a tile-component's parameters are resolved, whatever the order
// the markers came in (plan W7): tile-part COC > tile-part COD > main COC > main COD, and the same for
// QCC/QCD and RGN. Quantization step counts are checked against the bands they must cover (W8).
// Where OpenJPEG 2.5.4 accepts a header the standard forbids, the departures are in
// doc/departures.md.
#pragma once

#include <cstddef>
#include <cstdint>

#include <neoj2k/neoj2k.h>

#include "arena.h"
#include "messages.h"

namespace nj2k::detail {

inline constexpr std::uint32_t kMaxResolutions = 33;                   // NJ2K_J2K_MAXRLVLS
inline constexpr std::uint32_t kMaxBands = 3 * kMaxResolutions - 2;   // 97
inline constexpr std::uint32_t kMaxPocs = 32;  // OpenJPEG's J2K_MAX_POCS: at most 31 changes per tile
inline constexpr std::uint32_t kMaxComponents = 16384;
inline constexpr std::uint32_t kMaxTiles = 65535;

namespace mk {
inline constexpr std::uint16_t SOC = 0xFF4F, CAP = 0xFF50, SIZ = 0xFF51, COD = 0xFF52, COC = 0xFF53, TLM = 0xFF55,
                               PLM = 0xFF57, PLT = 0xFF58, CPF = 0xFF59, QCD = 0xFF5C, QCC = 0xFF5D, RGN = 0xFF5E,
                               POC = 0xFF5F, PPM = 0xFF60, PPT = 0xFF61, CRG = 0xFF63, COM = 0xFF64, SOT = 0xFF90,
                               SOP = 0xFF91, EPH = 0xFF92, SOD = 0xFF93, EOC = 0xFFD9;
}  // namespace mk

// Markers that carry no segment (ISO 15444-1 A.1): SOC, SOD, EOC, EPH and 0xFF30-0xFF3F.
inline bool segmentless(std::uint32_t m) noexcept
{
    return m == mk::SOC || m == mk::SOD || m == mk::EOC || m == mk::EPH || (m >= 0xFF30 && m <= 0xFF3F);
}

inline constexpr std::uint8_t kCstyPrecincts = 0x01, kCstySop = 0x02, kCstyEph = 0x04;
inline constexpr std::uint8_t kCblkHT = 0x40, kCblkHTMixed = 0x80;
inline constexpr std::uint8_t kQntNone = 0, kQntDerived = 1, kQntExpounded = 2;
inline constexpr std::uint16_t kRsizPart2 = 0x8000, kRsizHT = 0x4000;

// ---- where the header bytes come from ---------------------------------------------------------

struct Stream;

// The bytes a header is read from: borrowed memory, or a host stream. A plain class, not an
// interface: the library is built without RTTI, and code built with it (the tests, the fuzz
// targets, whose UBSan checks virtual calls against type information) uses it too.
class Source {
public:
    Source(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}
    explicit Source(Stream& stream) noexcept : stream_(&stream) {}

    // Reads up to n bytes; fewer only when the bytes end.
    std::size_t read(std::uint8_t* dst, std::size_t n) noexcept;
    // Skips n bytes; false when the bytes end first.
    bool skip(std::uint64_t n) noexcept;
    // Bytes read or skipped since the start.
    std::uint64_t tell() const noexcept;
    // Bytes left, or -1 when unknown.
    std::int64_t bytes_left() const noexcept;

private:
    Stream* stream_ = nullptr;
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t at_ = 0;
};

// ---- the model ----------------------------------------------------------------------------------

struct SizComponent {
    std::uint8_t prec;  // 1..31 (OpenJPEG's range; the standard allows up to 38)
    std::uint8_t sgnd;
    std::uint8_t dx, dy;  // 1..255
};

// What one COD or COC marker says about a tile-component (SPcod, SPcoc).
struct CodingStyle {
    std::uint8_t csty;            // COD: Scod & 1; COC: Scoc as read (OpenJPEG's tccp csty)
    std::uint8_t numresolutions;  // 1..33
    std::uint8_t cblkw, cblkh;    // code-block exponents, 2..10, sum at most 12
    std::uint8_t cblksty;
    std::uint8_t qmfbid;          // 0: 9/7 irreversible, 1: 5/3 reversible
    std::uint8_t precincts[kMaxResolutions];  // PPx | PPy << 4 per resolution; 0xFF when not given
};

// The tile-wide part of a COD marker (Scod, SGcod).
struct TileCoding {
    std::uint8_t csty;        // Scod: precincts, SOP, EPH
    std::uint8_t prg;         // progression order, 0..4
    std::uint16_t numlayers;  // 1..65535
    std::uint8_t mct;         // 0 or 1
};

// What one QCD or QCC marker says (SQcd, SQcc).
struct Quantization {
    std::uint8_t qntsty;  // none, derived or expounded
    std::uint8_t numgbits;
    std::uint8_t nbands;  // step sizes given, at most kMaxBands (more are ignored, as OpenJPEG does)
    std::uint16_t steps[kMaxBands];  // exponent << 11 | mantissa; "none" has mantissa 0

    // A band's exponent and mantissa, as OpenJPEG derives them (derived: from band 0).
    void step(std::uint32_t band, std::uint32_t& expn, std::uint32_t& mant) const noexcept;
};

// One progression order change (POC), as read; bounds are applied where it is used.
struct PocEntry {
    std::uint16_t compno0, compno1, layno1;
    std::uint8_t resno0, resno1, prg;
};

struct TlmEntry {
    std::uint16_t tile;
    std::uint32_t length;
};

// Packed packet headers of one PPM or PPT marker, as read (Zppm or Zppt, and the bytes after it).
struct PackedChunk {
    std::uint8_t z;
    std::uint32_t size;
    const std::uint8_t* data;
};

template <class T>
struct CompValue {
    std::uint16_t comp;
    T value;
};

// The markers of one tile's headers (over all its tile-parts). Only tiles whose headers carry COD,
// COC, QCD, QCC, RGN, POC or PPT get one. The COC, QCC and RGN markers are kept in the order they
// came in, each added in one step: when a component has several, the last one counts (resolved by
// resolve_tile and resolve_tile_component).
struct TileOverrides {
    std::uint16_t tile;
    bool has_cod;
    TileCoding cod;
    CodingStyle cod_style;
    const Quantization* qcd;
    ArenaList<CompValue<const CodingStyle*>> coc;
    ArenaList<CompValue<const Quantization*>> qcc;
    ArenaList<CompValue<std::uint8_t>> rgn;  // roishift
    ArenaList<PocEntry> poc;
    ArenaList<PackedChunk> ppt;
};

// The tiles whose headers carry markers of their own, by tile index: pages of 128 entries, each made
// when a tile in it first gets markers, under a directory of one entry per 128 tiles SIZ declares (at
// most 4 KiB, made with the first page). The memory follows the tile-part headers read, not the tiles
// SIZ declares (fork audit S2), and a tile is found or added in the same few steps whatever order the
// tile-parts come in.
struct TileDirectory {
    static constexpr std::uint32_t kPageTiles = 128;
    struct Page {
        TileOverrides* tile[kPageTiles];
    };
    Page** pages;             // page_count entries, null until a tile has markers
    std::uint32_t page_count;
    std::uint32_t size;       // tiles with markers of their own

    const TileOverrides* find(std::uint32_t tile) const noexcept
    {
        const Page* page = tile / kPageTiles < page_count ? pages[tile / kPageTiles] : nullptr;
        return page ? page->tile[tile % kPageTiles] : nullptr;
    }
};

struct MainHeader {
    // SIZ
    std::uint16_t rsiz;
    std::uint32_t x0, y0, x1, y1;      // the image area on the reference grid
    std::uint32_t tdx, tdy, tx0, ty0;  // tile size and tile grid origin
    std::uint32_t tw, th;              // tiles across and down
    std::uint32_t numcomps;
    SizComponent* comps;
    // COD and QCD (both required) and the main header's per-component overrides
    TileCoding cod;
    CodingStyle cod_style;
    Quantization qcd;
    const CodingStyle** coc;   // per component, null when there is no main COC
    const Quantization** qcc;  // per component, null when there is no main QCC
    std::int16_t* rgn;         // per component: roishift, or -1 when there is no main RGN
    ArenaList<PocEntry> poc;
    // PPM: the packed packet headers in Zppm order, Nppm fields removed, and the Nppm of each
    // tile-part in turn
    bool has_ppm;
    ArenaList<PackedChunk> ppm_chunks;
    const std::uint8_t* ppm_data;
    std::uint64_t ppm_size;
    ArenaList<std::uint32_t> ppm_lengths;
    // TLM (OpenJPEG warns and ignores an invalid one)
    bool tlm_invalid;
    ArenaList<TlmEntry> tlm;
    bool has_cap;  // Part 15 capabilities (HT)
    // positions in the stream: SOC, and the first SOT marker (the end of the main header)
    std::uint64_t start, end;
    // the fewest resolutions of any component, from the main header
    std::uint32_t min_resolutions;
    // tiles with markers of their own
    TileDirectory tiles;

    std::uint32_t tile_count() const noexcept { return tw * th; }
    const TileOverrides* overrides(std::uint32_t tile) const noexcept { return tiles.find(tile); }
    const CodingStyle& main_style(std::uint32_t comp) const noexcept { return coc[comp] ? *coc[comp] : cod_style; }
    const Quantization& main_quant(std::uint32_t comp) const noexcept { return qcc[comp] ? *qcc[comp] : qcd; }
    std::uint8_t main_roishift(std::uint32_t comp) const noexcept
    {
        return rgn[comp] >= 0 ? std::uint8_t(rgn[comp]) : std::uint8_t(0);
    }
};

// A tile-component's parameters with precedence applied.
struct TileComponentParams {
    const TileCoding* coding;
    const CodingStyle* style;
    const Quantization* quant;
    std::uint8_t roishift;
};

// One tile-part header.
struct TilePartHeader {
    std::uint64_t sot;  // offset of its SOT marker
    std::uint64_t data;  // offset of its first data byte, after SOD
    std::uint32_t psot;  // 0: the tile-part runs to the end of the codestream
    std::uint32_t data_length;  // Psot less the header and SOD, when psot != 0
    std::uint16_t tile;
    std::uint8_t tpsot, tnsot;
};

// What the codec tells the header reader.
struct HeaderSettings {
    nj2k_decoder_limits_t limits;
    bool dump;  // NJ2K_DPARAMETERS_DUMP_FLAG: the JP2 image header is not compared with SIZ
    // Components may differ in precision and sign without a warning: always for a codestream, and
    // in a JP2 file when its image header says so (BPC 255), as OpenJPEG has it
    bool allow_varying_depth = true;
    std::uint32_t ihdr_w, ihdr_h;  // JP2 image header size, 0 when there is none
};

// Reads the main header, from SOC to the first SOT marker (which is consumed). out is set on
// success only. A failure leaves an error message with the handler.
nj2k_status_t read_main_header(Source& src, Arena& arena, const Messages& msg, const HeaderSettings& settings,
                               MainHeader*& out) noexcept;

// Reads a tile-part header from just after its SOT marker (at sot) up to and including its SOD
// marker, and records the tile's markers in mh.tiles.
nj2k_status_t read_tile_part_header(Source& src, Arena& arena, const Messages& msg, const HeaderSettings& settings,
                                    MainHeader& mh, std::uint64_t sot, TilePartHeader& out) noexcept;

// One marker segment of a tile-part header (its marker code m and its body of n bytes), recorded in
// mh.tiles for the tile or checked as read_tile_part_header does: what may not appear there is
// refused, a marker segment the decoder does not know is skipped with a warning (DP-7). The
// decode's walk over the tile-parts calls it for each marker it reads.
nj2k_status_t read_tile_part_marker(Arena& arena, const Messages& msg, const HeaderSettings& settings, MainHeader& mh,
                                    std::uint16_t tile, std::uint16_t m, const std::uint8_t* body,
                                    std::uint32_t n) noexcept;

// Every component's parameters of a tile (out: mh.numcomps entries), with marker precedence applied
// (W7) and each component's step sizes checked against the bands its resolutions need (W8), in one
// pass over the components and the tile's markers. tile may be null for the main header's defaults.
nj2k_status_t resolve_tile(const MainHeader& mh, const TileOverrides* tile, const Messages& msg,
                           TileComponentParams* out) noexcept;

// One tile-component's parameters, as resolve_tile gives them; it looks through the tile's markers
// for the component, so a decode that needs every component takes resolve_tile.
nj2k_status_t resolve_tile_component(const MainHeader& mh, const TileOverrides* tile, std::uint32_t comp,
                                     const Messages& msg, TileComponentParams& out) noexcept;

// The data length of a tile-part with Psot = 0, from the bytes after its SOD: all of them, less
// the EOC marker when the codestream ends with one (plan W5: a cut codestream has none).
inline std::uint64_t psot0_data_length(std::uint64_t bytes_after_sod, bool ends_with_eoc) noexcept
{
    return ends_with_eoc && bytes_after_sod >= 2 ? bytes_after_sod - 2 : bytes_after_sod;
}

// ---- geometry, as OpenJPEG computes it -----------------------------------------------------------

inline std::uint32_t ceildiv(std::uint32_t a, std::uint32_t b) noexcept
{
    return std::uint32_t((std::uint64_t(a) + b - 1) / b);
}
inline std::uint32_t ceildivpow2(std::uint32_t a, std::uint32_t e) noexcept
{
    return e >= 32 ? (a ? 1u : 0u) : std::uint32_t((std::uint64_t(a) + (std::uint64_t(1) << e) - 1) >> e);
}

struct Rect {
    std::uint32_t x0, y0, x1, y1;
};

// The tile's area on the reference grid, clipped to the image.
Rect tile_rect(const MainHeader& mh, std::uint32_t tile) noexcept;

// Component geometry of the image nj2k_read_header() returns (OpenJPEG's
// opj_image_comp_header_update): w and h are the component's extent divided by 2^factor.
void header_component_geometry(const MainHeader& mh, std::uint32_t comp, std::uint32_t factor, std::uint32_t& x0,
                               std::uint32_t& y0, std::uint32_t& w, std::uint32_t& h) noexcept;

// Component geometry at a reduce factor as the decode sets it when the factor comes after the
// header (OpenJPEG's opj_j2k_update_image_dimensions). false when the image coordinates are above
// INT32_MAX, which OpenJPEG does not decode.
bool decoded_component_geometry(const MainHeader& mh, std::uint32_t comp, std::uint32_t factor, std::uint32_t& x0,
                                std::uint32_t& y0, std::uint32_t& w, std::uint32_t& h) noexcept;

// ---- working memory ------------------------------------------------------------------------------

// The memory a decode at this factor needs at most at once, computed from the header before
// anything is allocated: the decoded samples of the components selected (all when selected is
// null), the wavelet's lines and fixed scratch (image_working_memory), and the largest tile's part
// with the main header's coding (tile_working_memory's count): the code-block and precinct state of
// every resolution (full state for the resolutions decoded, header state for the ones dropped), the
// packet iterator's table and, when there are several tiles, its coefficients. Saturates at
// UINT64_MAX. Tile-part headers can change a tile's coding; decoding checks such a tile again with
// its own parameters (image_working_memory + tile_working_memory) before it allocates the tile.
std::uint64_t decode_working_memory(const MainHeader& mh, std::uint32_t factor, const bool* selected) noexcept;

// The part of decode_working_memory that does not depend on the tiles.
std::uint64_t image_working_memory(const MainHeader& mh, std::uint32_t factor, const bool* selected) noexcept;

// One tile's part of the working memory with its own parameters (resolve_tile: mh.numcomps entries).
std::uint64_t tile_working_memory(const MainHeader& mh, std::uint32_t tile, const TileComponentParams* params,
                                  std::uint32_t factor, const bool* selected) noexcept;

}  // namespace nj2k::detail

// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// One tile's decode: its layout (tile-components, resolutions, bands, precincts, code-blocks), its
// packets (Tier-2) and its samples (Tier-1, the inverse wavelet, the colour transform and the DC
// shift). The layout is flat arrays in the decoder's work arena, made from the header model with
// OpenJPEG's geometry (opj_tcd_init_tile), so every packet and every code-block is where OpenJPEG
// has it. Code-blocks of the resolutions the reduce factor drops keep only what their packet
// headers need (segment state, Lblock and tag trees): no coordinates, data chunks or samples.
#pragma once

#include <cstddef>
#include <cstdint>

#include <neoj2k/neoj2k.h>

#include "dwt.h"
#include "header.h"
#include "messages.h"
#include "t1.h"
#include "work_arena.h"

namespace nj2k::detail {

// A code-block's decoding state (opj_tcd_cblk_dec_t).
struct CodeBlock {
    Segment* segs;
    Chunk* chunks;
    std::uint32_t segs_cap, nchunks, chunks_cap;
    std::uint32_t numbps, numlenbits, numsegs, real_num_segs, numnewpasses;
    bool corrupted;
};

// A tag tree's nodes (opj_tgt_node_t without the encoder's 'known'): leaves first, then each level
// up to the root, a node's parent the one at (x / 2, y / 2) of the next level.
struct TagNode {
    std::int32_t value, low;
};
struct TagTree {
    TagNode* nodes;
    std::uint32_t w, h;  // leaves across and down; no tree when either is 0
};

struct Precinct {
    std::int32_t x0, y0, x1, y1;
    std::uint32_t cw, ch;           // code-blocks across and down
    std::int32_t cblk_x0, cblk_y0;  // the code-block grid's origin (tlcblkxstart, tlcblkystart)
    CodeBlock* cblks;
    TagTree incl, imsb;
};

struct Band {
    std::int32_t x0, y0, x1, y1;
    std::uint32_t bandno;  // 0 for resolution 0, else 1 HL, 2 LH, 3 HH
    std::int32_t numbps;
    float stepsize;
    Precinct* precincts;  // pw x ph of the resolution
    bool empty() const noexcept { return x1 - x0 == 0 || y1 - y0 == 0; }
};

struct Resolution {
    std::int32_t x0, y0, x1, y1;
    std::uint32_t pw, ph, numbands;
    std::uint32_t pdx, pdy;              // precinct exponents
    std::uint32_t cblkw_expn, cblkh_expn;
    Band bands[3];
};

struct TileComp {
    std::int32_t x0, y0, x1, y1;
    std::uint32_t numresolutions, minimum_num_resolutions;
    Resolution* res;
    TileComponentParams params;
    std::int32_t dc_shift;
    // the samples of the decoded resolution (minimum_num_resolutions - 1), w x h, int32 or float bits
    std::int32_t* data;
    std::uint32_t data_w, data_h;
};

// Packed packet headers being consumed: PPM for the whole codestream, PPT for a tile.
struct PackedCursor {
    const std::uint8_t* data;
    std::uint32_t len;
};

// What a tile's decode needs from the codestream and the codec.
struct TileInput {
    const MainHeader* mh;
    std::uint32_t tile;
    const TileComponentParams* params;  // mh->numcomps entries, precedence applied
    std::uint32_t reduce;               // the reduce factor in effect
    std::uint32_t layers;               // cp_layer: 0 for all
    bool strict;
    const bool* selected;               // the components decoded, null for all
    const std::uint8_t* data;           // the tile's bytes after SOD, every tile-part's in turn
    std::uint32_t size;
    PackedCursor* ppm;                  // null without PPM; consumed across the tiles
    bool has_ppt;                       // the tile's headers carry PPT markers
    PackedCursor ppt;                   // the tile's PPT data, merged in Zppt order
};

class Tile {
public:
    // opj_tcd_init_tile for decoding: false (an error message given) when OpenJPEG refuses the
    // tile's geometry, or E_NO_MEMORY / E_LIMIT through status.
    nj2k_status_t init(const TileInput& in, WorkArena& arena, const Messages& msg) noexcept;

    // Tier-2: every packet in OpenJPEG's order, with the stage 1a cut semantics and the exhaustion
    // exit. resno_decoded receives each component's decoded resolution (opj_image_comp_t).
    nj2k_status_t read_packets(WorkArena& arena, const Messages& msg, std::uint32_t* resno_decoded) noexcept;

    // Tier-1 into each component's buffer (planes: the decoded resolution's samples, allocated by
    // the caller from samples_needed()), the inverse wavelet up to resno_decoded, the colour
    // transform and the DC shift (opj_tcd_decode_tile after Tier-2).
    nj2k_status_t decode_samples(T1& t1, const Allocator& alloc, void* dwt_scratch,
                                 const std::uint32_t* resno_decoded, const Messages& msg) noexcept;

    // Bytes of the decoded resolution's samples of component c (0 when not decoded), and the
    // scratch the wavelet needs for any component.
    std::size_t samples_bytes(std::uint32_t c) const noexcept;
    std::size_t dwt_scratch_bytes_needed() const noexcept;
    // The largest code-block data of the tile, for t1.reserve().
    std::size_t largest_cblk_bytes() const noexcept;

    std::int32_t x0, y0, x1, y1;
    std::uint32_t numcomps = 0;
    TileComp* comps = nullptr;
    const TileInput* in = nullptr;
    PackedCursor ppt{nullptr, 0};  // the PPT headers being consumed
};

}  // namespace nj2k::detail

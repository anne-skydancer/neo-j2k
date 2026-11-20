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
#include "output.h"
#include "parallel.h"
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

// opj_tcd_is_subband_area_of_interest for a whole-tile decode: whether the band region [x0, x1) x
// [y0, y1) of resolution resno (band 0 LL, 1 HL, 2 LH, 3 HH) meets the tile-component, widened by
// the filter's margin, as OpenJPEG computes it. Tier-2 (which packets to read) and Tier-1 (which
// code-blocks to decode) take the same answer.
inline bool area_of_interest(const TileComp& tilec, std::uint32_t resno, std::uint32_t bandno, std::uint32_t x0,
                             std::uint32_t y0, std::uint32_t x1, std::uint32_t y1) noexcept
{
    const std::uint32_t filter_margin = (tilec.params.style->qmfbid == 1) ? 2 : 3;
    const std::uint32_t nb = (resno == 0) ? tilec.numresolutions - 1 : tilec.numresolutions - resno;
    // a tile-component coordinate in the band's (ISO 15444-1 B-15)
    auto band = [nb](std::int32_t coord, std::uint32_t b) {
        const std::uint32_t t = static_cast<std::uint32_t>(coord);
        return (nb == 0) ? t : (t <= (1U << (nb - 1)) * b) ? 0 : ceildivpow2(t - (1U << (nb - 1)) * b, nb);
    };
    const std::uint32_t xb = bandno & 1, yb = bandno >> 1;
    std::uint32_t tbx0 = band(tilec.x0, xb), tby0 = band(tilec.y0, yb), tbx1 = band(tilec.x1, xb),
                  tby1 = band(tilec.y1, yb);
    tbx0 = tbx0 < filter_margin ? 0 : tbx0 - filter_margin;
    tby0 = tby0 < filter_margin ? 0 : tby0 - filter_margin;
    tbx1 = tbx1 > UINT32_MAX - filter_margin ? UINT32_MAX : tbx1 + filter_margin;  // opj_uint_adds
    tby1 = tby1 > UINT32_MAX - filter_margin ? UINT32_MAX : tby1 + filter_margin;
    return x0 < tbx1 && y0 < tby1 && x1 > tbx0 && y1 > tby0;
}

// Packed packet headers being consumed: PPM for the whole codestream, PPT for a tile.
struct PackedCursor {
    const std::uint8_t* data;
    std::uint32_t len;
};

// What a tile's decode needs to run through the host's executor (stage 5, parallel.h). Tier-1: the
// code-blocks in OpenJPEG's order cut into runs of about equal cost (Tile::plan_t1, on the calling
// thread, before anything is decoded), run p being blocks [bounds[p], bounds[p + 1]) and decoded with
// t1[p], a scratch ready and with room for run_bytes[p], the largest block of that run (so the parts'
// room for code-block bytes adds up to at most the tile's coded bytes); refused says that the walk
// stopped at a block OpenJPEG refuses (refused_block), whose warning follows the runs. The wavelet's
// large levels are cut into jobs->width parts (the scratch sized by dwt_scratch_bytes_needed(width)).
struct TileJobs {
    const Jobs* jobs = nullptr;
    T1* t1 = nullptr;
    std::uint32_t runs = 0;
    // (left unset until plan_t1 fills them: a tile on one thread never touches them)
    std::uint32_t bounds[2 * kMaxJobs + 1];
    std::size_t run_bytes[2 * kMaxJobs];
    bool refused = false;
    T1Block refused_block{};
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
    bool cut;                           // the bytes ended before the tile-parts' declared lengths
    PackedCursor* ppm;                  // null without PPM; consumed across the tiles
    bool has_ppt;                       // the tile's headers carry PPT markers
    PackedCursor ppt;                   // the tile's PPT data, merged in Zppt order
};

class Tile {
public:
    // opj_tcd_init_tile for decoding: NJ2K_OK, or the status of what refuses the tile (an error
    // message given): E_UNSUPPORTED or E_LIMIT for a geometry OpenJPEG refuses, E_LIMIT for a
    // component with no more resolutions than the reduce factor, E_NO_MEMORY or E_LIMIT when the
    // work arena refuses.
    nj2k_status_t init(const TileInput& in, WorkArena& arena, const Messages& msg) noexcept;

    // Tier-2: every packet in OpenJPEG's order, with the stage 1a cut semantics and the exhaustion
    // exit. resno_decoded receives each component's decoded resolution (opj_image_comp_t).
    nj2k_status_t read_packets(WorkArena& arena, const Messages& msg, std::uint32_t* resno_decoded) noexcept;

    // Tier-1 into each component's buffer (TileComp::data: the decoded resolution's samples, which
    // the caller attaches, samples_bytes() each), the inverse wavelet up to resno_decoded, and the
    // colour transform's checks (opj_tcd_decode_tile after Tier-2): mct says which transform the
    // samples still need. The transform and the DC shift (finish_of) are the output pass's
    // (output.h), in place or on the way into the image.
    // With tj, Tier-1 and the wavelet's large levels run through the host's executor, with the same
    // samples, status and messages.
    nj2k_status_t decode_samples(T1& t1, void* dwt_scratch, const std::uint32_t* resno_decoded,
                                 const Messages& msg, const TileJobs* tj = nullptr) noexcept;
    // Component compno's DC shift (opj_tcd_dc_level_shift_decode's parameters).
    Finish finish_of(std::uint32_t compno) const noexcept;

    // Bytes of the decoded resolution's samples of component c (0 when not decoded), and the
    // scratch the wavelet needs for any component.
    std::size_t samples_bytes(std::uint32_t c) const noexcept;
    // parts: the wavelet's threaded levels' parts (0: one thread)
    std::size_t dwt_scratch_bytes_needed(std::uint32_t parts = 0) const noexcept;
    // The room a threaded decode's code-block costs need (plan_t1's cost), and the decoded
    // samples of the components decoded, which say whether the tile is large enough to split.
    std::size_t cblk_costs_bytes() const noexcept;
    std::uint64_t decoded_samples() const noexcept;
    // Tier-1's runs for at most max_runs parts (tj.runs, bounds, run_bytes, refused), from the
    // packet headers alone; cost has room for cblk_costs_bytes().
    void plan_t1(TileJobs& tj, std::uint32_t max_runs, void* cost) noexcept;
    // The largest code-block data of the tile, for t1.reserve().
    std::size_t largest_cblk_bytes() const noexcept;

    std::int32_t x0, y0, x1, y1;
    std::uint32_t numcomps = 0;
    TileComp* comps = nullptr;
    const TileInput* in = nullptr;
    PackedCursor ppt{nullptr, 0};  // the PPT headers being consumed
    Mct mct = Mct::kNone;          // the colour transform decode_samples left to the output pass
};

}  // namespace nj2k::detail

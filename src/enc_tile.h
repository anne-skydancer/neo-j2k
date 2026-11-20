// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The encoder's tile model (stage 6a): the structures OpenJPEG's tcd.c builds for one tile of the
// settings the Second Life viewer encodes with (one tile, one precinct per resolution, 64 x 64
// code-blocks, no mode switches), and the stages that work on it: the forward wavelet
// (enc_dwt.cpp), Tier-1 (enc_t1.cpp) and Tier-2 with the rate allocation (enc_t2.cpp). Ported from
// OpenJPEG 2.5.4 at the oracle's pin (oracle/PIN) with the same operations, so the codestream is
// byte for byte OpenJPEG's; the interface is neo-j2k's. Their notice is in enc_t1.cpp.
#pragma once

#include <cstddef>
#include <cstdint>

#include "arena.h"
#include "dwt.h"
#include "memory.h"
#include "messages.h"

namespace nj2k::detail {

// ---- the model -----------------------------------------------------------------------------

struct EncPass {
    std::uint32_t rate;  // bytes up to and including this pass
    std::uint32_t len;   // bytes of this pass
    double distortiondec;
    std::uint8_t term;
};

struct EncLayer {
    std::uint32_t numpasses;
    std::uint32_t len;
    double disto;
    const std::uint8_t* data;
};

struct EncCblk {
    std::int32_t x0, y0, x1, y1;  // in the band's coordinates
    std::uint32_t numbps;         // OpenJPEG wraps it for blocks whose largest sample is under one step
    std::uint32_t numlenbits;
    std::uint32_t numpasses;      // passes written so far by Tier-2
    std::uint32_t numpassesinlayers;
    std::uint32_t totalpasses;
    std::uint8_t* data;
    EncPass* passes;
    EncLayer* layers;
};

struct TagNode {
    TagNode* parent;
    std::int32_t value;
    std::int32_t low;
    std::uint32_t known;
};

struct TagTree {
    std::uint32_t numleafsh, numleafsv, numnodes;
    TagNode* nodes;
};

// A band with its one precinct (the viewer's streams have precincts of 2^15 samples, larger than
// every side the encoder takes).
struct EncBand {
    std::uint32_t bandno;
    std::int32_t x0, y0, x1, y1;
    float stepsize;
    std::int32_t numbps;
    std::uint32_t cw, ch;  // code-blocks of the precinct
    EncCblk* cblks;
    TagTree incltree, imsbtree;
    bool empty() const noexcept { return x1 - x0 == 0 || y1 - y0 == 0; }
};

struct EncRes {
    std::int32_t x0, y0, x1, y1;
    std::uint32_t numbands;
    EncBand bands[3];
};

struct EncComp {
    std::int32_t x0, y0, x1, y1;
    std::uint32_t numres;
    EncRes* res;
    void* data;  // w x h samples, int32 (5/3) or float (9/7)
};

struct EncStep {
    std::int32_t expn, mant;
};

struct EncTile {
    std::uint32_t numcomps;
    EncComp* comps;
    double distotile;
};

// the coding parameters of one tile, every component alike
struct EncParams {
    std::uint32_t numcomps;
    std::uint32_t prec;
    std::uint32_t numres;
    std::uint32_t numlayers;
    std::uint32_t numgbits;
    bool reversible;  // qmfbid 1
    std::uint32_t mct;  // 0 or 1
    std::uint32_t dc_shift;
    EncStep steps[3 * 33 - 2];
    float rates[100];  // the layers' targets after update_rates, 0 for lossless
};

// ---- the stages ----------------------------------------------------------------------------

// Tile-component sizes of the resolutions, in OpenJPEG's order.
void enc_resolution_rects(std::uint32_t w, std::uint32_t h, std::uint32_t numres, ResRect* out) noexcept;

// Forward wavelet in place (the quadrant layout of OpenJPEG), resolutions from the highest down.
// scratch needs enc_dwt_scratch_bytes(). False when the allocator ran out (never: scratch is given).
std::size_t enc_dwt_scratch_bytes(const ResRect* res, std::uint32_t numres) noexcept;
void enc_dwt_53(std::int32_t* data, std::uint32_t w, const ResRect* res, std::uint32_t numres, void* scratch) noexcept;
void enc_dwt_97(float* data, std::uint32_t w, const ResRect* res, std::uint32_t numres, void* scratch) noexcept;

// The step sizes of opj_dwt_calc_explicit_stepsizes (QCD).
void enc_calc_stepsizes(std::uint32_t numres, std::uint32_t prec, bool reversible, EncStep* out) noexcept;

// Forward colour transforms of the first three components.
void enc_rct(std::int32_t* c0, std::int32_t* c1, std::int32_t* c2, std::size_t n) noexcept;
void enc_ict(float* c0, float* c1, float* c2, std::size_t n) noexcept;

// The norms of the transforms (opj_dwt_getnorm, opj_dwt_getnorm_real, opj_mct_norms).
double enc_dwt_norm(std::uint32_t level, std::uint32_t orient) noexcept;
double enc_dwt_norm_real(std::uint32_t level, std::uint32_t orient) noexcept;
extern const double kEncMctNorms[3];
extern const double kEncMctNormsReal[3];

// Tier-1: one code-block's scratch, kept by the encoder between code-blocks.
struct EncT1;
EncT1* enc_t1_create(const Allocator& alloc) noexcept;
void enc_t1_destroy(EncT1* t1, const Allocator& alloc) noexcept;
// The 64 x 64 code-block's samples, in the order of the coding passes, are written by the caller
// through enc_t1_data() (w x h, four rows at a time, column by column) and coded here: the passes
// go to cblk->passes (from arena) and the bytes to cblk->data; returns the code-block's distortion
// (the tile's distotile grows by it). False when the arena runs out or the code-block's output
// does not fit its buffer.
std::int32_t* enc_t1_data(EncT1* t1) noexcept;
struct EncT1Result {
    double cumwmsedec;
    bool ok;
};
EncT1Result enc_t1_encode(EncT1* t1, Arena& arena, EncCblk& cblk, std::uint32_t orient, std::uint32_t compno,
                          std::uint32_t level, bool reversible, double stepsize, const double* mct_norms,
                          std::uint32_t mct_numcomps) noexcept;

// Tier-2 and the rate allocation: layers from the passes (opj_tcd_rateallocate), then the packets.
// len is the room in dest (OpenJPEG's tile buffer after SOT and SOD); false when it does not fit
// (messages through msg) or the arena runs out.
bool enc_rate_allocate_and_packets(Arena& arena, const Messages& msg, EncTile& tile, const EncParams& p,
                                   std::uint8_t* dest, std::uint32_t len, std::uint32_t* written) noexcept;

// the tag tree of opj_tgt_create, for numleafsh x numleafsv leaves; false when the arena runs out
bool enc_tgt_create(Arena& arena, TagTree& tree, std::uint32_t h, std::uint32_t v) noexcept;

}  // namespace nj2k::detail

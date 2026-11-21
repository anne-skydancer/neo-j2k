// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)

#pragma once

#include <openjpeg.h>

#include "common.h"

namespace nj2k::test {

enum class Packed { None, Ppm, Ppt };

struct GeneratedSpec {
    std::string name;
    int w = 32, h = 32, comps = 3, prec = 8;
    bool sgnd = false;
    int sub = 1;              // dx = dy of components 1 and 2 (4:2:0 style when 2)
    int x0 = 0, y0 = 0;       // image origin on the reference grid
    int tw = 0, th = 0;       // tile size, 0: one tile
    int tx0 = 0, ty0 = 0;     // tile grid origin
    bool irreversible = false;
    int mct = -1;             // -1: on for 3+ components with equal sizes
    int numres = 6;
    std::vector<float> rates;  // per layer, compression ratios; 0 last for lossless
    int cbw = 64, cbh = 64;
    std::vector<int> precincts;  // log2 size per resolution, coarsest first; empty: none
    OPJ_PROG_ORDER prog = OPJ_LRCP;
    int mode = 0;  // code-block style: 1 BYPASS, 2 RESET, 4 TERMALL, 8 VSC, 16 PTERM, 32 SEGMARK
    bool sop = false, eph = false, plt = false, tlm = false;
    bool poc = false;
    Packed packed = Packed::None;
    bool packed_keeps_markers = false;  // PPM/PPT that keep SOP and EPH
    bool oracle_refuses = false;        // an edge case the oracle refuses (the rewrite is checked on its source)
    int psot0 = 0;                      // 1: the last tile-part's Psot set to 0; 2: and the EOC dropped
    unsigned seed = 1;
    int texture = 24;  // texture amplitude, in 8-bit steps
};

// Encode synthetic samples with the pinned OpenJPEG encoder. The corpus tool can request SOP/EPH
// independently of the spec when rewriting packet headers.
bool encode_generated(const GeneratedSpec& spec, bool sop, bool eph, Bytes& out, std::string& error);

// One bounded combination in the native decoder's generated-feature matrix.
GeneratedSpec generated_case(std::uint32_t seed, std::uint32_t index);

}  // namespace nj2k::test

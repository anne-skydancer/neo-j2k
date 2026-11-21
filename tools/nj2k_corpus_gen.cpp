// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// nj2k_corpus_gen: writes the generated test corpus (corpus/generated) with the oracle's encoder.
//
//   nj2k_corpus_gen --out DIR       write every codestream and MANIFEST.tsv
//   nj2k_corpus_gen --verify DIR    encode again in memory and compare with the files (exit 1 on a
//                                   difference): the encoder's output is the same on every platform
//
// The images are synthetic and deterministic (gradients plus seeded texture). The set covers what
// the decoder must reproduce and what Second Life never sends: sizes from 1x1 to 4096, 1 to 5
// components, 8/12/16-bit and signed samples, subsampling, 5/3 and 9/7, all five progressions,
// 1 to 8 layers, 1 to 6 resolutions, image and tile origins, tiles, precincts, code-block sizes,
// SOP, EPH, every mode switch, PLT, TLM, POC, and what OpenJPEG's encoder cannot write, rewritten
// here: packed packet headers (PPM, PPT) from SOP+EPH streams, and a last tile-part of Psot = 0
// (it runs to the end of the codestream), with and without EOC. A rewrite is checked to decode
// exactly as its source at every reduce level.
#include <openjpeg.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "common.h"
#include "decoder.h"
#include "j2k_scan.h"

using namespace nj2k::test;

namespace {

enum class Packed { None, Ppm, Ppt };

struct Spec {
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

std::vector<Spec> specs()
{
    std::vector<Spec> v;
    auto add = [&](Spec s) { v.push_back(std::move(s)); };
    auto rates = [](std::initializer_list<float> r) { return std::vector<float>(r); };

    // sizes, components and resolutions
    add({.name = "size_1x1_g", .w = 1, .h = 1, .comps = 1, .numres = 1});
    add({.name = "size_1x1_rgb", .w = 1, .h = 1, .comps = 3, .numres = 1});
    add({.name = "size_2x2_rgba_97", .w = 2, .h = 2, .comps = 4, .irreversible = true, .numres = 2, .rates = rates({4})});
    add({.name = "size_3x5_g", .w = 3, .h = 5, .comps = 1, .numres = 2});
    add({.name = "size_17x37_rgb_97", .w = 17, .h = 37, .comps = 3, .irreversible = true, .numres = 4,
         .rates = rates({20, 8, 3})});
    add({.name = "size_1x64_g", .w = 1, .h = 64, .comps = 1, .numres = 1});
    add({.name = "size_64x1_g", .w = 64, .h = 1, .comps = 1, .numres = 1});
    add({.name = "size_4x4_rgb_r3", .w = 4, .h = 4, .comps = 3, .numres = 3});
    add({.name = "size_16x16_rgb_r5", .w = 16, .h = 16, .comps = 3, .numres = 5, .rates = rates({8, 0})});
    add({.name = "size_512x16_rgb_97_r5", .w = 512, .h = 16, .comps = 3, .irreversible = true, .numres = 5,
         .rates = rates({40, 10})});
    add({.name = "size_7x9_rgb_r1", .w = 7, .h = 9, .comps = 3, .numres = 1});
    add({.name = "sl_32_rgb_97", .w = 32, .h = 32, .comps = 3, .irreversible = true, .numres = 6,
         .rates = rates({256, 64, 16, 8, 4})});
    add({.name = "sl_32_rgba_53", .w = 32, .h = 32, .comps = 4, .numres = 6, .rates = rates({0})});
    add({.name = "sl_64_rgb_97", .w = 64, .h = 64, .comps = 3, .irreversible = true, .numres = 6,
         .rates = rates({512, 128, 32, 16, 8}), .texture = 12});
    add({.name = "sl_64_bake5_97", .w = 64, .h = 64, .comps = 5, .irreversible = true, .mct = 1, .numres = 6,
         .rates = rates({64, 16, 8}), .texture = 10});
    add({.name = "sl_128_rgba_97", .w = 128, .h = 128, .comps = 4, .irreversible = true, .numres = 6,
         .rates = rates({1024, 256, 64, 32}), .texture = 8});

    // progressions, with precincts so that the orders differ
    const std::vector<int> prec5 = {4, 4, 4, 5, 5};
    const std::pair<const char*, OPJ_PROG_ORDER> progs[] = {
        {"prog_lrcp", OPJ_LRCP}, {"prog_rlcp", OPJ_RLCP}, {"prog_rpcl", OPJ_RPCL}, {"prog_pcrl", OPJ_PCRL},
        {"prog_cprl", OPJ_CPRL}};
    for (const auto& [name, order] : progs) {
        add({.name = name, .w = 48, .h = 40, .comps = 3, .numres = 5, .rates = rates({16, 4, 0}), .cbw = 16,
             .cbh = 16, .precincts = prec5, .prog = order, .texture = 16});
    }

    // layers
    add({.name = "layers_1_97", .w = 48, .h = 48, .comps = 3, .irreversible = true, .numres = 5, .rates = rates({6})});
    add({.name = "layers_2_97", .w = 48, .h = 48, .comps = 3, .irreversible = true, .numres = 5,
         .rates = rates({24, 6})});
    add({.name = "layers_4_53", .w = 48, .h = 48, .comps = 1, .numres = 5, .rates = rates({32, 16, 4, 0}),
         .sop = true});
    add({.name = "layers_8_97", .w = 48, .h = 48, .comps = 3, .irreversible = true, .numres = 5,
         .rates = rates({512, 256, 128, 64, 32, 16, 8, 4}), .sop = true});

    // packet markers and mode switches
    const std::vector<float> r3 = {24, 8, 0};
    add({.name = "mark_sop", .w = 40, .h = 32, .comps = 3, .numres = 4, .rates = r3, .sop = true});
    add({.name = "mark_eph", .w = 40, .h = 32, .comps = 3, .numres = 4, .rates = r3, .eph = true});
    add({.name = "mark_sop_eph_97", .w = 40, .h = 32, .comps = 3, .irreversible = true, .numres = 4,
         .rates = rates({24, 8, 4}), .sop = true, .eph = true});
    add({.name = "mode_bypass", .w = 40, .h = 32, .comps = 3, .numres = 4, .rates = r3, .mode = 1, .sop = true});
    add({.name = "mode_termall", .w = 40, .h = 32, .comps = 3, .numres = 4, .rates = r3, .mode = 4, .sop = true});
    add({.name = "mode_bypass_termall", .w = 40, .h = 32, .comps = 3, .numres = 4, .rates = r3, .mode = 5,
         .sop = true});
    add({.name = "mode_segmark_vsc", .w = 40, .h = 32, .comps = 3, .numres = 4, .rates = r3, .mode = 40});
    add({.name = "mode_reset_pterm", .w = 40, .h = 32, .comps = 3, .numres = 4, .rates = r3, .mode = 18});
    add({.name = "mode_all_97", .w = 40, .h = 32, .comps = 3, .irreversible = true, .numres = 4,
         .rates = rates({24, 8, 4}), .mode = 63, .sop = true});
    add({.name = "mark_plt", .w = 48, .h = 40, .comps = 3, .numres = 5, .rates = r3, .plt = true});
    add({.name = "mark_tlm_tiles", .w = 48, .h = 40, .comps = 3, .tw = 32, .th = 16, .numres = 4, .rates = r3,
         .tlm = true});
    add({.name = "mark_plt_tlm_97", .w = 48, .h = 40, .comps = 3, .irreversible = true, .numres = 5,
         .rates = rates({24, 8, 4}), .plt = true, .tlm = true});

    // tiles and origins
    add({.name = "tile_100x80_t64_97", .w = 100, .h = 80, .comps = 3, .tw = 64, .th = 64, .irreversible = true,
         .numres = 5, .rates = rates({32, 8}), .texture = 12});
    add({.name = "tile_origin_61x47", .w = 61, .h = 47, .comps = 3, .x0 = 3, .y0 = 5, .tw = 32, .th = 32,
         .tx0 = 1, .ty0 = 2, .numres = 4, .rates = r3});
    add({.name = "tile_origin_odd_97", .w = 37, .h = 29, .comps = 1, .x0 = 11, .y0 = 7, .irreversible = true,
         .numres = 4, .rates = rates({8, 3})});
    add({.name = "tile_64_of_16", .w = 128, .h = 128, .comps = 1, .tw = 16, .th = 16, .numres = 3,
         .rates = rates({8}), .texture = 6});

    // progression order changes
    add({.name = "poc_lrcp_rlcp", .w = 64, .h = 48, .comps = 3, .numres = 5, .rates = rates({16, 4, 0}), .cbw = 16,
         .cbh = 16, .poc = true, .texture = 12});
    add({.name = "poc_sop_97", .w = 64, .h = 48, .comps = 3, .irreversible = true, .numres = 5,
         .rates = rates({32, 8, 4}), .cbw = 16, .cbh = 16, .sop = true, .poc = true, .texture = 12});

    // packed packet headers
    add({.name = "packed_ppm", .w = 48, .h = 40, .comps = 3, .numres = 5, .rates = r3, .packed = Packed::Ppm});
    add({.name = "packed_ppt", .w = 48, .h = 40, .comps = 3, .numres = 5, .rates = r3, .packed = Packed::Ppt});
    add({.name = "packed_ppm_sop_eph_97", .w = 48, .h = 40, .comps = 3, .irreversible = true, .numres = 5,
         .rates = rates({24, 8, 4}), .packed = Packed::Ppm, .packed_keeps_markers = true});
    add({.name = "packed_ppt_tiles", .w = 64, .h = 48, .comps = 3, .tw = 32, .th = 32, .numres = 4, .rates = r3,
         .packed = Packed::Ppt});
    add({.name = "packed_ppm_tiles_97", .w = 64, .h = 48, .comps = 3, .tw = 32, .th = 32, .irreversible = true,
         .numres = 4, .rates = rates({8, 3}), .packed = Packed::Ppm});
    // Two tiles get no coded data at these rates, so with packed headers their tile-parts hold no
    // bytes at all. That is valid JPEG 2000, but OpenJPEG 2.5.4 refuses such a tile
    // (opj_j2k_decode_tile: no tile data): kept as an edge case, its outcome recorded
    // (doc/departures.md, candidate DP-3).
    add({.name = "edge_ppt_empty_tiles_97", .w = 64, .h = 48, .comps = 3, .tw = 32, .th = 32, .irreversible = true,
         .numres = 4, .rates = rates({24, 8}), .packed = Packed::Ppt, .oracle_refuses = true});

    // a last tile-part of Psot = 0, which runs to the end of the codestream (ISO 15444-1 A.4.2),
    // with EOC and without (a cut stream ends so too): one tile with SOP and eight layers, and four
    // tiles (only the last tile-part may have Psot = 0)
    add({.name = "psot0_sop_97", .w = 48, .h = 48, .comps = 3, .irreversible = true, .numres = 5,
         .rates = rates({512, 256, 128, 64, 32, 16, 8, 4}), .sop = true, .psot0 = 1});
    add({.name = "psot0_sop_noeoc_97", .w = 48, .h = 48, .comps = 3, .irreversible = true, .numres = 5,
         .rates = rates({512, 256, 128, 64, 32, 16, 8, 4}), .sop = true, .psot0 = 2});
    add({.name = "psot0_tiles_97", .w = 100, .h = 80, .comps = 3, .tw = 64, .th = 64, .irreversible = true,
         .numres = 5, .rates = rates({32, 8}), .psot0 = 1, .texture = 12});
    add({.name = "psot0_tiles_noeoc_97", .w = 100, .h = 80, .comps = 3, .tw = 64, .th = 64, .irreversible = true,
         .numres = 5, .rates = rates({32, 8}), .psot0 = 2, .texture = 12});

    // precision, sign, subsampling, code-block sizes
    add({.name = "prec12_g", .w = 40, .h = 40, .comps = 1, .prec = 12, .numres = 4});
    add({.name = "prec16_g_97", .w = 40, .h = 40, .comps = 1, .prec = 16, .irreversible = true, .numres = 4,
         .rates = rates({8, 3})});
    add({.name = "sgnd8_rgb", .w = 40, .h = 40, .comps = 3, .sgnd = true, .mct = 0, .numres = 4});
    add({.name = "sub_420", .w = 48, .h = 40, .comps = 3, .sub = 2, .numres = 4, .rates = r3});
    add({.name = "mix_odd_sub420_signed12_53", .w = 36, .h = 31, .comps = 3, .prec = 12, .sgnd = true,
         .sub = 2, .x0 = 3, .y0 = 5, .tw = 19, .th = 17, .tx0 = 1, .ty0 = 2, .numres = 3,
         .rates = {16, 4, 0}, .cbw = 8, .cbh = 4, .precincts = {3, 3, 4}, .prog = OPJ_RPCL});
    add({.name = "mix_odd_sub420_signed12_97", .w = 43, .h = 38, .comps = 3, .prec = 12, .sgnd = true,
         .sub = 2, .x0 = 5, .y0 = 7, .irreversible = true, .numres = 3,
         .rates = {24, 8, 3}, .cbw = 4, .cbh = 8, .precincts = {3, 3, 4}, .prog = OPJ_CPRL});
    add({.name = "cblk_4x4", .w = 32, .h = 32, .comps = 3, .numres = 4, .rates = r3, .cbw = 4, .cbh = 4});
    add({.name = "cblk_32x16_97", .w = 64, .h = 48, .comps = 3, .irreversible = true, .numres = 5,
         .rates = rates({16, 6}), .cbw = 32, .cbh = 16});

    // large
    add({.name = "large_1024_rgb_97", .w = 1024, .h = 1024, .comps = 3, .irreversible = true, .numres = 6,
         .rates = rates({1600, 800, 400, 200, 100}), .texture = 6});
    add({.name = "large_2048x64_rgba_53", .w = 2048, .h = 64, .comps = 4, .numres = 6, .rates = rates({200, 50}),
         .texture = 4});
    add({.name = "large_4096_g_97", .w = 4096, .h = 4096, .comps = 1, .irreversible = true, .numres = 6,
         .rates = rates({3200, 800}), .texture = 3});
    add({.name = "large_4096x8_rgb_97", .w = 4096, .h = 8, .comps = 3, .irreversible = true, .numres = 4,
         .rates = rates({64, 16})});
    return v;
}

// ---- synthetic images -------------------------------------------------------------------------------

struct Rng {
    std::uint64_t s;
    std::uint32_t next()
    {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return std::uint32_t(s >> 11);
    }
};

int ceil_div(int a, int b)
{
    return (a + b - 1) / b;
}

opj_image_t* make_image(const Spec& s)
{
    std::vector<opj_image_cmptparm_t> cp(std::size_t(s.comps));
    const int x1 = s.x0 + s.w, y1 = s.y0 + s.h;
    for (int c = 0; c < s.comps; ++c) {
        auto& p = cp[std::size_t(c)];
        std::memset(&p, 0, sizeof(p));
        const int d = (c == 1 || c == 2) ? s.sub : 1;
        p.dx = p.dy = OPJ_UINT32(d);
        p.x0 = OPJ_UINT32(ceil_div(s.x0, d));
        p.y0 = OPJ_UINT32(ceil_div(s.y0, d));
        p.w = OPJ_UINT32(ceil_div(x1, d) - ceil_div(s.x0, d));
        p.h = OPJ_UINT32(ceil_div(y1, d) - ceil_div(s.y0, d));
        p.prec = OPJ_UINT32(s.prec);
        p.sgnd = s.sgnd ? 1 : 0;
    }
    const OPJ_COLOR_SPACE cs = s.comps >= 3 ? OPJ_CLRSPC_SRGB : OPJ_CLRSPC_GRAY;
    opj_image_t* img = opj_image_create(OPJ_UINT32(s.comps), cp.data(), cs);
    if (!img) {
        return nullptr;
    }
    img->x0 = OPJ_UINT32(s.x0);
    img->y0 = OPJ_UINT32(s.y0);
    img->x1 = OPJ_UINT32(x1);
    img->y1 = OPJ_UINT32(y1);
    // integer arithmetic only, so every platform makes the same image
    Fnv64 seed;
    seed.bytes(s.name.data(), s.name.size());
    seed.u32(s.seed);
    Rng rng{seed.value() | 1};
    const std::int64_t maxval = (std::int64_t(1) << s.prec) - 1;
    for (int c = 0; c < s.comps; ++c) {
        const auto& comp = img->comps[c];
        for (OPJ_UINT32 y = 0; y < comp.h; ++y) {
            for (OPJ_UINT32 x = 0; x < comp.w; ++x) {
                // two triangle waves that differ per component, an interference term, seeded texture
                const std::int64_t fx = comp.w > 1 ? std::int64_t(x) * 1024 / (comp.w - 1) : 512;
                const std::int64_t fy = comp.h > 1 ? std::int64_t(y) * 1024 / (comp.h - 1) : 512;
                const std::int64_t t1 = (fx * 3 + fy * 5 + c * 400) % 2048, t2 = (fx * 7 - fy * 2 + 4096 + c * 900) % 2048;
                const std::int64_t tri1 = t1 < 1024 ? t1 : 2047 - t1, tri2 = t2 < 1024 ? t2 : 2047 - t2;
                std::int64_t v = 20 + (tri1 * 150 >> 10) + (tri2 * 60 >> 10) + ((fx * fy >> 12) & 15);
                v += std::int64_t(rng.next() % unsigned(2 * s.texture + 1)) - s.texture;
                v = std::clamp<std::int64_t>(v, 0, 255) * maxval / 255;
                if (s.sgnd) {
                    v -= std::int64_t(1) << (s.prec - 1);
                }
                comp.data[std::size_t(y) * comp.w + x] = OPJ_INT32(v);
            }
        }
    }
    return img;
}

// ---- encoding ------------------------------------------------------------------------------------------

struct Sink {
    Bytes* out;
    std::size_t pos = 0;
};

OPJ_SIZE_T sink_write(void* buf, OPJ_SIZE_T n, void* user)
{
    auto* s = static_cast<Sink*>(user);
    if (s->pos + n > s->out->size()) {
        s->out->resize(s->pos + n);
    }
    std::memcpy(s->out->data() + s->pos, buf, n);
    s->pos += n;
    return n;
}

OPJ_OFF_T sink_skip(OPJ_OFF_T n, void* user)
{
    auto* s = static_cast<Sink*>(user);
    s->pos = std::size_t(OPJ_OFF_T(s->pos) + n);
    if (s->pos > s->out->size()) {
        s->out->resize(s->pos);
    }
    return n;
}

OPJ_BOOL sink_seek(OPJ_OFF_T to, void* user)
{
    auto* s = static_cast<Sink*>(user);
    s->pos = std::size_t(to);
    if (s->pos > s->out->size()) {
        s->out->resize(s->pos);
    }
    return OPJ_TRUE;
}

void on_message(const char* msg, void* user)
{
    if (user) {
        *static_cast<std::string*>(user) += msg;
    }
}

bool encode(const Spec& s, bool sop, bool eph, Bytes& out, std::string& err)
{
    opj_image_t* img = make_image(s);
    if (!img) {
        err = "image";
        return false;
    }
    opj_cparameters_t p;
    opj_set_default_encoder_parameters(&p);
    p.numresolution = s.numres;
    p.irreversible = s.irreversible ? 1 : 0;
    p.cblockw_init = s.cbw;
    p.cblockh_init = s.cbh;
    p.prog_order = s.prog;
    p.mode = s.mode;
    const bool equal_comps = s.sub == 1;
    p.tcp_mct = char(s.mct >= 0 ? s.mct : (s.comps >= 3 && equal_comps ? 1 : 0));
    if (s.rates.empty()) {
        p.tcp_numlayers = 1;
        p.tcp_rates[0] = 0;
    } else {
        p.tcp_numlayers = int(s.rates.size());
        for (std::size_t i = 0; i < s.rates.size(); ++i) {
            p.tcp_rates[i] = s.rates[i];
        }
    }
    p.cp_disto_alloc = 1;
    if (sop) {
        p.csty |= 0x02;
    }
    if (eph) {
        p.csty |= 0x04;
    }
    if (!s.precincts.empty()) {
        p.csty |= 0x01;
        p.res_spec = int(s.precincts.size());
        // OpenJPEG lists precinct sizes from the finest resolution down
        for (std::size_t i = 0; i < s.precincts.size(); ++i) {
            const int e = s.precincts[s.precincts.size() - 1 - i];
            p.prcw_init[i] = 1 << e;
            p.prch_init[i] = 1 << e;
        }
    }
    if (s.tw) {
        p.tile_size_on = OPJ_TRUE;
        p.cp_tdx = s.tw;
        p.cp_tdy = s.th;
        p.cp_tx0 = s.tx0;
        p.cp_ty0 = s.ty0;
    }
    if (s.poc) {
        // resolutions 0..2 layer by layer, then the rest resolution by resolution
        const int layers = p.tcp_numlayers;
        p.numpocs = 2;
        p.POC[0].tile = 1;
        p.POC[0].resno0 = 0;
        p.POC[0].compno0 = 0;
        p.POC[0].layno1 = OPJ_UINT32(layers);
        p.POC[0].resno1 = 3;
        p.POC[0].compno1 = OPJ_UINT32(s.comps);
        p.POC[0].prg1 = OPJ_LRCP;
        p.POC[1].tile = 1;
        p.POC[1].resno0 = 0;
        p.POC[1].compno0 = 0;
        p.POC[1].layno1 = OPJ_UINT32(layers);
        p.POC[1].resno1 = OPJ_UINT32(s.numres);
        p.POC[1].compno1 = OPJ_UINT32(s.comps);
        p.POC[1].prg1 = OPJ_RLCP;
    }
    char comment[] = "neo-j2k test corpus";
    p.cp_comment = comment;

    opj_codec_t* codec = opj_create_compress(OPJ_CODEC_J2K);
    opj_set_error_handler(codec, on_message, &err);
    out.clear();
    Sink sink{&out};
    opj_stream_t* st = opj_stream_create(1 << 20, OPJ_FALSE);
    opj_stream_set_user_data(st, &sink, nullptr);
    opj_stream_set_write_function(st, sink_write);
    opj_stream_set_skip_function(st, sink_skip);
    opj_stream_set_seek_function(st, sink_seek);
    bool ok = opj_setup_encoder(codec, &p, img) != 0;
    if (ok && (s.plt || s.tlm)) {
        const char* opts[3] = {nullptr, nullptr, nullptr};
        int n = 0;
        if (s.plt) opts[n++] = "PLT=YES";
        if (s.tlm) opts[n++] = "TLM=YES";
        ok = opj_encoder_set_extra_options(codec, opts) != 0;
    }
    ok = ok && opj_start_compress(codec, img, st) && opj_encode(codec, st) && opj_end_compress(codec, st);
    out.resize(sink.pos);
    opj_stream_destroy(st);
    opj_destroy_codec(codec);
    opj_image_destroy(img);
    return ok;
}

// ---- packed packet headers (PPM, PPT) from a SOP+EPH stream -----------------------------------------

void put16(Bytes& b, std::size_t at, std::uint32_t v)
{
    b[at] = std::uint8_t(v >> 8);
    b[at + 1] = std::uint8_t(v);
}

void append16(Bytes& b, std::uint32_t v)
{
    b.push_back(std::uint8_t(v >> 8));
    b.push_back(std::uint8_t(v));
}

void append32(Bytes& b, std::uint32_t v)
{
    append16(b, v >> 16);
    append16(b, v & 0xFFFF);
}

// Splits every tile-part's data at SOP and EPH, moves the packet headers into PPM (main header) or
// PPT (each tile-part header), rewrites Psot, and (keep_markers false) drops SOP and EPH and clears
// their COD flags.
bool pack_headers(const Bytes& in, Packed how, bool keep_markers, Bytes& out, std::string& err)
{
    const auto mh = parse_main_header(in, &err);
    if (!mh || !(mh->scod & 2) || !(mh->scod & 4)) {
        err = "packing needs SOP and EPH";
        return false;
    }
    const auto tps = scan_tile_parts(in, *mh);
    struct Part {
        Bytes header_markers;  // tile-part header markers between SOT and SOD
        Bytes headers;         // packed packet headers
        Bytes body;            // bitstream
        std::uint16_t tile;
        std::uint8_t tpsot, tnsot;
    };
    std::vector<Part> parts;
    for (const auto& tp : tps) {
        Part part;
        part.tile = tp.tile;
        part.tpsot = tp.tpsot;
        part.tnsot = tp.tnsot;
        part.header_markers.assign(in.begin() + long(tp.sot + 12), in.begin() + long(tp.data - 2));
        std::size_t p = tp.data;
        while (p < tp.end) {
            if (p + 6 > tp.end || in[p] != 0xFF || in[p + 1] != 0x91) {
                err = "packet without SOP";
                return false;
            }
            std::size_t h = p + 6, eph = h;
            while (eph + 1 < tp.end && !(in[eph] == 0xFF && in[eph + 1] == 0x92)) {
                ++eph;
            }
            if (eph + 1 >= tp.end) {
                err = "packet without EPH";
                return false;
            }
            std::size_t next = eph + 2;
            while (next + 1 < tp.end && !(in[next] == 0xFF && in[next + 1] == 0x91)) {
                ++next;
            }
            if (next + 1 >= tp.end) {
                next = tp.end;
            }
            part.headers.insert(part.headers.end(), in.begin() + long(h), in.begin() + long(keep_markers ? eph + 2 : eph));
            if (keep_markers) {
                part.body.insert(part.body.end(), in.begin() + long(p), in.begin() + long(h));
            }
            part.body.insert(part.body.end(), in.begin() + long(eph + 2), in.begin() + long(next));
            p = next;
        }
        parts.push_back(std::move(part));
    }
    // main header, COD flags cleared when the markers go
    Bytes main(in.begin(), in.begin() + long(mh->end));
    if (!keep_markers) {
        for (std::size_t p = 2; p + 4 <= main.size();) {
            const unsigned marker = unsigned(main[p] << 8 | main[p + 1]), len = unsigned(main[p + 2] << 8 | main[p + 3]);
            if (marker == 0xFF52) {
                main[p + 4] = std::uint8_t(main[p + 4] & ~0x06);
            }
            p += 2 + len;
        }
    }
    out = main;
    if (how == Packed::Ppm) {
        Bytes payload;
        for (const auto& part : parts) {
            append32(payload, std::uint32_t(part.headers.size()));
            payload.insert(payload.end(), part.headers.begin(), part.headers.end());
        }
        if (payload.size() + 3 > 65535) {
            err = "PPM payload too large for one segment";
            return false;
        }
        append16(out, 0xFF60);
        append16(out, std::uint32_t(payload.size() + 3));
        out.push_back(0);  // Zppm
        out.insert(out.end(), payload.begin(), payload.end());
    }
    for (const auto& part : parts) {
        const std::size_t sot = out.size();
        append16(out, 0xFF90);
        append16(out, 10);
        append16(out, part.tile);
        append32(out, 0);  // Psot, set below
        out.push_back(part.tpsot);
        out.push_back(part.tnsot);
        out.insert(out.end(), part.header_markers.begin(), part.header_markers.end());
        if (how == Packed::Ppt) {
            if (part.headers.size() + 3 > 65535) {
                err = "PPT payload too large for one segment";
                return false;
            }
            append16(out, 0xFF61);
            append16(out, std::uint32_t(part.headers.size() + 3));
            out.push_back(0);  // Zppt
            out.insert(out.end(), part.headers.begin(), part.headers.end());
        }
        append16(out, 0xFF93);
        out.insert(out.end(), part.body.begin(), part.body.end());
        const std::uint32_t psot = std::uint32_t(out.size() - sot);
        put16(out, sot + 6, psot >> 16);
        put16(out, sot + 8, psot & 0xFFFF);
    }
    append16(out, 0xFFD9);
    return true;
}

// The last tile-part's Psot set to 0, and (drop_eoc) the final EOC dropped.
bool last_psot_zero(const Bytes& in, bool drop_eoc, Bytes& out, std::string& err)
{
    const auto mh = parse_main_header(in, &err);
    if (!mh) {
        return false;
    }
    const auto tps = scan_tile_parts(in, *mh);
    if (tps.empty() || in.size() < 2 || in[in.size() - 2] != 0xFF || in[in.size() - 1] != 0xD9) {
        err = "no tile-part, or no EOC at the end";
        return false;
    }
    out = in;
    const std::size_t sot = tps.back().sot;
    put16(out, sot + 6, 0);
    put16(out, sot + 8, 0);
    if (drop_eoc) {
        out.resize(out.size() - 2);
    }
    return true;
}

// The packed stream must decode exactly as its source, at every reduce level.
bool same_decodes(const Bytes& a, const Bytes& b, int numres, std::string& err)
{
    auto oracle = make_oracle_backend();
    for (int r = 0; r < numres; ++r) {
        Request ra{a.data(), a.size(), r, 0, Mode::Plain, false};
        Request rb{b.data(), b.size(), r, 0, Mode::Plain, false};
        const Outcome x = oracle->decode(ra), y = oracle->decode(rb);
        if (!x.ok || !y.ok || x.hash != y.hash) {
            err = "the rewritten stream decodes differently at reduce " + std::to_string(r) + " (" + y.stage + " " +
                  y.last_error + ")";
            return false;
        }
    }
    return true;
}

bool build(const Spec& s, Bytes& out, std::string& err)
{
    if (s.psot0) {
        // The rewrite must decode as its source, with an EOC and without: OpenJPEG 2.5.4 took a
        // Psot = 0 tile-part to the stream's end less 2 bytes, so without an EOC it dropped the last
        // 2 data bytes (plan W5); the oracle keeps them since the fork's stage 1a.
        Bytes src;
        if (!encode(s, s.sop, s.eph, src, err) || !last_psot_zero(src, s.psot0 == 2, out, err) ||
                !same_decodes(src, out, s.numres, err)) {
            return false;
        }
    } else if (s.packed == Packed::None) {
        if (!encode(s, s.sop, s.eph, out, err)) {
            return false;
        }
    } else {
        Bytes src;
        if (!encode(s, true, true, src, err) || !pack_headers(src, s.packed, s.packed_keeps_markers, out, err)) {
            return false;
        }
        if (s.oracle_refuses) {
            auto oracle = make_oracle_backend();
            const Outcome a = oracle->decode(Request{src.data(), src.size(), 0, 0, Mode::Plain, false});
            const Outcome b = oracle->decode(Request{out.data(), out.size(), 0, 0, Mode::Plain, false});
            if (!a.ok || b.ok) {
                err = "expected the source to decode and the edge case to be refused";
                return false;
            }
            return true;
        }
        if (!same_decodes(src, out, s.numres, err)) {
            return false;
        }
    }
    // every file decodes whole at reduce 0
    auto oracle = make_oracle_backend();
    Request rq{out.data(), out.size(), 0, 0, Mode::Plain, false};
    const Outcome o = oracle->decode(rq);
    if (!o.ok) {
        err = "does not decode: " + o.stage + " " + o.last_error;
        return false;
    }
    return true;
}

const char* prog_name(OPJ_PROG_ORDER p)
{
    switch (p) {
    case OPJ_LRCP: return "LRCP";
    case OPJ_RLCP: return "RLCP";
    case OPJ_RPCL: return "RPCL";
    case OPJ_PCRL: return "PCRL";
    case OPJ_CPRL: return "CPRL";
    default: return "?";
    }
}

}  // namespace

int main(int argc, char** argv)
{
    console_reports();
    (void)single_thread_openjpeg();  // the encoder runs on this thread (OPJ_NUM_THREADS cleared)
    if (argc != 3 || (std::string(argv[1]) != "--out" && std::string(argv[1]) != "--verify")) {
        std::fprintf(stderr, "usage: nj2k_corpus_gen --out DIR | --verify DIR\n");
        return 2;
    }
    const bool verify = std::string(argv[1]) == "--verify";
    const std::string dir = argv[2];
    std::string manifest =
        "# SPDX-License-Identifier: BSD-2-Clause\n"
        "# neo-j2k generated corpus (tools/nj2k_corpus_gen.cpp); every file is the oracle encoder's output\n"
        "# file\tbytes\twidth\theight\tcomponents\tprecision\twavelet\tprogression\tlayers\tresolutions\tfeatures\n";
    int bad = 0, written = 0;
    for (const Spec& s : specs()) {
        Bytes data;
        std::string err;
        if (!build(s, data, err)) {
            std::fprintf(stderr, "FAIL %s: %s\n", s.name.c_str(), err.c_str());
            ++bad;
            continue;
        }
        const std::string file = s.name + ".j2k";
        const std::string path = join_path(dir, file);
        if (verify) {
            const auto have = read_file(path);
            if (!have || *have != data) {
                std::fprintf(stderr, "DIFFERS %s: the encoder gives other bytes on this platform\n", file.c_str());
                ++bad;
            }
        } else if (!write_file(path, data)) {
            std::fprintf(stderr, "FAIL %s: cannot write\n", path.c_str());
            ++bad;
        }
        std::string features;
        auto feat = [&](bool on, const char* f) {
            if (on) {
                features += features.empty() ? f : std::string(",") + f;
            }
        };
        feat(s.tw != 0, "tiles");
        feat(s.x0 || s.y0, "image-origin");
        feat(s.tx0 || s.ty0, "tile-origin");
        feat(!s.precincts.empty(), "precincts");
        feat(s.sop || (s.packed != Packed::None && s.packed_keeps_markers), "SOP");
        feat(s.eph || (s.packed != Packed::None && s.packed_keeps_markers), "EPH");
        feat(s.mode & 1, "BYPASS");
        feat(s.mode & 2, "RESET");
        feat(s.mode & 4, "TERMALL");
        feat(s.mode & 8, "VSC");
        feat(s.mode & 16, "PTERM");
        feat(s.mode & 32, "SEGMARK");
        feat(s.plt, "PLT");
        feat(s.tlm, "TLM");
        feat(s.poc, "POC");
        feat(s.packed == Packed::Ppm, "PPM");
        feat(s.packed == Packed::Ppt, "PPT");
        feat(s.sgnd, "signed");
        feat(s.sub != 1, "subsampled");
        feat(s.cbw != 64 || s.cbh != 64, "code-block-size");
        feat(s.oracle_refuses, "oracle-refuses");
        feat(s.psot0 != 0, "Psot=0");
        feat(s.psot0 == 2, "no-EOC");
        char line[512];
        std::snprintf(line, sizeof(line), "%s\t%zu\t%d\t%d\t%d\t%d\t%s\t%s\t%zu\t%d\t%s\n", file.c_str(), data.size(), s.w,
                      s.h, s.comps, s.prec, s.irreversible ? "9/7" : "5/3", s.poc ? "POC" : prog_name(s.prog),
                      std::max<std::size_t>(1, s.rates.size()), s.numres, features.empty() ? "-" : features.c_str());
        manifest += line;
        ++written;
    }
    if (!verify) {
        const std::string m = join_path(dir, "MANIFEST.tsv");
        if (!write_file(m, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(manifest.data()), manifest.size()))) {
            std::fprintf(stderr, "FAIL cannot write %s\n", m.c_str());
            ++bad;
        }
    }
    std::printf("nj2k_corpus_gen: %d codestreams %s, %d failures\n", written, verify ? "verified" : "written", bad);
    return bad ? 1 : 0;
}

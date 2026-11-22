// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The decoder's Tier-1 (src/t1.cpp: the MQ decoder, the raw (BYPASS) decoder, the three passes and the code-block
// driver) against the frozen reference (the text of src/t1.cpp as it was before the MQ-NG redesign: t1_frozen.cpp).
// The redesign must leave every coefficient and the refusal of a code-block as they were, so each test runs both
// decoders on the same input:
//   1. vectors: the code-blocks of real files as Tier-1 received them, dumped by the MQ-NG study's tool ("MQNG" files:
//      tests/vectors/t1 holds the blocks of corpus/generated and of the 256 x 256 study images; the 35,390 blocks of
//      the mode-switch corpus, the conformance files with them, are run when a folder of dumps is named);
//   2. generated code-blocks: every size 1..64 a side, the shapes 1024 x 4 and 4 x 1024 and their kin, every
//      code-block style 0..63, every orientation, bit-planes 0..31 and ROI shifts, split chunks, and adversarial
//      bytes (all 0xFF, markers 0xFF 0x90 inside a segment, zero-length segments, 109 one-byte segments);
//   2b. segments longer than the bytes (Tier-2 gives none): cut to the bytes present, as the frozen decode of the cut
//      lengths;
//   3. the tables: the 94 MQ states against the standard's Table C.2 written out here;
//   4. single decisions: one decision from a chosen state, every interval a in [0x8000, 0xffff] and every state (94),
//      c_hi around qe, every bit counter ct in [0, 8] and the next two bytes (from 00, 7F, 8F, 90, FE, FF), in the frozen
//      decoder against a model of ISO 15444-1 C.3.2 written out here (and in the redesigned one when it exists): the
//      decoded bit, a, c, ct, the byte position and the context's next state agree. The default run takes every 17th
//      interval and the ends; --exhaustive-full takes every interval and more bytes;
//   5. the run of AGG decisions: the cleanup pass's column of an empty stripe taken at once equals the same decisions
//      one at a time (a, c and the byte position).
// Usage: nj2k_t1_unit [--exhaustive-full] [--no-exhaustive] [PATH ...]   (PATH: a .blk file or a folder of them)
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "common.h"
#include "t1_frozen.h"

// the library's internals (by their path: src/ is no include directory)
#include "../../src/t1.h"
#include "../../src/t1_flags.h"
#include "../../src/t1_tables.h"
#include "t1_unit_new.h"

namespace {

namespace d = nj2k::detail;
namespace fz = nj2k::frozen;
using u8 = std::uint8_t;
using u32 = std::uint32_t;

int g_failures = 0;
#define CHECK_MSG(cond, ...)                                          \
    do {                                                              \
        if (!(cond)) {                                                \
            ++g_failures;                                             \
            std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            std::fprintf(stderr, __VA_ARGS__);                        \
            std::fprintf(stderr, "\n");                               \
        }                                                             \
    } while (0)

struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed) {}
    std::uint64_t next() noexcept
    {
        s += 0x9e3779b97f4a7c15ull;
        std::uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    u32 u(u32 n) noexcept { return static_cast<u32>((next() >> 11) % n); }  // n > 0
};

// ---- a code-block as Tier-1 receives it ----

struct Spec {
    u32 w = 0, h = 0, numbps = 0, orient = 0, roishift = 0, cblksty = 0;
    std::vector<u32> len, passes;  // the segments
    std::vector<u32> ref_len;      // the lengths the frozen reference is given, when the segments are longer than the bytes
    std::vector<u8> bytes;         // the chunks' bytes, gathered
    u32 split = 1;                 // the bytes arrive in this many chunks
};

// Both decoders, each with its scratch kept from block to block (no state may survive one).
struct Runner {
    d::Allocator alloc = d::Allocator::system();
    d::T1 lib{};
    fz::Scratch ref;
    d::Messages quiet;
    std::vector<d::Chunk> chunks;
    std::vector<d::Segment> segs, ref_segs;
    std::vector<std::int32_t> got, want;
    std::uint64_t blocks = 0, refused = 0;

    Runner()
    {
        const bool ok = lib.ready(alloc);
        CHECK_MSG(ok, "no memory");
    }
    ~Runner() { lib.release(alloc); }

    void build(const Spec& s, d::T1Block& b, d::T1Block& rb)
    {
        chunks.clear();
        const u32 n = static_cast<u32>(s.bytes.size());
        const u32 parts = s.bytes.empty() ? 0 : std::max<u32>(1, s.split);
        u32 at = 0;
        for (u32 i = 0; i < parts; ++i) {
            const u32 to = i + 1 == parts ? n : at + (n - at) / (parts - i);
            chunks.push_back({s.bytes.data() + at, to - at});
            at = to;
        }
        segs.assign(s.len.size(), d::Segment{});
        for (std::size_t i = 0; i < s.len.size(); ++i) {
            segs[i].len = s.len[i];
            segs[i].numpasses = segs[i].real_num_passes = s.passes[i];
        }
        ref_segs = segs;
        for (std::size_t i = 0; i < s.ref_len.size(); ++i) {
            ref_segs[i].len = s.ref_len[i];
        }
        b = d::T1Block{s.w, s.h, s.numbps, s.orient, s.roishift, s.cblksty, chunks.data(),
                       static_cast<u32>(chunks.size()), segs.data(), static_cast<u32>(segs.size())};
        rb = b;
        rb.segs = ref_segs.data();
    }

    // false at the first difference (said on stderr)
    bool run(const Spec& s, const char* what)
    {
        d::T1Block b, rb;
        build(s, b, rb);
        const bool okr = [&] {
            if (!ref.t.data || s.bytes.size() + 2 > ref.t.cblkdata_size) {
                ref.reserve(s.bytes.size());
            }
            return fz::t1_decode_cblk(ref.t, rb, quiet);
        }();
        if (!lib.reserve(alloc, s.bytes.size())) {
            CHECK_MSG(false, "no memory");
            return false;
        }
        const bool okn = d::t1_decode_cblk(lib, b, quiet);
        ++blocks;
        refused += okr ? 0 : 1;
        const std::size_t n = std::size_t(s.w) * s.h;
        if (okr != okn || (okr && std::memcmp(ref.t.data, lib.data, n * sizeof(std::int32_t)) != 0)) {
            std::fprintf(stderr, "%s: block differs: %u x %u orient %u numbps %u roi %u style %02x, %zu segments, %zu bytes: result %d / %d\n",
                         what, s.w, s.h, s.orient, s.numbps, s.roishift, s.cblksty, s.len.size(), s.bytes.size(), okr, okn);
            for (std::size_t i = 0; i < n && okr; ++i) {
                if (ref.t.data[i] != lib.data[i]) {
                    std::fprintf(stderr, "  first coefficient: %zu (x %zu y %zu): %d / %d\n", i, i % s.w, i / s.w, ref.t.data[i], lib.data[i]);
                    break;
                }
            }
            return false;
        }
        return true;
    }
};

// ---- 1. vectors ----

// "MQNG", a count, then each block: w, h, numbps, orient, roishift, cblksty, the segment count and its (length,
// passes) pairs, the byte count and the bytes; all little-endian u32 but the bytes
bool read_blocks(const std::string& path, std::vector<Spec>& out)
{
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    bool ok = true;
    auto u = [&]() {
        u32 x = 0;
        ok = ok && std::fread(&x, 4, 1, f) == 1;
        return x;
    };
    char magic[4] = {};
    ok = std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "MQNG", 4) == 0;
    const u32 count = ok ? u() : 0;
    for (u32 k = 0; ok && k < count; ++k) {
        Spec s;
        s.w = u(), s.h = u(), s.numbps = u(), s.orient = u(), s.roishift = u(), s.cblksty = u();
        const u32 ns = u();
        for (u32 i = 0; ok && i < ns; ++i) {
            s.len.push_back(u());
            s.passes.push_back(u());
        }
        const u32 nb = u();
        ok = ok && nb < (1u << 24) && s.w >= 1 && s.h >= 1 && s.w <= 1024 && s.h <= 1024 && s.w * s.h <= 4096;
        if (ok) {
            s.bytes.resize(nb);
            ok = nb == 0 || std::fread(s.bytes.data(), 1, nb, f) == nb;
        }
        out.push_back(std::move(s));
    }
    std::fclose(f);
    return ok;
}

void test_vectors(const std::vector<std::string>& paths)
{
    std::vector<std::string> files;
    for (const std::string& p : paths) {
        std::error_code ec;
        if (std::filesystem::is_directory(p, ec)) {
            for (const auto& e : std::filesystem::recursive_directory_iterator(p, ec)) {
                if (e.is_regular_file() && e.path().extension() == ".blk") {
                    files.push_back(e.path().string());
                }
            }
        } else {
            files.push_back(p);
        }
    }
    std::sort(files.begin(), files.end());
    CHECK_MSG(!files.empty(), "no .blk vectors under the given paths");
    Runner r;
    std::uint64_t styles[64] = {};
    for (const std::string& file : files) {
        std::vector<Spec> blocks;
        if (!read_blocks(file, blocks)) {
            CHECK_MSG(false, "cannot read the vectors %s", file.c_str());
            return;
        }
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            ++styles[blocks[i].cblksty & 63];
            if (!r.run(blocks[i], file.c_str())) {
                std::fprintf(stderr, "  vector %zu of %s\n", i, file.c_str());
                ++g_failures;
                return;
            }
        }
    }
    u32 kinds = 0;
    for (std::uint64_t n : styles) {
        kinds += n != 0;
    }
    std::printf("vectors: %llu code-blocks of %zu files identical (%u code-block styles, %llu refused)\n",
                static_cast<unsigned long long>(r.blocks), files.size(), kinds, static_cast<unsigned long long>(r.refused));
}

// ---- 2. generated code-blocks ----

// The bytes of a stream: random, all 0xFF, all 0, markers inside, sparse 0xFF, a walking pattern
void fill_bytes(std::vector<u8>& v, Rng& rng, u32 kind)
{
    for (std::size_t i = 0; i < v.size(); ++i) {
        u8 b = static_cast<u8>(rng.u(256));
        switch (kind) {
        case 0: break;                        // random
        case 1: b = 0xFF; break;              // all 0xFF
        case 2: b = 0x00; break;              // all 0
        case 3: b = (i & 1) ? static_cast<u8>(0x90 + rng.u(0x70)) : 0xFF; break;  // 0xFF then a marker code
        case 4: b = rng.u(8) == 0 ? 0xFF : static_cast<u8>(rng.u(0x90)); break;   // sparse 0xFF, stuffed bytes
        case 5: b = static_cast<u8>(i * 7 + (i >> 5)); break;
        default: b = rng.u(2) ? 0xFF : static_cast<u8>(0x8F - rng.u(3)); break;   // 0xFF around the 0x8F boundary
        }
        v[i] = b;
    }
}

// The most passes segment i of a code-block of this style can have (opj_t2_init_seg): one a segment with TERMALL,
// 10, then 2, 1, 2, 1, ... with BYPASS, else 109. Tier-2 starts a segment only when the one before it is full, so the
// segments but the last have that many; a segment that BYPASS types raw holds the significance and refinement passes
// only (the decoder starts its arithmetic coder at the cleanup pass of a segment that has one).
u32 seg_maxpasses(u32 style, u32 i)
{
    if (style & d::kCblkTermall) {
        return 1;
    }
    if (style & d::kCblkLazy) {
        return i == 0 ? 10u : (i % 2 == 1 ? 2u : 1u);
    }
    return 109;
}

// A code-block of the given shape and style with segments, passes and bytes drawn at random, the segments' passes
// as Tier-2 gives them; `hostile` takes numbps up to 31, now and then a ROI shift that refuses the block, and the
// adversarial streams.
Spec make_spec(Rng& rng, u32 w, u32 h, u32 style, bool hostile)
{
    Spec s;
    s.w = w, s.h = h, s.cblksty = style;
    s.orient = rng.u(4);
    s.numbps = rng.u(10) == 0 ? 0 : 1 + rng.u(hostile ? 31u : 12u);
    s.roishift = rng.u(8) == 0 ? rng.u(hostile ? 32u : 12u) : 0;
    u32 nsegs;
    const u32 shape = rng.u(8);
    if (shape == 0) {
        nsegs = 109;  // as many one-byte segments as a code-block can have
    } else if ((style & d::kCblkTermall) || (style & d::kCblkLazy)) {
        nsegs = 1 + rng.u(12);
    } else {
        nsegs = 1 + rng.u(3);
    }
    const u32 max_passes = 3 * s.numbps + 3;
    for (u32 i = 0; i < nsegs; ++i) {
        u32 len = shape == 0 ? 1 : (rng.u(6) == 0 ? 0u : rng.u(w * h / 3 + 24));
        if (hostile && rng.u(12) == 0) {
            len = 1 + rng.u(3);
        }
        s.len.push_back(len);
        const u32 most = seg_maxpasses(style, i);
        s.passes.push_back(i + 1 < nsegs ? most : rng.u(std::min(most, max_passes) + 1));
    }
    u32 total = 0;
    for (u32 l : s.len) {
        total += l;
    }
    s.bytes.resize(total + (rng.u(4) == 0 ? rng.u(5) : 0));  // now and then bytes no segment owns
    fill_bytes(s.bytes, rng, rng.u(hostile ? 7u : 1u));
    s.split = 1 + (rng.u(5) == 0 ? rng.u(4) : 0);
    return s;
}

void test_generated()
{
    Runner r;
    Rng rng(20261123);
    // every size, one style by rotation
    for (u32 h = 1; h <= 64; ++h) {
        for (u32 w = 1; w <= 64; ++w) {
            Spec s = make_spec(rng, w, h, (w * 5u + h * 3u + rng.u(64)) % 64u, (w + h) % 5 == 0);
            if (!r.run(s, "generated")) {
                ++g_failures;
                return;
            }
        }
    }
    // the edge sizes in every style
    const u32 edge[] = {1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64};
    for (u32 style = 0; style < 64; ++style) {
        for (u32 w : edge) {
            for (u32 h : edge) {
                Spec s = make_spec(rng, w, h, style, ((w ^ h) & 1) != 0);
                if (!r.run(s, "generated (edge sizes)")) {
                    ++g_failures;
                    return;
                }
            }
        }
    }
    // the long shapes the header allows (at most 4096 samples, 1024 a side)
    const u32 shapes[][2] = {{1024, 4}, {4, 1024}, {512, 8}, {8, 512}, {256, 16}, {16, 256}, {128, 32}, {32, 128},
                             {1024, 3}, {2, 1024}, {1024, 1}, {1, 1024}, {1024, 2}, {64, 64}, {4, 64}, {64, 4}};
    for (u32 i = 0; i < 16 * 24; ++i) {
        const u32* sh = shapes[i % 16];
        Spec s = make_spec(rng, sh[0], sh[1], rng.u(64), (i / 16) % 3 == 0);
        if (!r.run(s, "generated (long shapes)")) {
            ++g_failures;
            return;
        }
    }
    // full-size code-blocks over many contents
    for (u32 i = 0; i < 300; ++i) {
        Spec s = make_spec(rng, 64, 64, rng.u(64), i % 3 == 0);
        if (!r.run(s, "generated (64 x 64)")) {
            ++g_failures;
            return;
        }
    }
    std::printf("generated: %llu code-blocks identical (%llu refused as bit-planes plus ROI shift reach 31)\n",
                static_cast<unsigned long long>(r.blocks), static_cast<unsigned long long>(r.refused));
}

// The segments' lengths longer than the bytes that are there (Tier-2 never gives them): the decoder takes the bytes
// present, each segment cut to what is left after the ones before it; the frozen reference gets the cut lengths.
void test_overlong()
{
    Runner r;
    Rng rng(4242);
    for (u32 i = 0; i < 1500; ++i) {
        const u32 sizes[4][2] = {{64, 64}, {rng.u(64) + 1, rng.u(64) + 1}, {1024, 4}, {4, 1024}};
        const u32* sz = sizes[i % 4];
        Spec s = make_spec(rng, sz[0], sz[1], rng.u(64), i % 2 == 0);
        // lengthen some segments past the bytes (and some to 0xFFFFFFFF-ish: the sum must not wrap)
        for (u32& l : s.len) {
            const u32 k = rng.u(10);
            l = k == 0 ? l + 1 + rng.u(40) : k == 1 ? 0xFFFFFFF0u + rng.u(16) : k == 2 ? static_cast<u32>(s.bytes.size()) + rng.u(3) : l;
        }
        u32 start = 0;
        const u32 total = static_cast<u32>(s.bytes.size());
        for (u32 l : s.len) {
            const u32 cut = std::min(l, total - start);
            s.ref_len.push_back(cut);
            start += cut;
        }
        if (!r.run(s, "overlong segments")) {
            ++g_failures;
            return;
        }
    }
    std::printf("overlong segments: %llu code-blocks identical to the frozen decode of the cut lengths\n",
                static_cast<unsigned long long>(r.blocks));
}

// ---- 3. the tables ----

// ISO 15444-1 Table C.2: qe, next state on an MPS, on an LPS, switch (the MPS flips); 47 states
struct C2 {
    u32 qe, nmps, nlps, sw;
};
constexpr C2 kC2[47] = {
    {0x5601, 1, 1, 1},   {0x3401, 2, 6, 0},   {0x1801, 3, 9, 0},   {0x0AC1, 4, 12, 0},  {0x0521, 5, 29, 0},
    {0x0221, 38, 33, 0}, {0x5601, 7, 6, 1},   {0x5401, 8, 14, 0},  {0x4801, 9, 14, 0},  {0x3801, 10, 14, 0},
    {0x3001, 11, 17, 0}, {0x2401, 12, 18, 0}, {0x1C01, 13, 20, 0}, {0x1601, 29, 21, 0}, {0x5601, 15, 14, 1},
    {0x5401, 16, 14, 0}, {0x5101, 17, 15, 0}, {0x4801, 18, 16, 0}, {0x3801, 19, 17, 0}, {0x3401, 20, 18, 0},
    {0x3001, 21, 19, 0}, {0x2801, 22, 19, 0}, {0x2401, 23, 20, 0}, {0x2201, 24, 21, 0}, {0x1C01, 25, 22, 0},
    {0x1801, 26, 23, 0}, {0x1601, 27, 24, 0}, {0x1401, 28, 25, 0}, {0x1201, 29, 26, 0}, {0x1101, 30, 27, 0},
    {0x0AC1, 31, 28, 0}, {0x09C1, 32, 29, 0}, {0x08A1, 33, 30, 0}, {0x0521, 34, 31, 0}, {0x0441, 35, 32, 0},
    {0x02A1, 36, 33, 0}, {0x0221, 37, 34, 0}, {0x0141, 38, 35, 0}, {0x0111, 39, 36, 0}, {0x0085, 40, 37, 0},
    {0x0049, 41, 38, 0}, {0x0025, 42, 39, 0}, {0x0015, 43, 40, 0}, {0x0009, 44, 41, 0}, {0x0005, 45, 42, 0},
    {0x0001, 45, 43, 0}, {0x5601, 46, 46, 0}};

// the standard's state s and MPS m as the library's index (state * 2 + MPS)
inline u32 idx(u32 s, u32 m) { return s * 2 + m; }

void test_tables()
{
    bool ok = true;
    for (u32 s = 0; s < 47 && ok; ++s) {
        for (u32 m = 0; m < 2; ++m) {
            const d::MqState& e = d::kMqcStates[idx(s, m)];
            const u32 nmps = idx(kC2[s].nmps, m);
            const u32 nlps = idx(kC2[s].nlps, kC2[s].sw ? 1 - m : m);
            if (e.qeval != kC2[s].qe || e.mps != m || u32(e.nmps - d::kMqcStates) != nmps || u32(e.nlps - d::kMqcStates) != nlps) {
                std::fprintf(stderr, "kMqcStates[%u] differs from Table C.2 (state %u, MPS %u)\n", idx(s, m), s, m);
                ok = false;
            }
        }
    }
    CHECK_MSG(ok, "the MQ state table is not the standard's");
    ok = ok && t1_unit_check_new_tables(kC2);
    CHECK_MSG(ok, "the decoder's state words are not the standard's");
    if (ok) {
        std::printf("tables: the 94 states are Table C.2's\n");
    }
}

// ---- 4. single decisions ----

// ISO 15444-1 C.3.2, with the one-bit renormalisation loop (RENORMD) and BYTEIN of C.3.4, on the state index
struct Model {
    u32 a, c, ct, state;
    const u8* bp;
    u32 decode() noexcept
    {
        const u32 qe = kC2[state >> 1].qe;
        const u32 mps = state & 1;
        const u32 nmps = idx(kC2[state >> 1].nmps, mps);
        const u32 nlps = idx(kC2[state >> 1].nlps, kC2[state >> 1].sw ? 1 - mps : mps);
        u32 dbit;
        a -= qe;
        if ((c >> 16) < qe) {
            if (a < qe) {
                dbit = mps;
                state = nmps;
            } else {
                dbit = 1 - mps;
                state = nlps;
            }
            a = qe;
            renorm();
        } else {
            c -= qe << 16;
            if ((a & 0x8000) == 0) {
                if (a < qe) {
                    dbit = 1 - mps;
                    state = nlps;
                } else {
                    dbit = mps;
                    state = nmps;
                }
                renorm();
            } else {
                dbit = mps;
            }
        }
        return dbit;
    }
    void renorm() noexcept
    {
        do {
            if (ct == 0) {
                bytein();
            }
            a <<= 1;
            c <<= 1;
            --ct;
        } while ((a & 0x8000) == 0);
    }
    void bytein() noexcept
    {
        if (*bp == 0xFF) {
            if (bp[1] > 0x8F) {
                c += 0xFF00;
                ct = 8;
            } else {
                ++bp;
                c += u32(*bp) << 9;
                ct = 7;
            }
        } else {
            ++bp;
            c += u32(*bp) << 8;
            ct = 8;
        }
    }
};

constexpr u8 kBytes[6] = {0x00, 0x7F, 0x8F, 0x90, 0xFE, 0xFF};

// One decision of the frozen decoder, the model and the redesigned decoder from the same state and bytes (by[0..3], then
// 0xFF); false when they differ.
bool one_decision(u32 a, u32 state, u32 chi, u32 clo, u32 ct, const u8* by)
{
    u8 buf[5] = {by[0], by[1], by[2], by[3], 0xFF};  // bp starts at buf[0]: the byte-in reads bp[0] and bp[1]
    const u32 c = (chi << 16) | clo;
    // the frozen decoder
    fz::Mqc m{};
    for (u32 i = 0; i < 19; ++i) {
        fz::mq_setstate(m, i, 0);
    }
    fz::mq_setstate(m, 0, state);
    m.a = a, m.c = c, m.ct = ct;
    u8 fb[5];
    std::memcpy(fb, buf, 5);
    m.bp = fb;
    const u32 dfz = fz::mq_decode_one(m, 0);
    // the model
    Model mo{a, c, ct, state, buf};
    const u32 dmo = mo.decode();
    if (dfz != dmo || m.a != mo.a || m.c != mo.c || m.ct != mo.ct || m.bp - fb != mo.bp - buf ||
        fz::mq_state_index(m, 0) != mo.state) {
        std::fprintf(stderr, "frozen decoder against the model: a=%04x state=%u c=%08x ct=%u bytes %02x %02x %02x: d %u/%u a %04x/%04x c %08x/%08x ct %u/%u bp %td/%td state %u/%u\n",
                     a, state, c, ct, by[0], by[1], by[2], dfz, dmo, m.a, mo.a, m.c, mo.c, m.ct, mo.ct, m.bp - fb, mo.bp - buf,
                     fz::mq_state_index(m, 0), mo.state);
        return false;
    }
    return t1_unit_one_decision_new(a, state, c, ct, by, dmo, mo.a, mo.c, mo.ct, static_cast<u32>(mo.bp - buf), mo.state);
}

bool test_single(bool full)
{
    std::uint64_t n = 0;
    const u32 step = full ? 1u : 17u;
    for (u32 a0 = 0; a0 <= 0x7FFF; a0 += step) {
        for (int end = 0; end < (full || a0 + step > 0x7FFF ? 2 : 1); ++end) {
            const u32 a = end ? 0xFFFFu - a0 : 0x8000u + a0;
            for (u32 state = 0; state < 94; ++state) {
                const u32 qe = d::kMqcStates[state].qeval;
                const u32 chis[4] = {qe ? qe - 1 : 0, qe, qe + 1, (a * 2654435761u + state * 40503u) & 0xFFFFu};
                for (u32 ct = 0; ct <= 8; ++ct) {
                    for (u32 k = 0; k < 4; ++k) {
                        const u32 pick = a * 31u + state * 7u + ct * 13u + k * 5u;
                        const u32 clo = (k & 1) ? 0xFFFFu : (pick * 2246822519u) & 0xFFFFu;
                        const u32 pairs = full ? 4u : 1u;
                        for (u32 p = 0; p < pairs; ++p) {
                            u32 q = pick * 2654435761u + p * 40503u;
                            u8 by[4];
                            for (u8& x : by) {
                                x = kBytes[(q >> 8) % 6];
                                q = q * 1664525u + 1013904223u;
                            }
                            if (!one_decision(a, state, chis[k], clo, ct, by)) {
                                ++g_failures;
                                return false;
                            }
                            ++n;
                        }
                    }
                }
            }
        }
    }
    std::printf("single decisions: %llu identical (%s)\n", static_cast<unsigned long long>(n), full ? "every interval" : "every 17th interval");
    return true;
}

// ---- 5. the run of AGG decisions ----

// The run: with the AGG context's MPS 0 and room = min(a - 0x8000, c_hi), the columns while n * qe <= room are each an
// MPS 0 without renormalisation: a -= n * qe, c -= (n * qe) << 16 (study stage 2). Against the decisions one at a time.
bool test_agg_run()
{
    std::uint64_t n = 0;
    Rng rng(77);
    for (u32 state = 0; state < 94; state += 2) {  // MPS 0
        const u32 qe = d::kMqcStates[state].qeval;
        for (u32 ai = 0; ai < 400; ++ai) {
            const u32 a = ai < 200 ? 0x8000 + ai * 7 : 0x8000 + rng.u(0x8000);
            for (u32 ci = 0; ci < 24; ++ci) {
                const u32 chi = ci < 8 ? std::min(qe * ci + (ci & 1), 0xFFFFu) : rng.u(0x10000);
                const u32 clo = rng.u(0x10000);
                for (u32 cols : {1u, 2u, 3u, 7u, 16u, 63u}) {
                    // one at a time through the frozen decoder, the columns' MPS decisions in a row: stops at the first other
                    u8 fb[4] = {0x55, 0x12, 0x34, 0xFF};
                    fz::Mqc m{};
                    for (u32 i = 0; i < 19; ++i) {
                        fz::mq_setstate(m, i, 0);
                    }
                    fz::mq_setstate(m, 0, state);
                    m.a = a, m.c = (chi << 16) | clo, m.ct = 5, m.bp = fb;
                    u32 done = 0;
                    while (done < cols) {
                        const u32 a0 = m.a, c0 = m.c, ct0 = m.ct;
                        u8* bp0 = m.bp;
                        const u32 s0 = fz::mq_state_index(m, 0);
                        const u32 dbit = fz::mq_decode_one(m, 0);
                        if (dbit != 0 || m.a != a0 - qe || m.c != c0 - (qe << 16) || m.ct != ct0 || m.bp != bp0 ||
                            fz::mq_state_index(m, 0) != s0) {
                            break;  // not an MPS without renormalisation
                        }
                        ++done;
                    }
                    // the run's formula
                    const u32 room = std::min(a - 0x8000u, chi);
                    u32 run = 0;
                    while (run < cols && (run + 1) * qe <= room) {
                        ++run;
                    }
                    if (run != done) {
                        std::fprintf(stderr, "AGG run: state %u a=%04x c_hi=%04x cols %u: the decisions give %u, the formula %u\n", state, a, chi, cols, done, run);
                        ++g_failures;
                        return false;
                    }
                    if (!t1_unit_agg_run_new(state, a, (chi << 16) | clo, cols, done)) {
                        ++g_failures;
                        return false;
                    }
                    ++n;
                }
            }
        }
    }
    std::printf("AGG runs: %llu cases identical to the decisions one at a time\n", static_cast<unsigned long long>(n));
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    nj2k::test::console_reports();
    bool full = false;
    bool exhaustive = true;
    std::vector<std::string> vectors;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--exhaustive-full") == 0) {
            full = true;
        } else if (std::strcmp(argv[i], "--no-exhaustive") == 0) {
            exhaustive = false;
        } else if (argv[i][0] == '-') {
            std::fprintf(stderr, "usage: nj2k_t1_unit [--exhaustive-full] [--no-exhaustive] [PATH ...]\n");
            return 2;
        } else {
            vectors.push_back(argv[i]);
        }
    }
    if (full) {
        // the nightly run: only the exhaustive comparison
        test_single(true);
    } else {
        test_tables();
        if (!vectors.empty()) {
            test_vectors(vectors);
        }
        test_generated();
        test_overlong();
        if (exhaustive) {
            test_single(false);
        }
        test_agg_run();
    }
    if (g_failures) {
        std::fprintf(stderr, "t1_unit: %d failures\n", g_failures);
        return 1;
    }
    std::printf("t1_unit: ok\n");
    return 0;
}

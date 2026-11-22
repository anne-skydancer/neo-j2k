// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// nj2k_t1_dec: the decoder's Tier-1 against the frozen reference (tests/unit/t1_frozen.cpp, the decoder as it was
// before the MQ-NG redesign, built here with the library's own flags), on the code-blocks of dump files (the MQ-NG
// study's "MQNG" files, as tests/unit/t1_unit.cpp reads them). One thread, pinned; the time is TSC cycles of
// t1_decode_cblk over all the blocks of a file; the two decoders alternate in the order each round, and the figure is
// the median of the rounds' new / frozen ratios (new_Mcycles per round and frozen's are printed too). Every block is
// checked equal before anything is timed.
//
//   nj2k_t1_dec [--rounds 11] [--pin 7] FILE.blk ...
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#endif
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

#include "../src/t1.h"
#include "../tests/unit/t1_frozen.h"

namespace {

namespace d = nj2k::detail;
namespace fz = nj2k::frozen;
using u32 = std::uint32_t;

struct Blk {
    u32 w, h, numbps, orient, roishift, cblksty;
    std::vector<d::Segment> segs;
    std::vector<std::uint8_t> bytes;
    d::Chunk chunk;
    d::T1Block b;
};

bool load(const char* path, std::vector<Blk>& out)
{
    std::FILE* f = std::fopen(path, "rb");
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
    const u32 n = ok ? u() : 0;
    out.reserve(out.size() + n);
    for (u32 k = 0; ok && k < n; ++k) {
        Blk b{};
        b.w = u(), b.h = u(), b.numbps = u(), b.orient = u(), b.roishift = u(), b.cblksty = u();
        const u32 ns = u();
        for (u32 i = 0; ok && i < ns; ++i) {
            d::Segment s{};
            s.len = u();
            s.numpasses = s.real_num_passes = u();
            b.segs.push_back(s);
        }
        b.bytes.resize(u());
        ok = ok && (b.bytes.empty() || std::fread(b.bytes.data(), 1, b.bytes.size(), f) == b.bytes.size());
        out.push_back(std::move(b));
    }
    std::fclose(f);
    for (Blk& b : out) {  // after the vector stops moving
        b.chunk = {b.bytes.data(), static_cast<u32>(b.bytes.size())};
        b.b = d::T1Block{b.w, b.h, b.numbps, b.orient, b.roishift, b.cblksty, &b.chunk, b.bytes.empty() ? 0u : 1u,
                         b.segs.data(), static_cast<u32>(b.segs.size())};
    }
    return ok;
}

}  // namespace

int main(int argc, char** argv)
{
    int rounds = 11, pin = 7;
    std::vector<const char*> files;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--rounds") == 0 && i + 1 < argc) {
            rounds = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--pin") == 0 && i + 1 < argc) {
            pin = std::atoi(argv[++i]);
        } else {
            files.push_back(argv[i]);
        }
    }
#if defined(_WIN32)
    SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << pin);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#else
    (void)pin;
#endif
    const d::Allocator alloc = d::Allocator::system();
    d::T1 lib{};
    fz::Scratch ref;
    const d::Messages quiet;
    if (!lib.ready(alloc) || !lib.reserve(alloc, 1u << 22)) {
        return 2;
    }
    ref.reserve(1u << 22);
    int bad = 0;
    for (const char* path : files) {
        std::vector<Blk> blocks;
        if (!load(path, blocks)) {
            std::fprintf(stderr, "cannot read %s\n", path);
            return 2;
        }
        // equality first
        for (const Blk& b : blocks) {
            const bool a = fz::t1_decode_cblk(ref.t, b.b, quiet);
            const bool c = d::t1_decode_cblk(lib, b.b, quiet);
            if (a != c || std::memcmp(ref.t.data, lib.data, std::size_t(b.w) * b.h * 4) != 0) {
                std::fprintf(stderr, "%s: a block differs\n", path);
                ++bad;
                break;
            }
        }
        std::vector<double> ratio, told, tnew;
        for (int r = 0; r < rounds + 1; ++r) {  // the first round warms up
            std::uint64_t cyc[2] = {};
            for (int k = 0; k < 2; ++k) {
                const bool new_first = (r & 1) != 0;
                const bool use_new = (k == 0) == new_first;
                const std::uint64_t t0 = __rdtsc();
                if (use_new) {
                    for (const Blk& b : blocks) {
                        d::t1_decode_cblk(lib, b.b, quiet);
                    }
                } else {
                    for (const Blk& b : blocks) {
                        fz::t1_decode_cblk(ref.t, b.b, quiet);
                    }
                }
                cyc[use_new ? 1 : 0] = __rdtsc() - t0;
            }
            if (r > 0) {
                ratio.push_back(double(cyc[1]) / double(cyc[0]));
                told.push_back(double(cyc[0]) / 1e6);
                tnew.push_back(double(cyc[1]) / 1e6);
            }
        }
        std::sort(ratio.begin(), ratio.end());
        std::sort(told.begin(), told.end());
        std::sort(tnew.begin(), tnew.end());
        std::printf("%s: %zu blocks, frozen %.2f Mcycles, new %.2f Mcycles, ratio %.3f (min %.3f max %.3f)\n", path, blocks.size(),
                    told[told.size() / 2], tnew[tnew.size() / 2], ratio[ratio.size() / 2], ratio.front(), ratio.back());
    }
    lib.release(alloc);
    return bad ? 1 : 0;
}

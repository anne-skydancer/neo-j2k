// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// Shared by the fuzz targets. An input that starts with SOC (FF 4F) is a codestream decoded with
// the default options, so the corpus files are seeds as they are; any other input carries four
// option bytes in front of the codestream (fuzz/make_seeds.py writes seeds with them):
//   byte 0  reduce level (0-7: past the coarsest level too)
//   byte 1  mode: plain, strict or viewer
//   byte 2  layers to decode (0 for all)
//   byte 3  cut: 255 keeps every byte, n keeps 1 + (size - 1) * n / 255 bytes
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "common.h"
#include "decoder.h"
#include "j2k_scan.h"

namespace nj2k::fuzz {

struct Input {
    std::span<const std::uint8_t> codestream;
    int reduce = 0;
    test::Mode mode = test::Mode::Plain;
    int layers = 0;
};

inline Input split_input(const std::uint8_t* data, std::size_t size)
{
    Input in;
    if (size >= 2 && data[0] == 0xFF && data[1] == 0x4F) {
        in.codestream = {data, size};
        return in;
    }
    if (size < 5) {
        return in;
    }
    in.reduce = data[0] % 8;
    const test::Mode modes[3] = {test::Mode::Plain, test::Mode::Strict, test::Mode::Viewer};
    in.mode = modes[data[1] % 3];
    in.layers = data[2] % 10;
    const std::size_t n = size - 4;
    const std::size_t keep = data[3] == 255 ? n : 1 + (n - 1) * data[3] / 255;
    in.codestream = {data + 4, keep};
    return in;
}

// Once per process, before the first codec: the oracle decodes and encodes on the calling thread.
inline void fuzz_setup()
{
    static const bool once = (test::single_thread_openjpeg(), true);
    (void)once;
}

// Inputs that reached the code under test (past the filters and guards). The replay driver
// (standalone_main.cpp) reports it and fails when no input did, so a replay cannot pass having
// tested nothing; libFuzzer builds never read it.
inline std::size_t& exercised() noexcept
{
    static std::size_t n = 0;
    return n;
}

// OpenJPEG allocates and loops by what a header declares (fork audit S2, S3; doc/oracle.md OD-3
// and OD-5): the oracle only sees streams within these limits, so the fuzzer finds bugs rather
// than known resource bombs. neo-j2k enforces its own limits and gets no such filter.

// The header target. opj_read_header reads the main header, which allocates by the tiles and
// components SIZ declares (fork audit S2: 779 MB from 102 bytes with 65,535 tiles in OpenJPEG
// 2.5.4; the 1a pin allocates a tile's parameters only when the tile appears, but keeps an entry
// of about 5.5 KB for every tile). No other main-header marker allocates more than its own bytes,
// and the header pass allocates no samples, so nothing else is bounded here.
inline bool oracle_may_read_header(std::span<const std::uint8_t> cs)
{
    if (cs.size() < 4 || cs[0] != 0xFF || cs[1] != 0x4F) {
        return true;  // no SOC: OpenJPEG refuses the stream before it allocates
    }
    if (cs[2] != 0xFF || cs[3] != 0x51) {
        // SIZ must follow SOC. OpenJPEG skips an unknown marker there by scanning for the next one
        // it knows, so a SIZ can hide behind it, unseen here.
        return false;
    }
    if (cs.size() < 42) {
        return true;  // a cut SIZ: OpenJPEG refuses it
    }
    auto u32 = [&](std::size_t p) {
        return std::uint32_t(cs[p]) << 24 | std::uint32_t(cs[p + 1]) << 16 | std::uint32_t(cs[p + 2]) << 8 | cs[p + 3];
    };
    const std::uint64_t xsiz = u32(8), ysiz = u32(12), xtsiz = u32(24), ytsiz = u32(28), xtosiz = u32(32),
                        ytosiz = u32(36);
    const std::uint64_t csiz = std::uint64_t(cs[40]) << 8 | cs[41];
    if (xtsiz == 0 || ytsiz == 0 || xsiz <= xtosiz || ysiz <= ytosiz) {
        return true;  // OpenJPEG refuses this SIZ before it allocates
    }
    const std::uint64_t tiles = ((xsiz - xtosiz + xtsiz - 1) / xtsiz) * ((ysiz - ytosiz + ytsiz - 1) / ytsiz);
    return tiles <= 4096 && tiles * csiz <= 16384;
}

// The decode target. It fails closed: a stream whose main header this filter cannot follow
// exactly as OpenJPEG does (a header it cannot parse, or a marker OpenJPEG skips by scanning) is
// kept away, since what OpenJPEG then reads is unknown here.
inline bool oracle_may_decode(std::span<const std::uint8_t> cs, int reduce)
{
    if (!oracle_may_read_header(cs)) {
        return false;
    }
    const auto mh = test::parse_main_header(cs);
    if (!mh || mh->unknown_marker) {
        return false;
    }
    const auto tps = test::scan_tile_parts(cs, *mh);
    // small enough that a decode stays well under a second with sanitizers, so the fuzzer explores
    // structure rather than waiting on large images
    test::Limits limits;
    limits.max_tiles = 1024;
    limits.max_components = 16;
    limits.max_layers = 64;
    limits.max_side = 4096;
    limits.max_samples = 1ull << 20;
    limits.max_packets = 1ull << 16;
    limits.max_codeblocks = 1ull << 16;
    if (!test::within_limits(*mh, tps, reduce, limits)) {
        return false;
    }
    // In non-strict mode OpenJPEG allocates every tile-part's declared length (Psot) before it
    // reads, whatever is left of the stream: a 782-byte stream that declares 4 GB commits 4 GB
    // (oracle defect OD-3, doc/oracle.md)
    for (const auto& tp : tps) {
        if (tp.psot > (1u << 26)) {
            return false;
        }
    }
    return true;
}

}  // namespace nj2k::fuzz

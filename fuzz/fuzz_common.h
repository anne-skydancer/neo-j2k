// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// Shared by the fuzz targets. An input that starts with SOC (FF 4F) is a codestream decoded with
// the default options, so the corpus files are seeds as they are; any other input carries four
// option bytes in front of the codestream:
//   byte 0  reduce level (0-7: past the coarsest level too)
//   byte 1  mode: plain, strict or viewer
//   byte 2  layers to decode (0 for all)
//   byte 3  cut: 255 keeps every byte, n keeps 1 + (size - 1) * n / 255 bytes
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

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

// OpenJPEG allocates and loops by what a header declares (fork audit S2, S3): the oracle only sees
// streams within these limits, so the fuzzer finds bugs rather than known resource bombs. neo-j2k
// enforces its own limits and gets no such filter.
inline bool oracle_may_decode(std::span<const std::uint8_t> cs, int reduce)
{
    // small enough that a decode stays well under a second with sanitizers, so the fuzzer explores
    // structure rather than waiting on large images
    test::Limits limits;
    limits.max_tiles = 1024;
    limits.max_components = 16;
    limits.max_layers = 64;
    limits.max_side = 4096;
    limits.max_samples = 1ull << 20;
    limits.max_packets = 1ull << 16;
    std::string why;
    const auto mh = test::parse_main_header(cs, &why);
    if (mh) {
        return test::within_limits(*mh, reduce, limits);
    }
    // No complete main header. SIZ comes first and OpenJPEG allocates its tiles on reading it, so
    // a stream whose SIZ declares too much is kept away even when the rest is cut.
    if (cs.size() >= 4 + 40 && cs[2] == 0xFF && cs[3] == 0x51) {
        auto u32 = [&](std::size_t p) {
            return std::uint32_t(cs[p]) << 24 | std::uint32_t(cs[p + 1]) << 16 | std::uint32_t(cs[p + 2]) << 8 | cs[p + 3];
        };
        const std::uint64_t xsiz = u32(8), ysiz = u32(12), xtsiz = u32(24), ytsiz = u32(28), xtosiz = u32(32),
                            ytosiz = u32(36);
        const std::uint32_t csiz = std::uint32_t(cs[40] << 8 | cs[41]);
        if (xtsiz == 0 || ytsiz == 0 || xsiz <= xtosiz || ysiz <= ytosiz) {
            return true;  // OpenJPEG refuses this SIZ before it allocates
        }
        const std::uint64_t tiles = ((xsiz - xtosiz + xtsiz - 1) / xtsiz) * ((ysiz - ytosiz + ytsiz - 1) / ytsiz);
        return tiles <= limits.max_tiles && csiz <= limits.max_components;
    }
    return true;
}

}  // namespace nj2k::fuzz

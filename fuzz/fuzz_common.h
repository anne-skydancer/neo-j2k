// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// Shared by the fuzz targets. An input that starts with SOC (FF 4F) is a codestream decoded with
// the default options, so the corpus files are seeds as they are; any other input carries four
// option bytes in front of the codestream (fuzz/make_inputs.py --decode-seeds writes seeds with them):
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

// Once per process, before the first codec: failures reported on the console, and the oracle
// decoding and encoding on the calling thread.
inline void fuzz_setup()
{
    static const bool once = (test::console_reports(), test::single_thread_openjpeg(), true);
    (void)once;
}

// The decode target's working memory: neo-j2k's own decode has it as its max_memory, and the
// comparison of the two decoders as theirs (decode_request).
inline constexpr std::uint64_t kDecodeWorkingMemory = 64ull << 20;

// The request both decoders get in the decode target's comparison: the input's options under the
// Second Life profile with kDecodeWorkingMemory, the limits neo-j2k's own decode has.
inline test::Request decode_request(const Input& in)
{
    test::Request rq;
    rq.data = in.codestream.data();
    rq.size = in.codestream.size();
    rq.reduce = in.reduce;
    rq.layers = in.mode == test::Mode::Viewer ? 0 : in.layers;
    rq.mode = in.mode;
    rq.limits = true;
    rq.max_memory = kDecodeWorkingMemory;
    return rq;
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
// and OD-5). The fuzzer is to find bugs rather than these known resource bombs, so the oracle is
// guarded: the header target filters what it gives the oracle, and the decode target decodes under
// the oracle's own limits (the fork's opj_decoder_set_limits) and keeps from it only what those do
// not bound. neo-j2k enforces its own limits and is never guarded.

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

// The decode target's one guard (OD-3, doc/oracle.md): in non-strict mode OpenJPEG allocates a
// tile-part's declared length (Psot) before it reads it, whatever is left of the stream, and its
// limits do not count that buffer, so a 782-byte stream that declares 4 GB commits 4 GB. True when
// any SOT marker segment of the bytes declares more than 64 MiB. Every byte is scanned, not the
// marker segments in turn: OpenJPEG reads only SOT segments of 10 bytes, and it may reach one by
// scanning past an unknown marker, so a scan for FF 90 00 0A finds every SOT it could read (and some
// it would not, which only keeps more inputs from the oracle). Strict mode refuses a tile-part longer
// than the bytes left before it allocates, so it needs no guard.
inline bool oracle_psot_bomb(std::span<const std::uint8_t> cs)
{
    for (std::size_t i = 0; cs.size() >= 10 && i <= cs.size() - 10; ++i) {
        if (cs[i] == 0xFF && cs[i + 1] == 0x90 && cs[i + 2] == 0x00 && cs[i + 3] == 0x0A) {
            const std::uint32_t psot = std::uint32_t(cs[i + 6]) << 24 | std::uint32_t(cs[i + 7]) << 16 |
                                       std::uint32_t(cs[i + 8]) << 8 | cs[i + 9];
            if (psot > (1u << 26)) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace nj2k::fuzz

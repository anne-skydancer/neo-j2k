// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// Fuzz target: the decoder's Tier-1 on a code-block of any bytes. The input becomes a T1Block, and the library's
// t1_decode_cblk and the frozen reference (tests/unit/t1_frozen.cpp: the decoder as it was before the MQ-NG redesign)
// decode it; the result (the refusal of a code-block whose bit-planes plus ROI shift reach 31) and every coefficient
// must be the same, with no read or write outside the buffers (the sanitizers' part) and no hang.
//   bytes 0-1  the shape: bit 7 of byte 0 set picks one of 16 long shapes (1024 x 4, 4 x 1024, ...) by its low 4
//              bits; else the width is 1 + byte 0 % 64 and the height 1 + byte 1 % 64
//   byte 2     the code-block style (low 6 bits) and the number of chunks the bytes arrive in less one (bits 6-7)
//   byte 3     the orientation (bits 0-1) and the ROI shift (bits 2-7, 0-63 taken modulo 32)
//   byte 4     the bit-planes (modulo 36: past 31 are refused)
//   byte 5     the number of segments (modulo 112: the most a code-block can have is 109)
//   then       two bytes a segment (its length, its passes), then the code-block's bytes
// A segment's length is as the byte says: past the bytes that are there, the decoder takes the bytes present
// (the frozen reference is given the cut length). A failed check says why on stderr, then aborts.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../src/t1.h"
#include "../tests/unit/t1_frozen.h"
#include "fuzz_common.h"

namespace {

namespace d = nj2k::detail;
namespace fz = nj2k::frozen;

constexpr std::uint32_t kLong[16][2] = {{1024, 4}, {4, 1024}, {512, 8}, {8, 512}, {256, 16}, {16, 256},
                                        {128, 32}, {32, 128}, {1024, 3}, {2, 1024}, {1024, 1}, {1, 1024},
                                        {1024, 2}, {64, 64}, {4, 64}, {64, 4}};

[[noreturn]] void fail(const char* what, std::uint32_t w, std::uint32_t h, std::uint32_t style)
{
    std::fprintf(stderr, "t1: %s (%u x %u, style %02x)\n", what, w, h, style);
    std::fflush(stderr);
    std::abort();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < 6) {
        return 0;
    }
    std::uint32_t w, h;
    if (data[0] & 0x80) {
        w = kLong[data[0] & 15][0];
        h = kLong[data[0] & 15][1];
    } else {
        w = 1 + data[0] % 64u;
        h = 1 + data[1] % 64u;
    }
    const std::uint32_t style = data[2] & 63u;
    const std::uint32_t parts = 1 + (data[2] >> 6);
    const std::uint32_t orient = data[3] & 3u;
    const std::uint32_t roishift = (data[3] >> 2) % 32u;
    const std::uint32_t numbps = data[4] % 36u;
    const std::uint32_t nsegs = data[5] % 112u;
    std::size_t at = 6;
    if (size < at + 2 * std::size_t(nsegs)) {
        return 0;
    }
    const std::uint8_t* seg_bytes = data + at;
    at += 2 * std::size_t(nsegs);
    const std::uint8_t* bytes = data + at;
    const std::uint32_t nbytes = static_cast<std::uint32_t>(std::min<std::size_t>(size - at, 1u << 20));

    std::vector<d::Segment> segs(nsegs), cut(nsegs);
    std::uint32_t start = 0;
    for (std::uint32_t i = 0; i < nsegs; ++i) {
        segs[i] = d::Segment{seg_bytes[2 * i], seg_bytes[2 * i + 1], seg_bytes[2 * i + 1], 0, 0, 0};
        cut[i] = segs[i];
        cut[i].len = std::min(segs[i].len, nbytes - start);
        start += cut[i].len;
    }
    std::vector<d::Chunk> chunks;
    {
        std::uint32_t from = 0;
        const std::uint32_t n = nbytes ? parts : 0;
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::uint32_t to = i + 1 == n ? nbytes : from + (nbytes - from) / (n - i);
            chunks.push_back({bytes + from, to - from});
            from = to;
        }
    }
    const d::T1Block lib_block{w, h, numbps, orient, roishift, style, chunks.data(),
                               static_cast<std::uint32_t>(chunks.size()), segs.data(), nsegs};
    d::T1Block ref_block = lib_block;
    ref_block.segs = cut.data();

    static const d::Messages quiet;
    static const d::Allocator alloc = d::Allocator::system();
    static d::T1 lib;
    static fz::Scratch ref;  // both kept from input to input, as a worker keeps its scratch
    if (!lib.ready(alloc) || !lib.reserve(alloc, nbytes)) {
        return 0;
    }
    ref.reserve(nbytes);
    ++nj2k::fuzz::exercised();
    const bool okr = fz::t1_decode_cblk(ref.t, ref_block, quiet);
    const bool okn = d::t1_decode_cblk(lib, lib_block, quiet);
    if (okr != okn) {
        fail("the refusal differs", w, h, style);
    }
    if (std::memcmp(ref.t.data, lib.data, std::size_t(w) * h * sizeof(std::int32_t)) != 0) {
        fail("the coefficients differ", w, h, style);
    }
    return 0;
}

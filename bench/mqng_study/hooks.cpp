// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
#include "hooks.h"

#include "t1.h"

namespace mqs {

Clock g_clock;
bool g_dump = false;
std::vector<DumpBlock> g_blocks;

void dump(const nj2k::detail::T1Block& b) noexcept
{
    if (!g_dump) {
        return;
    }
    DumpBlock k{};
    k.w = b.w;
    k.h = b.h;
    k.numbps = b.numbps;
    k.orient = b.orient;
    k.roishift = b.roishift;
    k.cblksty = b.cblksty;
    for (std::uint32_t i = 0; i < b.nsegs; ++i) {
        k.seg_len.push_back(b.segs[i].len);
        k.seg_passes.push_back(b.segs[i].real_num_passes);
    }
    for (std::uint32_t i = 0; i < b.nchunks; ++i) {
        k.bytes.insert(k.bytes.end(), b.chunks[i].data, b.chunks[i].data + b.chunks[i].len);
    }
    g_blocks.push_back(std::move(k));
}

}  // namespace mqs

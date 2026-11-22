// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The code-block dump file of the MQ-NG study: "MQNG", a count, then each block's six fields, its segments
// (length, passes) and its gathered bytes, all little-endian u32 but the bytes.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace mqs {

struct DumpBlock {
    std::uint32_t w, h, numbps, orient, roishift, cblksty;
    std::vector<std::uint32_t> seg_len, seg_passes;
    std::vector<std::uint8_t> bytes;
};

inline bool write_blocks(const std::string& path, const std::vector<DumpBlock>& v)
{
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    auto u = [&](std::uint32_t x) { std::fwrite(&x, 4, 1, f); };
    std::fwrite("MQNG", 1, 4, f);
    u(static_cast<std::uint32_t>(v.size()));
    for (const DumpBlock& b : v) {
        u(b.w), u(b.h), u(b.numbps), u(b.orient), u(b.roishift), u(b.cblksty);
        u(static_cast<std::uint32_t>(b.seg_len.size()));
        for (std::size_t i = 0; i < b.seg_len.size(); ++i) {
            u(b.seg_len[i]), u(b.seg_passes[i]);
        }
        u(static_cast<std::uint32_t>(b.bytes.size()));
        if (!b.bytes.empty()) {
            std::fwrite(b.bytes.data(), 1, b.bytes.size(), f);
        }
    }
    return std::fclose(f) == 0;
}

inline bool read_blocks(const std::string& path, std::vector<DumpBlock>& v)
{
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    bool ok = true;
    auto u = [&]() {
        std::uint32_t x = 0;
        ok = ok && std::fread(&x, 4, 1, f) == 1;
        return x;
    };
    char magic[4] = {};
    ok = std::fread(magic, 1, 4, f) == 4 && magic[0] == 'M' && magic[1] == 'Q' && magic[2] == 'N' && magic[3] == 'G';
    const std::uint32_t n = u();
    for (std::uint32_t k = 0; ok && k < n; ++k) {
        DumpBlock b{};
        b.w = u(), b.h = u(), b.numbps = u(), b.orient = u(), b.roishift = u(), b.cblksty = u();
        const std::uint32_t ns = u();
        for (std::uint32_t i = 0; ok && i < ns; ++i) {
            b.seg_len.push_back(u());
            b.seg_passes.push_back(u());
        }
        const std::uint32_t nb = u();
        b.bytes.resize(nb);
        ok = ok && (nb == 0 || std::fread(b.bytes.data(), 1, nb, f) == nb);
        v.push_back(std::move(b));
    }
    std::fclose(f);
    return ok;
}

}  // namespace mqs

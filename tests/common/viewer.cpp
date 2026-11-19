// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The viewer rules the replay follows, re-stated from their behaviour in the viewer's release line
// (indra/llimage/llimagej2c.cpp calcDataSizeJ2C, indra/llimagej2coj/llj2creduce.h and
// llj2cdecodeguard.h). Only behaviour is reproduced here; no viewer code is copied.
#include <algorithm>
#include <cmath>

#include "decoder.h"

namespace nj2k::test::viewer {

std::int64_t data_size_j2c(int w, int h, int discard)
{
    // 64x64 blocks at discard 5, 4 components of 8 bits, compression rate 1/8, then a 600-byte header
    constexpr std::int64_t base_area = 64 * 64, bits_per_tile = 4 * 8, header_bytes = 600;
    constexpr int hard_cap = 12;
    constexpr double rate = 1.0 / 8.0;
    const int discard_layers = std::max(5 - discard, 0);
    auto scaled = [](std::int64_t area) { return static_cast<std::int64_t>(std::llround(double(area * bits_per_tile) * rate)); };
    std::int64_t bits = scaled(base_area);
    if (w > 0 && h > 0) {
        const std::int64_t surface = std::int64_t(w) * h;
        std::int64_t area = base_area;
        int layers = 1;
        while (surface > area && layers < hard_cap) {
            if (layers <= discard_layers) {
                bits += scaled(area);
            }
            ++layers;
            area *= 4;
        }
        const int max_dim = std::max(w, h);
        const float ratio = float(max_dim) / 64.0f;
        const int dim_layers = ratio > 0.0f ? std::max(int(std::floor(std::log2(ratio))) + 1, 1) : 1;
        if (dim_layers > layers) {
            int extra = std::min(dim_layers, hard_cap) - layers;
            while (extra-- > 0) {
                if (layers <= discard_layers) {
                    bits += scaled(area);
                }
                ++layers;
                area *= 4;
            }
        }
    }
    return bits / 8 + header_bytes;
}

int reduce_factor(int requested_discard, int resolutions, int width, int height, int max_image_size)
{
    if (resolutions <= 0 || requested_discard < resolutions || requested_discard > 30) {
        return requested_discard;
    }
    const int coarsest = resolutions - 1;
    const long long side = static_cast<long long>(max_image_size) >> requested_discard;
    const long long pixels = ((width + (1LL << coarsest) - 1) >> coarsest) * ((height + (1LL << coarsest) - 1) >> coarsest);
    return pixels <= side * side ? coarsest : requested_discard;
}

bool decode_allowed(std::uint64_t width, std::uint64_t height, std::uint64_t components, unsigned reduce)
{
    constexpr std::uint64_t max_components = 8, max_side = 16384, max_bytes = 1ull << 30, bytes_per_sample = 4;
    if (components < 1 || components > max_components || reduce > 31) {
        return false;
    }
    const std::uint64_t w = (width + (1ull << reduce) - 1) >> reduce;
    const std::uint64_t h = (height + (1ull << reduce) - 1) >> reduce;
    return w <= max_side && h <= max_side && w * h * bytes_per_sample <= max_bytes / components;
}

}  // namespace nj2k::test::viewer

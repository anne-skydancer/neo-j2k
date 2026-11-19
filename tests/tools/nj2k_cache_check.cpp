// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// nj2k_cache_check: stage 1b's gate on a Second Life viewer's texture cache, run where the cache is
// (it is private, and stays there). Reads texture.entries and texture.cache (the viewer's
// lltexturecache layout: a 44-byte header and 28-byte entries; the first 600 bytes of every texture
// in its slot), read only, and for every texture:
// - compares neo-j2k's header read with the oracle's (the image and the codestream information);
// - reads it under the Second Life profile, with the reduce factor clamped as the viewer clamps it,
//   at every reduce level 0-5, and checks what decoding checks before it allocates: the decoded
//   side and the header-computed working memory.
// Prints aggregates only: no texture data, no names, no UUIDs.
//
//   nj2k_cache_check <texturecache dir> [--opensim]
// Exit status: 0 when every texture reads as the oracle reads it and passes the profile, 1
// otherwise, 2 on a usage or input error.
#include <neoj2k/neoj2k.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "common.h"
#include "decoder.h"

// the library's internals, for the working-memory figure (by their path: src/ is no include directory)
#include "../../src/arena.h"
#include "../../src/header.h"

using namespace nj2k::test;

namespace {

std::uint32_t le32(const std::uint8_t* p)
{
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: nj2k_cache_check <texturecache dir> [--opensim]\n");
        return 2;
    }
    const std::string dir = argv[1];
    const bool opensim = argc > 2 && std::string(argv[2]) == "--opensim";
    const auto entries = read_file(dir + "/texture.entries");
    std::ifstream cache(dir + "/texture.cache", std::ios::binary);
    if (!entries || entries->size() < 44 || !cache) {
        std::fprintf(stderr, "nj2k_cache_check: no texture.entries or texture.cache in %s\n", dir.c_str());
        return 2;
    }
    float version = 0;
    std::memcpy(&version, entries->data(), 4);
    const std::uint32_t address = le32(entries->data() + 4), count = le32(entries->data() + 40);
    if (address != 64 || entries->size() != 44 + std::size_t(count) * 28) {
        std::fprintf(stderr, "nj2k_cache_check: unrecognised cache layout (version %.2f, %u entries)\n", version, count);
        return 2;
    }
    (void)single_thread_openjpeg();
    auto oracle = make_oracle_backend(), neo = make_neoj2k_backend();
    nj2k_decoder_limits_t profile{};
    nj2k_decoder_limits_init(&profile, opensim ? NJ2K_DECODER_LIMITS_OPENSIM_TEXTURE : NJ2K_DECODER_LIMITS_SL_TEXTURE);

    std::size_t textures = 0, both_read = 0, both_refused = 0, different = 0, refused_by_profile = 0, checks = 0;
    std::uint64_t peak_need = 0;
    std::uint32_t peak_side = 0, max_comps = 0, max_layers = 0, max_tiles = 0;
    std::map<std::string, std::size_t> refusals, differences;
    std::vector<std::uint8_t> slot(600);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* e = entries->data() + 44 + std::size_t(i) * 28;
        const std::int32_t image_size = std::int32_t(le32(e + 16));
        if (image_size <= 0) {
            continue;
        }
        const std::size_t n = std::min<std::size_t>(600, std::size_t(image_size));
        cache.seekg(std::streamoff(i) * 600);
        if (!cache.read(reinterpret_cast<char*>(slot.data()), std::streamsize(n))) {
            break;
        }
        ++textures;
        Request rq;
        rq.data = slot.data();
        rq.size = n;
        const HeaderFacts a = oracle->header(rq), b = neo->header(rq);
        const std::string diff = header_difference(a, b);
        if (!diff.empty()) {
            ++different;
            ++differences[diff.substr(0, diff.find(':'))];
            continue;
        }
        if (!a.ok) {
            ++both_refused;
            continue;
        }
        ++both_read;
        // the decode-start checks under the profile, at every reduce level, as the viewer asks
        for (std::uint32_t reduce = 0; reduce <= 5; ++reduce) {
            nj2k::detail::Allocator sys = nj2k::detail::Allocator::system();
            nj2k::detail::Arena arena;
            arena.init(&sys);
            arena.set_budget(profile.max_memory);
            nj2k::detail::Messages msg;
            nj2k::detail::HeaderSettings hs{};
            hs.limits = profile;
            nj2k::detail::MemorySource src(slot.data(), n);
            nj2k::detail::MainHeader* mh = nullptr;
            ++checks;
            const nj2k_status_t st = nj2k::detail::read_main_header(src, arena, msg, hs, mh);
            if (st != NJ2K_OK) {
                ++refused_by_profile;
                ++refusals[std::string("header: ") + nj2k_status_string(st)];
                break;
            }
            const std::uint32_t factor = std::min(reduce, mh->min_resolutions - 1);
            const std::uint64_t need = nj2k::detail::decode_working_memory(*mh, factor, nullptr);
            peak_need = std::max(peak_need, need);
            max_comps = std::max(max_comps, mh->numcomps);
            max_layers = std::max<std::uint32_t>(max_layers, mh->cod.numlayers);
            max_tiles = std::max(max_tiles, mh->tile_count());
            bool side_ok = true;
            for (std::uint32_t c = 0; c < mh->numcomps; ++c) {
                std::uint32_t x0 = 0, y0 = 0, w = 0, h = 0;
                nj2k::detail::header_component_geometry(*mh, c, factor, x0, y0, w, h);
                peak_side = std::max({peak_side, w, h});
                side_ok = side_ok && w <= profile.max_decoded_side && h <= profile.max_decoded_side;
            }
            if (!side_ok) {
                ++refused_by_profile;
                ++refusals["decoded side"];
            } else if (need > profile.max_memory) {
                ++refused_by_profile;
                ++refusals["working memory"];
            }
        }
    }
    std::printf("cache: %u entries, %zu textures read from their header slots\n", count, textures);
    std::printf("header facts against the oracle: %zu both read, %zu both refused, %zu different\n", both_read,
                both_refused, different);
    for (const auto& [k, v] : differences) {
        std::printf("  different %s: %zu\n", k.c_str(), v);
    }
    std::printf("%s profile at reduce 0-5 (clamped as the viewer clamps): %zu checks, %zu refused\n",
                opensim ? "OpenSim" : "Second Life", checks, refused_by_profile);
    for (const auto& [k, v] : refusals) {
        std::printf("  refused, %s: %zu\n", k.c_str(), v);
    }
    std::printf("largest: %u components, %u layers, %u tiles, %u px a side decoded, %.1f MiB of working memory "
                "(limit %.0f MiB)\n",
                max_comps, max_layers, max_tiles, peak_side, double(peak_need) / (1024.0 * 1024.0),
                double(profile.max_memory) / (1024.0 * 1024.0));
    return different || refused_by_profile ? 1 : 0;
}

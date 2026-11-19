// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// nj2k_cache_check: stage 1b's gate on a Second Life viewer's texture cache, run where the cache is
// (it is private, and stays there). Reads texture.entries and texture.cache (the viewer's
// lltexturecache layout: a 44-byte header and 28-byte entries; the first 600 bytes of every texture
// in its slot), read only, and for every texture:
// - compares neo-j2k's header read with the oracle's (the image and the codestream information);
// - at every reduce level 0-5, asks neo-j2k through its public API what the viewer will ask of it:
//   the header read under the profile, with the reduce factor set before it and clamped
//   (NJ2K_REDUCE_CLAMP), then nj2k_get_decoded_dimensions(), which runs the checks decoding runs
//   before it allocates (the decoded side and the header-computed working memory). A refusal at
//   either step is the profile refusing the texture.
// The working memory it prints is the library's own figure (decode_working_memory, which the API
// does not give); the verdict never depends on it. Prints aggregates only: no texture data, no
// names, no UUIDs.
//
//   nj2k_cache_check <texturecache dir> [--opensim]
// Exit status: 0 when every texture reads as the oracle reads it and passes the profile, 1
// otherwise, 2 on a usage or input error, or when the build does not have the profile asked for.
#include <neoj2k/neoj2k.h>
#include <neoj2k/neoj2k.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "common.h"
#include "decoder.h"

// the library's internals, for the working-memory figure only (by their path: src/ is no include
// directory)
#include "../../src/arena.h"
#include "../../src/header.h"

using namespace nj2k::test;

namespace {

std::uint32_t le32(const std::uint8_t* p)
{
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}

void quiet(const char*, void*) {}

// The library's working-memory figure for a texture at a reduce factor (0 when it does not read).
std::uint64_t working_memory(const std::uint8_t* data, std::size_t size, std::uint32_t factor)
{
    nj2k::detail::Allocator sys = nj2k::detail::Allocator::system();
    nj2k::detail::Arena arena;
    arena.init(&sys);
    nj2k::detail::Messages msg;
    nj2k::detail::HeaderSettings hs{};
    nj2k::detail::Source src(data, size);
    nj2k::detail::MainHeader* mh = nullptr;
    if (nj2k::detail::read_main_header(src, arena, msg, hs, mh) != NJ2K_OK) {
        return 0;
    }
    return nj2k::detail::decode_working_memory(*mh, factor, nullptr);
}

}  // namespace

int main(int argc, char** argv)
{
    console_reports();
    if (argc < 2) {
        std::fprintf(stderr, "usage: nj2k_cache_check <texturecache dir> [--opensim]\n");
        return 2;
    }
    const std::string dir = argv[1];
    const bool opensim = argc > 2 && std::string(argv[2]) == "--opensim";
    nj2k_decoder_limits_t profile{};
    if (!nj2k_decoder_limits_init(&profile, opensim ? NJ2K_DECODER_LIMITS_OPENSIM_TEXTURE : NJ2K_DECODER_LIMITS_SL_TEXTURE)) {
        std::fprintf(stderr, "nj2k_cache_check: this build of neo-j2k does not have the %s profile\n",
                     opensim ? "OpenSim" : "Second Life");
        return 2;
    }
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
        // what the viewer's decode asks at every reduce level, through the API
        for (std::uint32_t reduce = 0; reduce <= 5; ++reduce) {
            ++checks;
            nj2k::Codec codec = nj2k::make_decoder();
            nj2k_set_error_handler(codec.get(), quiet, nullptr);
            nj2k_set_warning_handler(codec.get(), quiet, nullptr);
            nj2k_decoder_set_limits(codec.get(), &profile);
            nj2k_decoder_set_reduce_policy(codec.get(), NJ2K_REDUCE_CLAMP);
            nj2k_decoder_set_strict_mode(codec.get(), NJ2K_FALSE);
            nj2k_set_decoded_resolution_factor(codec.get(), reduce);  // before the header, as the viewer
            nj2k::MemoryReader reader(std::span<const std::uint8_t>(slot.data(), n));
            auto stream = reader.make_stream();
            nj2k_image_t* raw = nullptr;
            if (!nj2k_read_header(stream.get(), codec.get(), &raw)) {
                ++refused_by_profile;
                ++refusals[std::string("header: ") + nj2k_status_string(nj2k_get_status(codec.get()))];
                break;
            }
            nj2k::Image image(raw);
            std::uint32_t side = 0;
            for (std::uint32_t c = 0; c < image->numcomps; ++c) {
                side = std::max({side, image->comps[c].w, image->comps[c].h});
            }
            const std::uint32_t factor = image->comps[0].factor;
            peak_side = std::max(peak_side, side);
            max_comps = std::max(max_comps, image->numcomps);
            if (const nj2k::CodestreamInfo info{nj2k_get_cstr_info(codec.get())}) {
                max_layers = std::max<std::uint32_t>(max_layers, info->m_default_tile_info.numlayers);
                max_tiles = std::max(max_tiles, info->tw * info->th);
            }
            peak_need = std::max(peak_need, working_memory(slot.data(), n, factor));
            if (!nj2k_get_decoded_dimensions(codec.get(), image.get())) {
                ++refused_by_profile;
                const nj2k_status_t st = nj2k_get_status(codec.get());
                if (st != NJ2K_E_LIMIT) {
                    ++refusals[std::string("decoded dimensions: ") + nj2k_status_string(st)];
                } else {
                    const bool too_long = profile.max_decoded_side != 0 && side > profile.max_decoded_side;
                    ++refusals[too_long ? "decoded side" : "working memory"];
                }
            }
        }
    }
    std::printf("cache: %u entries, %zu textures read from their header slots\n", count, textures);
    std::printf("header facts against the oracle: %zu both read, %zu both refused, %zu different\n", both_read,
                both_refused, different);
    for (const auto& [k, v] : differences) {
        std::printf("  different %s: %zu\n", k.c_str(), v);
    }
    std::printf("%s profile at reduce 0-5 (clamped as the viewer clamps), through the API: %zu checks, %zu refused\n",
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

// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// Fuzz target: header parsing only.
// - neo-j2k (stage 1b): the header read through the API (J2K, or JP2 when the input starts with
//   the JP2 signature), with no limits and with the Second Life profile, then the decoded
//   dimensions and the codestream information; and the library's tile-part header reader over every
//   tile-part. No filter: neo-j2k bounds its own memory. Every allocation goes through a counting
//   hook, which aborts when the header state outgrows the input (64 bytes for each input byte, plus
//   256 KiB) or when the codec does not give back everything it took.
// - the test scanner (j2k_scan) reads the main header, the tile-parts and the packet positions;
// - the oracle reads the main header and its codestream information, non-strict, as the viewer's
//   metadata pass does, behind the header filter only.
#include <openjpeg.h>

#include <neoj2k/neoj2k.h>
#include <neoj2k/neoj2k.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <malloc.h>
#endif

#include "fuzz_common.h"

// the library's internals: the tile-part header reader (by its path: src/ is no include directory)
#include "../src/header.h"

namespace {

struct Mem {
    const std::uint8_t* p;
    std::size_t n, at;
};

OPJ_SIZE_T mem_read(void* out, OPJ_SIZE_T bytes, void* user)
{
    auto* m = static_cast<Mem*>(user);
    if (m->at >= m->n) {
        return static_cast<OPJ_SIZE_T>(-1);
    }
    const std::size_t k = bytes < m->n - m->at ? bytes : m->n - m->at;
    std::memcpy(out, m->p + m->at, k);
    m->at += k;
    return k;
}

OPJ_OFF_T mem_skip(OPJ_OFF_T bytes, void* user)
{
    auto* m = static_cast<Mem*>(user);
    const OPJ_OFF_T to = OPJ_OFF_T(m->at) + bytes;
    if (to < 0 || to > OPJ_OFF_T(m->n)) {
        m->at = to < 0 ? 0 : m->n;
        return -1;
    }
    m->at = std::size_t(to);
    return bytes;
}

OPJ_BOOL mem_seek(OPJ_OFF_T to, void* user)
{
    auto* m = static_cast<Mem*>(user);
    if (to < 0 || to > OPJ_OFF_T(m->n)) {
        return OPJ_FALSE;
    }
    m->at = std::size_t(to);
    return OPJ_TRUE;
}

void quiet(const char*, void*) {}

struct Budget {
    std::size_t live = 0, peak = 0;

    static void* alloc(void* user, NJ2K_SIZE_T size, NJ2K_SIZE_T alignment)
    {
        auto* b = static_cast<Budget*>(user);
#if defined(_WIN32)
        void* p = _aligned_malloc(size, alignment < 16 ? 16 : alignment);
#else
        void* p = nullptr;
        if (posix_memalign(&p, alignment < sizeof(void*) ? sizeof(void*) : alignment, size) != 0) {
            p = nullptr;
        }
#endif
        if (p) {
            b->live += size;
            b->peak = b->live > b->peak ? b->live : b->peak;
        }
        return p;
    }
    static void release(void* user, void* ptr, NJ2K_SIZE_T size)
    {
        static_cast<Budget*>(user)->live -= size;
#if defined(_WIN32)
        _aligned_free(ptr);
#else
        std::free(ptr);
#endif
    }
};

void neo_header(const std::uint8_t* data, std::size_t size, const nj2k_decoder_limits_t* limits)
{
    Budget budget;
    {
        nj2k_allocator_t hook{sizeof(nj2k_allocator_t), Budget::alloc, Budget::release, &budget};
        const bool jp2 = nj2k::test::is_jp2_file(data, size);
        nj2k_codec_t* codec = nj2k_create_decompress_with_allocator(jp2 ? NJ2K_CODEC_JP2 : NJ2K_CODEC_J2K, &hook);
        if (!codec) {
            std::abort();
        }
        if (limits) {
            nj2k_decoder_set_limits(codec, limits);
        }
        nj2k_decoder_set_strict_mode(codec, NJ2K_FALSE);
        // the reduce factor from the input's first byte, as clamped as the viewer asks for it
        nj2k_decoder_set_reduce_policy(codec, NJ2K_REDUCE_CLAMP);
        nj2k_set_decoded_resolution_factor(codec, size ? data[size - 1] % 8 : 0);
        nj2k::MemoryReader reader(std::span<const std::uint8_t>(data, size));
        auto stream = reader.make_stream();
        nj2k_image_t* image = nullptr;
        if (nj2k_read_header(stream.get(), codec, &image)) {
            nj2k_get_decoded_dimensions(codec, image);
            nj2k_codestream_info_v2_t* info = nj2k_get_cstr_info(codec);
            nj2k_destroy_cstr_info(&info);
            const NJ2K_UINT32 first = 0;
            nj2k_set_decoded_components(codec, 1, &first, NJ2K_FALSE);
            nj2k_get_decoded_dimensions(codec, image);
        } else if (image) {
            std::abort();  // a refusal hands out no image
        }
        nj2k_image_destroy(image);
        nj2k_destroy_codec(codec);
    }
    if (budget.live != 0 || budget.peak > 64 * size + 256 * 1024) {
        std::abort();
    }
}

// The tile-part header reader over every tile-part of a codestream (stage 1c drives it).
void neo_tile_parts(const std::uint8_t* data, std::size_t size)
{
    using namespace nj2k::detail;
    Budget budget;
    {
        Allocator a;
        a.alloc_fn = Budget::alloc;
        a.free_fn = Budget::release;
        a.user = &budget;
        Arena arena;
        arena.init(&a);
        Messages msg;
        HeaderSettings hs{};
        MemorySource src(data, size);
        MainHeader* mh = nullptr;
        if (read_main_header(src, arena, msg, hs, mh) == NJ2K_OK) {
            std::uint64_t sot = mh->end;
            for (int parts = 0; parts < 4096; ++parts) {
                TilePartHeader tp{};
                if (read_tile_part_header(src, arena, msg, hs, *mh, sot, tp) != NJ2K_OK) {
                    break;
                }
                const TileOverrides* t = mh->overrides(tp.tile);
                for (std::uint32_t c = 0; c < mh->numcomps && c < 16; ++c) {
                    TileComponentParams p{};
                    (void)resolve_tile_component(*mh, t, c, msg, p);
                }
                (void)tile_packets(*mh, tp.tile, t);
                const std::uint64_t left = size - tp.data;
                const std::uint64_t length = tp.psot ? tp.data_length : left;
                if (length >= left || !src.skip(length)) {
                    break;
                }
                std::uint8_t m[2];
                if (src.read(m, 2) != 2 || m[0] != 0xFF || m[1] != 0x90) {
                    break;
                }
                sot = tp.data + length;
            }
            (void)decode_working_memory(*mh, mh->min_resolutions - 1, nullptr);
        }
        arena.release_all();
    }
    if (budget.live != 0 || budget.peak > 64 * size + 256 * 1024) {
        std::abort();
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    nj2k::fuzz::fuzz_setup();
    const std::span<const std::uint8_t> bytes(data, size);

    nj2k_decoder_limits_t sl{};
    nj2k_decoder_limits_init(&sl, NJ2K_DECODER_LIMITS_SL_TEXTURE);
    neo_header(data, size, nullptr);
    neo_header(data, size, &sl);
    neo_tile_parts(data, size);
    ++nj2k::fuzz::exercised();

    if (const auto mh = nj2k::test::parse_main_header(bytes)) {
        const auto tps = nj2k::test::scan_tile_parts(bytes, *mh);
        for (const auto& tp : tps) {
            (void)nj2k::test::packet_starts(bytes, *mh, tp);
        }
        (void)nj2k::test::layer_boundaries(bytes);
        (void)nj2k::test::within_limits(*mh, tps, 0, nj2k::test::Limits{});
    }
    if (size == 0 || !nj2k::fuzz::oracle_may_read_header(bytes)) {
        return 0;
    }
    opj_dparameters_t params;
    opj_set_default_decoder_parameters(&params);
    params.flags |= OPJ_DPARAMETERS_DUMP_FLAG;
    opj_codec_t* codec = opj_create_decompress(OPJ_CODEC_J2K);
    opj_set_info_handler(codec, quiet, nullptr);
    opj_set_warning_handler(codec, quiet, nullptr);
    opj_set_error_handler(codec, quiet, nullptr);
    opj_setup_decoder(codec, &params);
    opj_decoder_set_strict_mode(codec, OPJ_FALSE);
    Mem mem{data, size, 0};
    opj_stream_t* stream = opj_stream_create(size, OPJ_TRUE);
    opj_stream_set_user_data(stream, &mem, nullptr);
    opj_stream_set_user_data_length(stream, size);
    opj_stream_set_read_function(stream, mem_read);
    opj_stream_set_skip_function(stream, mem_skip);
    opj_stream_set_seek_function(stream, mem_seek);
    opj_image_t* image = nullptr;
    if (opj_read_header(stream, codec, &image)) {
        opj_codestream_info_v2_t* info = opj_get_cstr_info(codec);
        if (info) {
            opj_destroy_cstr_info(&info);
        }
    }
    opj_image_destroy(image);
    opj_stream_destroy(stream);
    opj_destroy_codec(codec);
    return 0;
}

// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// Fuzz target: header parsing only.
// - neo-j2k (stage 1b): the header read through the API (J2K, or JP2 when the input starts with
//   the JP2 signature), with no limits and with the Second Life profile, then the decoded
//   dimensions, the codestream information and a component selection; and the library's tile-part
//   header reader over every tile-part, with each tile's parameters resolved for all its components
//   at once and one at a time, which must agree. No filter: neo-j2k bounds its own memory. Every
//   allocation goes through a counting hook, which aborts when the header state outgrows the input
//   (64 bytes for each input byte, plus 256 KiB) or when the codec does not give back everything it
//   took. The input's last byte picks the reduce factor (its low three bits) and whether the stream
//   gives its length (bit 3 set: no length, as a pipe gives none).
// - the test scanner (j2k_scan) reads the main header, the tile-parts and the packet positions;
// - the oracle reads the header too, behind the header filter only (applied to the codestream of a
//   JP2 file), and its read and neo-j2k's must agree: the outcome, the image and the codestream
//   information (header_difference), at the input's reduce factor. A difference aborts unless the
//   main header, or a JP2 file's boxes, show a departure doc/departures.md lists for it (DP-5 to
//   DP-8, DP-11).
#include <neoj2k/neoj2k.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#endif

#include "departures.h"
#include "fuzz_common.h"

// the library's internals: the tile-part header reader (by its path: src/ is no include directory)
#include "../src/header.h"

namespace {

using nj2k::fuzz::be16;
using nj2k::fuzz::be32;
using nj2k::fuzz::header_departure;

struct Mem {
    const std::uint8_t* p;
    std::size_t n, at;
};

NJ2K_SIZE_T mem_read(void* out, NJ2K_SIZE_T bytes, void* user)
{
    auto* m = static_cast<Mem*>(user);
    if (m->at >= m->n) {
        return static_cast<NJ2K_SIZE_T>(-1);
    }
    const std::size_t k = bytes < m->n - m->at ? bytes : m->n - m->at;
    std::memcpy(out, m->p + m->at, k);
    m->at += k;
    return k;
}

NJ2K_OFF_T mem_skip(NJ2K_OFF_T bytes, void* user)
{
    auto* m = static_cast<Mem*>(user);
    const NJ2K_OFF_T to = NJ2K_OFF_T(m->at) + bytes;
    if (to < 0 || to > NJ2K_OFF_T(m->n)) {
        m->at = to < 0 ? 0 : m->n;
        return -1;
    }
    m->at = std::size_t(to);
    return bytes;
}

NJ2K_BOOL mem_seek(NJ2K_OFF_T to, void* user)
{
    auto* m = static_cast<Mem*>(user);
    if (to < 0 || to > NJ2K_OFF_T(m->n)) {
        return NJ2K_FALSE;
    }
    m->at = std::size_t(to);
    return NJ2K_TRUE;
}

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

void neo_header(const std::uint8_t* data, std::size_t size, const nj2k_decoder_limits_t* limits, std::uint32_t reduce,
                bool no_length)
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
        // the reduce factor set before the header and clamped to the coarsest level, as the
        // viewer's decode asks for it
        nj2k_decoder_set_reduce_policy(codec, NJ2K_REDUCE_CLAMP);
        nj2k_set_decoded_resolution_factor(codec, reduce);
        Mem mem{data, size, 0};
        nj2k_stream_t* stream = nj2k_stream_create(size ? size : 1, NJ2K_TRUE);
        if (!stream) {
            std::abort();
        }
        nj2k_stream_set_user_data(stream, &mem, nullptr);
        if (!no_length) {
            nj2k_stream_set_user_data_length(stream, size);
        }
        nj2k_stream_set_read_function(stream, mem_read);
        nj2k_stream_set_skip_function(stream, mem_skip);
        nj2k_stream_set_seek_function(stream, mem_seek);
        nj2k_image_t* image = nullptr;
        if (nj2k_read_header(stream, codec, &image)) {
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
        nj2k_stream_destroy(stream);
        nj2k_destroy_codec(codec);
    }
    if (budget.live != 0 || budget.peak > 64 * size + 256 * 1024) {
        std::fprintf(stderr, "header state: %zu bytes at most, %zu not given back, from %zu input bytes\n", budget.peak,
                     budget.live, size);
        std::abort();
    }
}

bool same_params(const nj2k::detail::TileComponentParams& a, const nj2k::detail::TileComponentParams& b)
{
    return a.coding == b.coding && a.style == b.style && a.quant == b.quant && a.roishift == b.roishift;
}

// The tile-part header reader over every tile-part of a codestream (stage 1c drives it), each
// tile's parameters resolved for all its components (resolve_tile) and for the first ones on their
// own (resolve_tile_component), which must give the same.
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
        Source src(data, size);
        MainHeader* mh = nullptr;
        if (read_main_header(src, arena, msg, hs, mh) == NJ2K_OK) {
            // the whole-tile resolution for codestreams of up to 1,024 components, which keeps an
            // input's work small while every component count the profiles allow is covered
            std::vector<TileComponentParams> params(mh->numcomps <= 1024 ? mh->numcomps : 0);
            std::uint64_t sot = mh->end;
            for (int parts = 0; parts < 4096; ++parts) {
                TilePartHeader tp{};
                if (read_tile_part_header(src, arena, msg, hs, *mh, sot, tp) != NJ2K_OK) {
                    break;
                }
                const TileOverrides* t = mh->overrides(tp.tile);
                const bool whole = !params.empty() && resolve_tile(*mh, t, msg, params.data()) == NJ2K_OK;
                if (whole) {
                    (void)tile_working_memory(*mh, tp.tile, params.data(), mh->min_resolutions - 1, nullptr);
                }
                for (std::uint32_t c = 0; c < mh->numcomps && c < 16; ++c) {
                    TileComponentParams p{};
                    const bool one = resolve_tile_component(*mh, t, c, msg, p) == NJ2K_OK;
                    if (whole && (!one || !same_params(p, params[c]))) {
                        std::fprintf(stderr, "tile %u, component %u: resolved alone, not as with the whole tile\n",
                                     unsigned(tp.tile), c);
                        std::abort();
                    }
                }
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
        std::fprintf(stderr, "tile-part header state: %zu bytes at most, %zu not given back, from %zu input bytes\n",
                     budget.peak, budget.live, size);
        std::abort();
    }
}

// The bytes OpenJPEG reads as the codestream of a JP2 file: its boxes walked by their lengths to the
// first codestream box, or to a box header it cannot take (an XL length of 4 GiB or more), after
// which it reads the codestream from where it stopped. Empty when it reaches no codestream.
std::span<const std::uint8_t> jp2_codestream(std::span<const std::uint8_t> file)
{
    std::size_t at = 0;
    while (file.size() - at >= 8) {
        std::uint64_t length = be32(file, at), header = 8;
        const std::uint32_t type = be32(file, at + 4);
        if (length == 1) {
            if (file.size() - at < 16) {
                break;
            }
            if (be32(file, at + 8) != 0) {
                return file.subspan(at + 16);
            }
            length = be32(file, at + 12);
            header = 16;
        }
        if (type == 0x6A703263) {  // jp2c
            return file.subspan(at + header);
        }
        if (length < header || length > file.size() - at) {
            break;
        }
        at += std::size_t(length);
    }
    return {};
}

// The departure (DP-11) a JP2 file's boxes show, or null. A box OpenJPEG skips sets its state to
// JP2_STATE_UNKNOWN (0x7FFFFFFF), which claims every box read: after it OpenJPEG takes a JP2 header
// box with no file type box before it, and reads image boxes that come before the JP2 header box as
// if they were in it. Only an image box outside the JP2 header box, before it, brings that about in
// a file that both readers otherwise take alike (an unknown box right after the signature is refused
// by both), so such a box explains a difference; walked as jp2_codestream walks the boxes.
const char* jp2_departure(std::span<const std::uint8_t> file)
{
    std::size_t at = 0;
    while (file.size() - at >= 8) {
        std::uint64_t length = be32(file, at), header = 8;
        const std::uint32_t type = be32(file, at + 4);
        if (length == 1) {
            if (file.size() - at < 16 || be32(file, at + 8) != 0) {
                break;
            }
            length = be32(file, at + 12);
            header = 16;
        }
        switch (type) {
        case 0x6A703268:  // jp2h
        case 0x6A703263:  // jp2c
            return nullptr;
        case 0x69686472:  // ihdr
        case 0x636F6C72:  // colr
        case 0x62706363:  // bpcc
        case 0x70636C72:  // pclr
        case 0x636D6170:  // cmap
        case 0x63646566:  // cdef
            return "DP-11: an image box before the JP2 header box";
        default:
            break;
        }
        if (length < header || length > file.size() - at) {
            break;
        }
        at += std::size_t(length);
    }
    return nullptr;
}

const char* facts_text(const nj2k::test::HeaderFacts& f)
{
    return f.ok ? "read" : f.stage.c_str();
}

// neo-j2k's header read against the oracle's, through the same request code (the test backends).
void compare_with_oracle(const std::uint8_t* data, std::size_t size, std::span<const std::uint8_t> codestream,
                         std::uint32_t reduce, bool jp2)
{
    static const std::unique_ptr<nj2k::test::Backend> oracle = nj2k::test::make_oracle_backend();
    static const std::unique_ptr<nj2k::test::Backend> neo = nj2k::test::make_neoj2k_backend();
    nj2k::test::Request rq;
    rq.data = data;
    rq.size = size;
    rq.reduce = int(reduce);
    rq.mode = nj2k::test::Mode::Plain;
    rq.jp2 = jp2;
    const nj2k::test::HeaderFacts a = oracle->header(rq), b = neo->header(rq);
    const std::string diff = nj2k::test::header_difference(a, b);
    if (diff.empty() || header_departure(codestream, reduce) ||
        (jp2 && jp2_departure(std::span<const std::uint8_t>(data, size)))) {
        return;
    }
    std::fprintf(stderr,
                 "header read differs from the oracle's at reduce %u, with no departure to explain it: %s\n"
                 "  oracle: %s %s\n  neo-j2k: %s %s\n",
                 reduce, diff.c_str(), facts_text(a), a.last_error.c_str(), facts_text(b), b.last_error.c_str());
    std::abort();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    nj2k::fuzz::fuzz_setup();
    const std::span<const std::uint8_t> bytes(data, size);
    const std::uint32_t reduce = size ? data[size - 1] % 8u : 0;
    const bool no_length = size && (data[size - 1] & 0x08) != 0;

    nj2k_decoder_limits_t sl{};
    nj2k_decoder_limits_init(&sl, NJ2K_DECODER_LIMITS_SL_TEXTURE);
    neo_header(data, size, nullptr, reduce, no_length);
    neo_header(data, size, &sl, reduce, no_length);
    neo_tile_parts(data, size);
    ++nj2k::fuzz::exercised();

    if (const auto mh = nj2k::test::parse_main_header(bytes)) {
        const auto tps = nj2k::test::scan_tile_parts(bytes, *mh);
        for (const auto& tp : tps) {
            (void)nj2k::test::packet_starts(bytes, *mh, tp);
        }
        (void)nj2k::test::layer_boundaries(bytes);
    }
    const bool jp2 = nj2k::test::is_jp2_file(data, size);
    const std::span<const std::uint8_t> codestream = jp2 ? jp2_codestream(bytes) : bytes;
    if (size == 0 || !nj2k::fuzz::oracle_may_read_header(codestream)) {
        return 0;
    }
    compare_with_oracle(data, size, codestream, reduce, jp2);
    return 0;
}

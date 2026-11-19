// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// Fuzz target: decode, in the configurations the viewer uses and OpenJPEG's own OSS-Fuzz target
// never takes (non-strict partial decodes at every reduce level, the viewer's two-pass request),
// plus strict mode and layer limits; the cut length comes from the input (fuzz_common.h).
// - neo-j2k (stage 1c) decodes every input, with no filter: from memory into the caller's planes,
//   under the Second Life limits with 64 MiB of working memory (it checks them from the headers
//   before it allocates), in the input's mode, reduce level (clamped in viewer mode, as the viewer
//   asks) and layer limit. Every allocation goes through a counting hook, which aborts when the
//   codec holds more than twice the working-memory limit plus its header state (64 bytes for each
//   input byte, plus 256 KiB), or does not give back everything it took.
// - the oracle decodes the same request behind its filter, and neo-j2k's decode of it through the
//   same request code (the test backends) must give the same outcome, image and, in viewer mode,
//   8-bit copy, unless the codestream shows a departure doc/departures.md lists (departures.h).
#include <neoj2k/neoj2k.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#endif

#include "departures.h"
#include "fuzz_common.h"

namespace {

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

constexpr std::uint64_t kWorkingMemory = 64ull << 20;

// neo-j2k from memory, bounded: the decode itself, its memory, and that it gives it all back.
void neo_decode(const nj2k::fuzz::Input& in)
{
    Budget budget;
    {
        nj2k_allocator_t hook{sizeof(nj2k_allocator_t), Budget::alloc, Budget::release, &budget};
        nj2k_codec_t* codec = nj2k_create_decompress_with_allocator(NJ2K_CODEC_J2K, &hook);
        if (!codec) {
            std::abort();
        }
        nj2k_dparameters_t params;
        nj2k_set_default_decoder_parameters(&params);
        params.cp_reduce = static_cast<NJ2K_UINT32>(in.reduce);
        params.cp_layer = in.mode == nj2k::test::Mode::Viewer ? 0u : static_cast<NJ2K_UINT32>(in.layers);
        nj2k_setup_decoder(codec, &params);
        nj2k_decoder_limits_t limits;
        nj2k_decoder_limits_init(&limits, NJ2K_DECODER_LIMITS_SL_TEXTURE);
        limits.max_memory = kWorkingMemory;
        nj2k_decoder_set_limits(codec, &limits);
        nj2k_decoder_set_strict_mode(codec, in.mode == nj2k::test::Mode::Strict ? NJ2K_TRUE : NJ2K_FALSE);
        if (in.mode == nj2k::test::Mode::Viewer) {
            nj2k_decoder_set_reduce_policy(codec, NJ2K_REDUCE_CLAMP);
        }
        nj2k_image_t image;
        if (nj2k_read_header_memory(codec, in.codestream.data(), in.codestream.size(), &image)) {
            // 8-bit planes, one after another (the limits keep them to 2,048 x 2,048 x 8)
            std::size_t total = 0;
            for (std::uint32_t c = 0; c < image.numcomps; ++c) {
                total += std::size_t(image.comps[c].w) * image.comps[c].h;
            }
            std::vector<std::uint8_t> raw(total + 1);
            std::vector<nj2k_plane_t> planes(image.numcomps);
            std::size_t at = 0;
            for (std::uint32_t c = 0; c < image.numcomps; ++c) {
                planes[c] = nj2k_plane_t{raw.data() + at, std::int64_t(image.comps[c].w), 1, c};
                at += std::size_t(image.comps[c].w) * image.comps[c].h;
            }
            const nj2k_output_t out{sizeof(nj2k_output_t), NJ2K_SAMPLE_U8, image.numcomps, planes.data()};
            const bool ok = nj2k_decode_memory(codec, in.codestream.data(), in.codestream.size(), &out) != NJ2K_FALSE;
            const nj2k_status_t st = nj2k_get_status(codec);
            if (ok != (st >= 0)) {
                std::fprintf(stderr, "nj2k_decode_memory returned %d with status %d\n", int(ok), int(st));
                std::abort();
            }
            // and again, from the header the codec holds
            (void)nj2k_decode_memory(codec, in.codestream.data(), in.codestream.size(), &out);
        }
        nj2k_destroy_codec(codec);
    }
    const std::size_t bound = std::size_t(2 * kWorkingMemory) + 64 * in.codestream.size() + 256 * 1024;
    if (budget.live != 0 || budget.peak > bound) {
        std::fprintf(stderr, "decode: %zu bytes at most (bound %zu), %zu not given back, from %zu input bytes\n",
                     budget.peak, bound, budget.live, in.codestream.size());
        std::abort();
    }
}

const char* outcome_text(const nj2k::test::Outcome& o)
{
    return o.ok ? "decoded" : o.stage.c_str();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    nj2k::fuzz::fuzz_setup();
    static const auto oracle = nj2k::test::make_oracle_backend();
    static const auto neo = nj2k::test::make_neoj2k_backend();
    const nj2k::fuzz::Input in = nj2k::fuzz::split_input(data, size);
    if (in.codestream.empty()) {
        return 0;
    }
    neo_decode(in);
    ++nj2k::fuzz::exercised();
    if (!nj2k::fuzz::oracle_may_decode(in.codestream, in.reduce)) {
        return 0;
    }
    nj2k::test::Request rq;
    rq.data = in.codestream.data();
    rq.size = in.codestream.size();
    rq.reduce = in.reduce;
    rq.layers = in.mode == nj2k::test::Mode::Viewer ? 0 : in.layers;
    rq.mode = in.mode;
    const nj2k::test::Outcome a = oracle->decode(rq), b = neo->decode(rq);
    const bool same = a.ok == b.ok && a.hash == b.hash && a.u8_hash == b.u8_hash;
    if (same || nj2k::fuzz::decode_departure(in.codestream, static_cast<std::uint32_t>(in.reduce))) {
        return 0;
    }
    std::fprintf(stderr,
                 "decode differs from the oracle's (%s, reduce %d, layers %d, %zu bytes), with no departure to "
                 "explain it\n  oracle: %s %s\n  neo-j2k: %s %s\n",
                 nj2k::test::mode_name(in.mode), in.reduce, rq.layers, rq.size, outcome_text(a), a.last_error.c_str(),
                 outcome_text(b), b.last_error.c_str());
    std::abort();
}

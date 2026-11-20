// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// Fuzz target: decode, in the configurations the viewer uses and OpenJPEG's own OSS-Fuzz target
// never takes (non-strict partial decodes at every reduce level, the viewer's two-pass request),
// plus strict mode and layer limits; the cut length comes from the input (fuzz_common.h).
// - neo-j2k (stage 1c) decodes every input, with no filter: from memory into the caller's planes,
//   under the Second Life limits with 64 MiB of working memory (it checks them from the headers
//   before it allocates), in the input's mode, reduce level (clamped in viewer mode, as the viewer
//   asks) and layer limit. The planes are sized from the header once nj2k_get_decoded_dimensions()
//   accepts it under the limits, as a host does; a header it refuses must be refused by the decode
//   too, with the same status. Every allocation goes through a counting hook, which aborts when the
//   codec holds more than twice the working-memory limit plus its header state (64 bytes for each
//   input byte, plus 256 KiB), or does not give back everything it took.
// - the oracle decodes the same request under the same limits (the Second Life profile with
//   max_memory at 64 MiB, set with the fork's opj_decoder_set_limits), and so does neo-j2k through the
//   same request code (the test backends); only a stream that would make OpenJPEG allocate a
//   tile-part's declared length beyond 64 MiB, which its limits do not bound, is kept from it (OD-3,
//   fuzz_common.h). The two must give the same outcome, image and, in viewer mode, 8-bit copy,
//   unless one of them refused for memory (each counts its own structures against the limit), the
//   codestream shows a departure doc/departures.md lists (departures.h), or the images differ only
//   where one clamps a sample to its component's minimum and the other to its maximum (DP-2, the DC
//   shift's edges).
// - Built with NJ2K_FUZZ_THREADS (nj2k_fuzz_decode_threads_replay, stage 5), it decodes each input
//   again through a pool of threads with every stage of the decode split however small the image
//   (2 and 4 parts), and aborts unless the statuses, the planes and every message equal the
//   one-thread decode's, then compares with the oracle as the plain target does. The
//   ThreadSanitizer job replays the seeds and regressions through it.
#include <neoj2k/neoj2k.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#endif

#include "departures.h"
#include "fuzz_common.h"

#if defined(NJ2K_FUZZ_THREADS)
#include "../src/parallel.h"
#include "thread_pool.h"
#endif

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

constexpr std::uint64_t kWorkingMemory = nj2k::fuzz::kDecodeWorkingMemory;

// What neo_decode gave: each call's status, the planes after each decode, and every message.
struct NeoResult {
    std::vector<nj2k_status_t> statuses;
    std::vector<std::uint8_t> planes;
    std::string messages;
};

#if defined(NJ2K_FUZZ_THREADS)
void collect(const char* m, void* user)
{
    *static_cast<std::string*>(user) += m;
}
#endif

// neo-j2k from memory, bounded: the decode itself, its memory, and that it gives it all back. With
// an executor (NJ2K_FUZZ_THREADS), every stage of the decode goes through it.
NeoResult neo_decode(const nj2k::fuzz::Input& in, const nj2k_executor_t* executor = nullptr)
{
    NeoResult result;
    Budget budget;
    {
        nj2k_allocator_t hook{sizeof(nj2k_allocator_t), Budget::alloc, Budget::release, &budget};
        nj2k_codec_t* codec = nj2k_create_decompress_with_allocator(NJ2K_CODEC_J2K, &hook);
        if (!codec) {
            std::abort();
        }
#if defined(NJ2K_FUZZ_THREADS)
        nj2k_set_info_handler(codec, collect, &result.messages);
        nj2k_set_warning_handler(codec, collect, &result.messages);
        nj2k_set_error_handler(codec, collect, &result.messages);
        if (executor && (!nj2k_decoder_set_executor(codec, executor) ||
                         !nj2k::detail::set_parallel_thresholds(codec, 0, 0, 0))) {
            std::abort();
        }
#else
        (void)executor;
#endif
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
        const nj2k_output_t none{sizeof(nj2k_output_t), NJ2K_SAMPLE_U8, 0, nullptr};
        if (!nj2k_read_header_memory(codec, in.codestream.data(), in.codestream.size(), &image)) {
            // the decode reads the header again, and refuses it as the header read did
            const nj2k_status_t header = nj2k_get_status(codec);
            result.statuses.push_back(header);
            if (nj2k_decode_memory(codec, in.codestream.data(), in.codestream.size(), &none) ||
                nj2k_get_status(codec) != header) {
                std::fprintf(stderr, "nj2k_decode_memory: status %d where the header read gave %d\n",
                             int(nj2k_get_status(codec)), int(header));
                std::abort();
            }
        } else if (!nj2k_get_decoded_dimensions(codec, &image)) {
            // above the limits at the decoded resolution: no planes for it, and the decode refuses it
            // as the dimensions check did, before it writes anything
            const nj2k_status_t limit = nj2k_get_status(codec);
            result.statuses.push_back(limit);
            if (nj2k_decode_memory(codec, in.codestream.data(), in.codestream.size(), &none) ||
                nj2k_get_status(codec) != limit) {
                std::fprintf(stderr, "nj2k_decode_memory: status %d where nj2k_get_decoded_dimensions gave %d\n",
                             int(nj2k_get_status(codec)), int(limit));
                std::abort();
            }
        } else {
            // 8-bit planes, one after another (the limits checked: at most 2,048 x 2,048 x 8), with
            // guard bytes before, between and after them: a write past a plane that stays inside
            // the buffer is caught as well as one past its end (AddressSanitizer)
            constexpr std::size_t kGuard = 64;
            constexpr std::uint8_t kGuardByte = 0xA5;
            std::size_t total = kGuard;
            for (std::uint32_t c = 0; c < image.numcomps; ++c) {
                total += std::size_t(image.comps[c].w) * image.comps[c].h + kGuard;
            }
            std::vector<std::uint8_t> raw(total, kGuardByte);
            std::vector<nj2k_plane_t> planes(image.numcomps);
            std::vector<std::size_t> guards{0};
            std::size_t at = kGuard;
            for (std::uint32_t c = 0; c < image.numcomps; ++c) {
                planes[c] = nj2k_plane_t{raw.data() + at, std::int64_t(image.comps[c].w), 1, c};
                at += std::size_t(image.comps[c].w) * image.comps[c].h;
                guards.push_back(at);
                at += kGuard;
            }
            auto check_guards = [&] {
                for (std::size_t g : guards) {
                    for (std::size_t i = 0; i < kGuard; ++i) {
                        if (raw[g + i] != kGuardByte) {
                            std::fprintf(stderr, "nj2k_decode_memory wrote outside the planes, at byte %zu\n", g + i);
                            std::abort();
                        }
                    }
                }
            };
            const nj2k_output_t out{sizeof(nj2k_output_t), NJ2K_SAMPLE_U8, image.numcomps, planes.data()};
            const bool ok = nj2k_decode_memory(codec, in.codestream.data(), in.codestream.size(), &out) != NJ2K_FALSE;
            const nj2k_status_t st = nj2k_get_status(codec);
            if (ok != (st >= 0)) {
                std::fprintf(stderr, "nj2k_decode_memory returned %d with status %d\n", int(ok), int(st));
                std::abort();
            }
            check_guards();
            result.statuses.push_back(st);
            result.planes = raw;
            // and again, from the header the codec holds
            (void)nj2k_decode_memory(codec, in.codestream.data(), in.codestream.size(), &out);
            check_guards();
            result.statuses.push_back(nj2k_get_status(codec));
            result.planes.insert(result.planes.end(), raw.begin(), raw.end());
        }
        nj2k_destroy_codec(codec);
    }
    const std::size_t bound = std::size_t(2 * kWorkingMemory) + 64 * in.codestream.size() + 256 * 1024;
    if (budget.live != 0 || budget.peak > bound) {
        std::fprintf(stderr, "decode: %zu bytes at most (bound %zu), %zu not given back, from %zu input bytes\n",
                     budget.peak, bound, budget.live, in.codestream.size());
        std::abort();
    }
    return result;
}

const char* outcome_text(const nj2k::test::Outcome& o)
{
    return o.ok ? "decoded" : o.stage.c_str();
}

// A refusal for memory: the decoder's memory limit, or an allocation it could not make. The two
// libraries count different structures against max_memory (OpenJPEG the tile parameters and each
// tile's structures as it builds them, neo-j2k its working memory, doc/api.md), so they can refuse
// at different sizes, which says nothing about the decode. Their other limits (tiles, components,
// layers, the decoded side) are the same and must refuse alike.
bool memory_refusal(const nj2k::test::Outcome& o)
{
    return !o.ok && o.last_error.find("memory") != std::string::npos;
}

// DP-2, the DC shift's edges: both decode the same image but for samples that one decoder clamps to
// their component's minimum and the other to its maximum (OpenJPEG's 5/3 shift adds in 32 bits and
// wraps; its MSVC build rounds a 9/7 sample of 2^31 to INT_MIN; neo-j2k saturates). The request is
// decoded again with the planes kept, only when the hashes differ.
bool dc_shift_edges(nj2k::test::Backend& oracle, nj2k::test::Backend& neo, nj2k::test::Request rq)
{
    rq.capture = true;
    const nj2k::test::Outcome a = oracle.decode(rq), b = neo.decode(rq);
    if (!a.ok || !b.ok || a.x0 != b.x0 || a.y0 != b.y0 || a.x1 != b.x1 || a.y1 != b.y1 || a.numcomps != b.numcomps ||
        a.color_space != b.color_space || a.comps.size() != b.comps.size() || a.planes.size() != b.planes.size() ||
        a.planes.size() != a.comps.size()) {
        return false;
    }
    bool edges = false;
    for (std::size_t c = 0; c < a.comps.size(); ++c) {
        const nj2k::test::Component &p = a.comps[c], &q = b.comps[c];
        if (p.w != q.w || p.h != q.h || p.dx != q.dx || p.dy != q.dy || p.x0 != q.x0 || p.y0 != q.y0 ||
            p.prec != q.prec || p.sgnd != q.sgnd || p.factor != q.factor || p.resno_decoded != q.resno_decoded ||
            a.planes[c].size() != b.planes[c].size() || p.prec == 0 || p.prec > 31) {
            return false;
        }
        const std::int64_t lo = p.sgnd ? -(std::int64_t(1) << (p.prec - 1)) : 0;
        const std::int64_t hi = p.sgnd ? (std::int64_t(1) << (p.prec - 1)) - 1 : (std::int64_t(1) << p.prec) - 1;
        for (std::size_t i = 0; i < a.planes[c].size(); ++i) {
            const std::int64_t x = a.planes[c][i], y = b.planes[c][i];
            if (x == y) {
                continue;
            }
            if (!((x == lo && y == hi) || (x == hi && y == lo))) {
                return false;
            }
            edges = true;
        }
    }
    return edges;
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
#if defined(NJ2K_FUZZ_THREADS)
    // threads: the one-thread decode against the same through 2 and 4 parts of a pool shared by the run
    static nj2k::test::ThreadPool pool(3);
    const NeoResult one = neo_decode(in);
    for (const std::uint32_t parts : {2u, 4u}) {
        const nj2k_executor_t ex = pool.executor(parts);
        const NeoResult many = neo_decode(in, &ex);
        if (many.statuses != one.statuses || many.planes != one.planes || many.messages != one.messages) {
            std::fprintf(stderr,
                         "decode through %u parts differs from one thread's (%s, reduce %d, %zu bytes): %s%s%s\n",
                         parts, nj2k::test::mode_name(in.mode), in.reduce, in.codestream.size(),
                         many.statuses != one.statuses ? "statuses " : "", many.planes != one.planes ? "planes " : "",
                         many.messages != one.messages ? "messages" : "");
            std::abort();
        }
    }
#else
    neo_decode(in);
#endif
    ++nj2k::fuzz::exercised();
    if (in.mode != nj2k::test::Mode::Strict && nj2k::fuzz::oracle_psot_bomb(in.codestream)) {
        return 0;
    }
    const nj2k::test::Request rq = nj2k::fuzz::decode_request(in);  // the limits neo_decode has
    const nj2k::test::Outcome a = oracle->decode(rq), b = neo->decode(rq);
    const bool same = a.ok == b.ok && a.hash == b.hash && a.u8_hash == b.u8_hash;
    if (same || memory_refusal(a) || memory_refusal(b) ||
        nj2k::fuzz::decode_departure(in.codestream, static_cast<std::uint32_t>(in.reduce)) ||
        (a.ok && b.ok && dc_shift_edges(*oracle, *neo, rq))) {
        return 0;
    }
    std::fprintf(stderr,
                 "decode differs from the oracle's (%s, reduce %d, layers %d, %zu bytes), with no departure to "
                 "explain it\n  oracle: %s %s\n  neo-j2k: %s %s\n",
                 nj2k::test::mode_name(in.mode), in.reduce, rq.layers, rq.size, outcome_text(a), a.last_error.c_str(),
                 outcome_text(b), b.last_error.c_str());
    std::abort();
}

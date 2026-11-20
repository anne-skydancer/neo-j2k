// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The decoder behind nj2k_codec_t: its allocator and arenas, its message handlers, its settings
// (reduce factor and policy, layers, strict mode, limits), the header model once it is read, the
// memory it keeps between decodes, and the status of its last call. It holds no global state; one
// decoder serves one thread at a time, and can decode codestream after codestream from memory
// (nj2k_decode_memory) reusing its memory and, for the same main header, the header model.
#pragma once

#include <cstddef>
#include <cstdint>

#include <neoj2k/neoj2k.h>

#include "arena.h"
#include "header.h"
#include "jp2.h"
#include "memory.h"
#include "messages.h"
#include "parallel.h"
#include "stream.h"
#include "t1.h"
#include "work_arena.h"

namespace nj2k::detail {

// A buffer the decoder keeps between decodes and grows when a decode needs more.
struct Scratch {
    void* p = nullptr;
    std::size_t size = 0;
    bool ensure(const Allocator& a, std::size_t n) noexcept
    {
        if (n <= size) {
            return true;
        }
        release(a);
        p = a.allocate(n, 64);
        if (!p) {
            return false;
        }
        size = n;
        return true;
    }
    // As ensure, but a failed allocation keeps the buffer there was (the threaded path's larger
    // needs, which fall back to one thread when they cannot be met)
    bool grow(const Allocator& a, std::size_t n) noexcept
    {
        if (n <= size) {
            return true;
        }
        void* q = a.allocate(n, 64);
        if (!q) {
            return false;
        }
        release(a);
        p = q;
        size = n;
        return true;
    }
    void release(const Allocator& a) noexcept
    {
        if (p) {
            a.release(p, size);
        }
        p = nullptr;
        size = 0;
    }
};

// The Tier-1 scratches of a threaded tile's parts (stage 5), kept between decodes as the decoder's
// own T1 is. Each is 256 bytes (sizeof(T1)), in an array aligned to 64 bytes, so two parts' scratches
// never share a cache line.
struct T1Parts {
    T1* t1 = nullptr;
    std::uint32_t count = 0;
    // at least parts scratches, each ready (its fixed buffers); false when the allocator refuses
    bool ensure(const Allocator& a, std::uint32_t parts) noexcept;
    void release(const Allocator& a) noexcept;
    std::size_t bytes() const noexcept;
};

struct Decoder {
    static constexpr std::uint32_t kMagic = 0x4E4A3244u;  // "NJ2D"
    enum class State : std::uint8_t { created, header, decoded, failed };
    // memory kept between decodes unless the host says otherwise (nj2k_decoder_set_retained_bytes)
    static constexpr std::size_t kDefaultRetain = 8u << 20;

    std::uint32_t magic = kMagic;
    Allocator allocator;
    Arena arena;       // the header model (and the JP2 boxes)
    Arena tile_arena;  // tile-part header markers, per decode
    WorkArena work;    // a decode's layout and walk state
    T1 t1;
    Scratch planes;    // a tile's decoded samples, every component
    Scratch dwt;       // the inverse wavelet's lines
    Scratch input;     // the bytes of a stream decode
    std::size_t retain = kDefaultRetain;
    Messages messages;
    nj2k_status_t status = NJ2K_OK;
    NJ2K_CODEC_FORMAT format = NJ2K_CODEC_J2K;
    State state = State::created;
    // settings
    std::uint32_t reduce = 0;  // the reduce factor as asked (cp_reduce, nj2k_set_decoded_resolution_factor)
    std::uint32_t layers = 0;  // cp_layer: layers to decode, 0 for all
    bool dump = false;         // NJ2K_DPARAMETERS_DUMP_FLAG
    bool ignore_pclr = false;  // NJ2K_DPARAMETERS_IGNORE_PCLR_CMAP_CDEF_FLAG
    bool strict = true;        // OpenJPEG's default
    // strict mode was set through the API at some point: OpenJPEG then never applies its TNsot
    // correction (opj_j2k_decoder_set_strict_mode sets m_nb_tile_parts_correction_checked)
    bool tnsot_check_off = false;
    std::uint32_t reduce_policy = NJ2K_REDUCE_EXACT;
    nj2k_decoder_limits_t limits = {0, 0, 0, 0, 0};
    // once the header is read
    MainHeader* header = nullptr;
    Jp2Header* jp2 = nullptr;
    std::uint32_t factor = 0;          // the reduce factor in effect, after the policy
    std::uint32_t* selected = nullptr;  // nj2k_set_decoded_components, in the caller's order
    std::uint32_t nselected = 0;
    bool* selected_mask = nullptr;     // null when every component is decoded
    // where the selection is kept: made by the first selection, numcomps entries each, and
    // overwritten by every later one, so repeated calls take no more memory
    std::uint32_t* selection_order = nullptr;
    bool* selection_mask = nullptr;
    // a header read from memory (nj2k_read_header_memory): its main header's bytes, kept so that a
    // later codestream that starts with the same ones reuses the model; and the image the header
    // gives, in the arena
    std::uint8_t* header_bytes = nullptr;
    std::size_t header_bytes_size = 0;
    nj2k_image_comp_t* image_comps = nullptr;
    bool header_from_memory = false;   // nj2k_read_header_memory() read the header
    nj2k_decoder_limits_t header_limits = {0, 0, 0, 0, 0};  // the limits the kept header was read under
    // the working memory of a decode of the header held, every component decoded, at
    // estimate_factor (decode_working_memory, computed once per header and factor; UINT32_MAX: none)
    std::uint64_t estimate = 0;
    std::uint32_t estimate_factor = UINT32_MAX;
    // threads in one image (stage 5): the host's executor, the parts' Tier-1 scratches and the
    // code-blocks' costs of a threaded tile; last, so the fields above keep their places
    nj2k_executor_t executor = {0, 0, nullptr, nullptr};
    T1Parts t1_parts;
    Scratch blocks;
    // the sizes a decode splits from (parallel.h; set_parallel_thresholds for the tests)
    std::uint64_t parallel_tile = kParallelTileSamples;
    std::uint64_t parallel_level = kParallelLevelArea;
    std::uint64_t parallel_output = kParallelOutputSamples;
    std::uint64_t parallel_clear = kParallelClearBytes;

    static Decoder* from(nj2k_codec_t* codec) noexcept
    {
        auto* d = reinterpret_cast<Decoder*>(codec);
        return d && d->magic == kMagic ? d : nullptr;
    }

    // The executor as a decode's stages use it: false without one (or with a concurrency below 2).
    bool jobs(Jobs& out) const noexcept;
    // Memory kept for the next decode: what is above retain goes back to the allocator.
    void trim_after_decode() noexcept;
    void release_all() noexcept;
    std::size_t kept_bytes() const noexcept;
};

}  // namespace nj2k::detail

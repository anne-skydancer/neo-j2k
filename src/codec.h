// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The decoder behind nj2k_codec_t: its allocator and arena, its message handlers, its settings
// (reduce factor and policy, layers, strict mode, limits), the header model once it is read, and
// the status of its last call. It holds no global state; one decoder serves one thread at a time.
#pragma once

#include <cstdint>

#include <neoj2k/neoj2k.h>

#include "arena.h"
#include "header.h"
#include "jp2.h"
#include "memory.h"
#include "messages.h"
#include "stream.h"

namespace nj2k::detail {

struct Decoder {
    static constexpr std::uint32_t kMagic = 0x4E4A3244u;  // "NJ2D"
    enum class State : std::uint8_t { created, header, failed };

    std::uint32_t magic = kMagic;
    Allocator allocator;
    Arena arena;
    Messages messages;
    nj2k_status_t status = NJ2K_OK;
    NJ2K_CODEC_FORMAT format = NJ2K_CODEC_J2K;
    State state = State::created;
    // settings
    std::uint32_t reduce = 0;  // the reduce factor as asked (cp_reduce, nj2k_set_decoded_resolution_factor)
    std::uint32_t layers = 0;  // cp_layer: layers to decode, 0 for all
    bool dump = false;         // NJ2K_DPARAMETERS_DUMP_FLAG
    bool strict = true;        // OpenJPEG's default
    std::uint32_t reduce_policy = NJ2K_REDUCE_EXACT;
    nj2k_decoder_limits_t limits = {0, 0, 0, 0, 0};
    // once the header is read
    MainHeader* header = nullptr;
    Jp2Header* jp2 = nullptr;
    std::uint32_t factor = 0;          // the reduce factor in effect, after the policy
    std::uint32_t* selected = nullptr;  // nj2k_set_decoded_components, in the caller's order
    std::uint32_t nselected = 0;
    bool* selected_mask = nullptr;

    static Decoder* from(nj2k_codec_t* codec) noexcept
    {
        auto* d = reinterpret_cast<Decoder*>(codec);
        return d && d->magic == kMagic ? d : nullptr;
    }
};

}  // namespace nj2k::detail

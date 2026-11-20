// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The encoder behind nj2k_codec_t (stage 6a): its allocator, message handlers, the settings
// nj2k_setup_encoder took, and the memory of one encode. It shares the codec handle with the
// decoder: both start with a 32-bit magic, and Decoder::from and Encoder::from each answer only for
// their own. It holds no global state; one encoder serves one image, from one thread at a time.
#pragma once

#include <cstddef>
#include <cstdint>

#include <neoj2k/neoj2k.h>

#include "arena.h"
#include "enc_tile.h"
#include "memory.h"
#include "messages.h"
#include "stream.h"

namespace nj2k::detail {

struct Encoder {
    static constexpr std::uint32_t kMagic = 0x4E4A3245u;  // "NJ2E"
    enum class State : std::uint8_t { created, setup, started, encoded, finished, failed };

    // what the core encoder takes (doc/api.md, "The encoder")
    static constexpr std::uint32_t kMaxSide = 4096;
    static constexpr std::uint64_t kMaxSamples = std::uint64_t(1) << 26;
    static constexpr std::uint32_t kMaxComponents = 8;
    static constexpr std::uint32_t kMaxLayers = 100;
    static constexpr std::size_t kMaxComment = 65535 - 4;  // Lcom = length + 4 in 16 bits

    std::uint32_t magic = kMagic;
    Allocator allocator;
    Arena arena;  // one encode's tile model, buffers and header; released when the encode ends
    Messages messages;
    nj2k_status_t status = NJ2K_OK;
    State state = State::created;

    // the settings, from nj2k_setup_encoder
    EncParams params = {};
    std::uint32_t width = 0, height = 0;
    char* comment = nullptr;  // the COM marker's text (the allocator's), without its terminator
    std::size_t comment_len = 0;
    // after nj2k_start_compress
    std::uint8_t* tile_buf = nullptr;     // SOT, SOD and the packets: OpenJPEG's m_encoded_tile_data
    std::uint32_t tile_size = 0;
    void* planes[Encoder::kMaxComponents] = {};  // the samples of each component (arena), width x height

    static Encoder* from(nj2k_codec_t* codec) noexcept
    {
        auto* e = reinterpret_cast<Encoder*>(codec);
        return e && e->magic == kMagic ? e : nullptr;
    }
    void release_all() noexcept;
};

}  // namespace nj2k::detail

// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// C++20 wrapper over the C API (draft). Header-only: owning handles that release themselves,
// move-only, everything noexcept. It calls nothing but the C ABI, so it is safe across a DLL
// boundary and with either C runtime.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>

#include "neoj2k.h"

namespace nj2k {

struct CodecDeleter {
    void operator()(nj2k_codec_t* codec) const noexcept { nj2k_destroy_codec(codec); }
};
struct StreamDeleter {
    void operator()(nj2k_stream_t* stream) const noexcept { nj2k_stream_destroy(stream); }
};
struct ImageDeleter {
    void operator()(nj2k_image_t* image) const noexcept { nj2k_image_destroy(image); }
};
struct CodestreamInfoDeleter {
    void operator()(nj2k_codestream_info_v2_t* info) const noexcept { nj2k_destroy_cstr_info(&info); }
};

using Codec = std::unique_ptr<nj2k_codec_t, CodecDeleter>;
using Stream = std::unique_ptr<nj2k_stream_t, StreamDeleter>;
using Image = std::unique_ptr<nj2k_image_t, ImageDeleter>;
using CodestreamInfo = std::unique_ptr<nj2k_codestream_info_v2_t, CodestreamInfoDeleter>;

[[nodiscard]] inline Codec make_decoder(NJ2K_CODEC_FORMAT format = NJ2K_CODEC_J2K) noexcept
{
    return Codec(nj2k_create_decompress(format));
}

[[nodiscard]] inline Codec make_encoder(NJ2K_CODEC_FORMAT format = NJ2K_CODEC_J2K) noexcept
{
    return Codec(nj2k_create_compress(format));
}

[[nodiscard]] inline nj2k_dparameters_t default_decoder_parameters() noexcept
{
    nj2k_dparameters_t p;
    nj2k_set_default_decoder_parameters(&p);
    return p;
}

[[nodiscard]] inline std::string_view version() noexcept { return nj2k_version(); }
[[nodiscard]] inline std::string_view engine_string() noexcept { return nj2k_engine_string(); }
[[nodiscard]] inline bool cpu_supported() noexcept { return nj2k_cpu_supported() != 0; }

// An input stream over borrowed bytes. The reader must outlive the stream it makes; the bytes are
// never copied by the reader.
class MemoryReader {
public:
    explicit MemoryReader(std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}
    MemoryReader(const MemoryReader&) = delete;
    MemoryReader& operator=(const MemoryReader&) = delete;

    [[nodiscard]] Stream make_stream(std::size_t buffer_size = NJ2K_J2K_STREAM_CHUNK_SIZE) noexcept
    {
        Stream stream(nj2k_stream_create(buffer_size, NJ2K_TRUE));
        if (!stream) {
            return stream;
        }
        pos_ = 0;
        nj2k_stream_set_user_data(stream.get(), this, nullptr);
        nj2k_stream_set_user_data_length(stream.get(), bytes_.size());
        nj2k_stream_set_read_function(stream.get(), &MemoryReader::read);
        nj2k_stream_set_skip_function(stream.get(), &MemoryReader::skip);
        nj2k_stream_set_seek_function(stream.get(), &MemoryReader::seek);
        return stream;
    }

private:
    static NJ2K_SIZE_T read(void* buffer, NJ2K_SIZE_T n, void* user) noexcept
    {
        auto* self = static_cast<MemoryReader*>(user);
        if (self->pos_ >= self->bytes_.size()) {
            return static_cast<NJ2K_SIZE_T>(-1);
        }
        const std::size_t left = self->bytes_.size() - self->pos_;
        const std::size_t take = n < left ? n : left;
        std::memcpy(buffer, self->bytes_.data() + self->pos_, take);
        self->pos_ += take;
        return take;
    }
    static NJ2K_OFF_T skip(NJ2K_OFF_T n, void* user) noexcept
    {
        auto* self = static_cast<MemoryReader*>(user);
        const auto size = static_cast<NJ2K_OFF_T>(self->bytes_.size());
        const NJ2K_OFF_T to = static_cast<NJ2K_OFF_T>(self->pos_) + n;
        if (to < 0 || to > size) {
            self->pos_ = static_cast<std::size_t>(to < 0 ? 0 : size);
            return -1;
        }
        self->pos_ = static_cast<std::size_t>(to);
        return n;
    }
    static NJ2K_BOOL seek(NJ2K_OFF_T to, void* user) noexcept
    {
        auto* self = static_cast<MemoryReader*>(user);
        if (to < 0 || to > static_cast<NJ2K_OFF_T>(self->bytes_.size())) {
            return NJ2K_FALSE;
        }
        self->pos_ = static_cast<std::size_t>(to);
        return NJ2K_TRUE;
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t pos_ = 0;
};

}  // namespace nj2k

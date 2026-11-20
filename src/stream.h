// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The stream behind nj2k_stream_t: OpenJPEG's user stream (a buffer refilled through the host's
// read function, skip and seek functions, user data and its length), reading side, and the writing
// side of the encoder (stage 6a). One difference from OpenJPEG: a read function that returns 0 ends
// the stream, and a write function that returns 0 fails it, where OpenJPEG would call them again for
// ever.
#pragma once

#include <cstddef>
#include <cstdint>

#include <neoj2k/neoj2k.h>

namespace nj2k::detail {

struct Stream {
    static constexpr std::uint32_t kMagic = 0x4E4A3253u;  // "NJ2S"

    std::uint32_t magic = kMagic;
    bool input = true;
    bool ended = false;  // the read function reported the end
    std::uint8_t* buffer = nullptr;
    std::size_t buffer_size = 0;
    std::size_t pos = 0;     // buffer[pos, filled) holds bytes not handed out yet
    std::size_t filled = 0;
    std::uint64_t offset = 0;  // bytes handed out (read or skipped) since the start
    void* user = nullptr;
    nj2k_stream_free_user_data_fn free_user = nullptr;
    std::uint64_t length = 0;  // the host's nj2k_stream_set_user_data_length, 0 when not given
    nj2k_stream_read_fn read_fn = nullptr;
    nj2k_stream_write_fn write_fn = nullptr;
    nj2k_stream_skip_fn skip_fn = nullptr;
    nj2k_stream_seek_fn seek_fn = nullptr;

    static Stream* from(nj2k_stream_t* s) noexcept
    {
        auto* p = reinterpret_cast<Stream*>(s);
        return p && p->magic == kMagic ? p : nullptr;
    }

    // Reads up to n bytes; fewer only when the stream ends.
    std::size_t read(std::uint8_t* dst, std::size_t n) noexcept;
    // Skips n bytes; false when the stream ends first (the bytes up to its end are skipped).
    bool skip(std::uint64_t n) noexcept;
    // The writing side (opj_stream_write_data, opj_stream_flush): bytes into the buffer, handed to the
    // host's write function when it is full; offset counts the bytes accepted. False when the write
    // function fails (returns -1 or 0).
    bool write(const std::uint8_t* src, std::size_t n) noexcept;
    bool flush() noexcept;
    // Bytes left before the length the host gave, or -1 when it gave none.
    std::int64_t bytes_left() const noexcept
    {
        return length ? (length >= offset ? std::int64_t(length - offset) : 0) : -1;
    }
};

}  // namespace nj2k::detail

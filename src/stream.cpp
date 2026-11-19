// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// nj2k_stream_*: OpenJPEG's stream API (cio.c) with its behaviour: a stream made for reading takes
// read, skip and seek functions, one made for writing a write function; user data and its free
// function; file streams over the C runtime's FILE. The reading side serves the decoder.
#include "stream.h"

#include <cstdio>
#include <cstring>
#include <new>

#include "memory.h"

namespace nj2k::detail {

namespace {

constexpr std::size_t kEnd = static_cast<std::size_t>(-1);

}  // namespace

std::size_t Stream::read(std::uint8_t* dst, std::size_t n) noexcept
{
    std::size_t done = 0;
    while (done < n) {
        if (pos < filled) {
            const std::size_t take = filled - pos < n - done ? filled - pos : n - done;
            std::memcpy(dst + done, buffer + pos, take);
            pos += take;
            done += take;
            continue;
        }
        if (ended || !read_fn) {
            ended = true;
            break;
        }
        const std::size_t want = n - done;
        if (want >= buffer_size) {
            // as large as the buffer: straight into the destination
            const std::size_t got = read_fn(dst + done, want, user);
            if (got == kEnd || got == 0) {
                ended = true;
                break;
            }
            done += got < want ? got : want;
            continue;
        }
        pos = filled = 0;
        const std::size_t got = read_fn(buffer, buffer_size, user);
        if (got == kEnd || got == 0) {
            ended = true;
            break;
        }
        filled = got < buffer_size ? got : buffer_size;
    }
    offset += done;
    return done;
}

bool Stream::skip(std::uint64_t n) noexcept
{
    const std::size_t in_buffer = filled - pos;
    if (n <= in_buffer) {
        pos += static_cast<std::size_t>(n);
        offset += n;
        return true;
    }
    pos = filled = 0;
    offset += in_buffer;
    n -= in_buffer;
    if (ended) {
        return false;
    }
    if (length && (offset > length || n > length - offset)) {
        // past the end the host gave: skip to it, as OpenJPEG does
        const std::uint64_t left = length > offset ? length - offset : 0;
        if (left && skip_fn && skip_fn(static_cast<NJ2K_OFF_T>(left), user) == static_cast<NJ2K_OFF_T>(left)) {
            offset += left;
        }
        ended = true;
        return false;
    }
    while (n > 0) {
        if (!skip_fn) {
            ended = true;
            return false;
        }
        const NJ2K_OFF_T step = n > std::uint64_t(INT64_MAX) ? INT64_MAX : static_cast<NJ2K_OFF_T>(n);
        const NJ2K_OFF_T got = skip_fn(step, user);
        if (got <= 0 || got > step) {
            ended = true;
            return false;
        }
        offset += static_cast<std::uint64_t>(got);
        n -= static_cast<std::uint64_t>(got);
    }
    return true;
}

}  // namespace nj2k::detail

using nj2k::detail::Stream;

namespace {

Stream* new_stream(NJ2K_SIZE_T buffer_size, NJ2K_BOOL is_input) noexcept
{
    void* mem = nj2k::detail::heap_calloc(1, sizeof(Stream));
    if (!mem) {
        return nullptr;
    }
    auto* s = new (mem) Stream();
    s->input = is_input != 0;
    if (buffer_size) {
        // filled by the read function before any byte of it is read: no need to clear it, which a
        // host that sizes the buffer to the whole file (the viewer) would pay on every header read,
        // and OpenJPEG does not either (opj_malloc)
        s->buffer = static_cast<std::uint8_t*>(nj2k::detail::heap_malloc(buffer_size));
        if (!s->buffer) {
            nj2k::detail::heap_free(s);
            return nullptr;
        }
        s->buffer_size = buffer_size;
    }
    return s;
}

// The file functions of nj2k_stream_create_file_stream, as OpenJPEG's (cio.c, openjpeg.c).
NJ2K_SIZE_T file_read(void* buffer, NJ2K_SIZE_T n, void* user)
{
    const std::size_t got = std::fread(buffer, 1, n, static_cast<std::FILE*>(user));
    return got ? got : static_cast<NJ2K_SIZE_T>(-1);
}

NJ2K_SIZE_T file_write(void* buffer, NJ2K_SIZE_T n, void* user)
{
    return std::fwrite(buffer, 1, n, static_cast<std::FILE*>(user));
}

int file_seek_raw(std::FILE* f, NJ2K_OFF_T to, int whence)
{
#if defined(_WIN32)
    return _fseeki64(f, to, whence);
#else
    return fseeko(f, static_cast<off_t>(to), whence);
#endif
}

NJ2K_OFF_T file_skip(NJ2K_OFF_T n, void* user)
{
    return file_seek_raw(static_cast<std::FILE*>(user), n, SEEK_CUR) ? -1 : n;
}

NJ2K_BOOL file_seek(NJ2K_OFF_T to, void* user)
{
    return file_seek_raw(static_cast<std::FILE*>(user), to, SEEK_SET) ? NJ2K_FALSE : NJ2K_TRUE;
}

void file_close(void* user)
{
    std::fclose(static_cast<std::FILE*>(user));
}

NJ2K_UINT64 file_length(std::FILE* f)
{
#if defined(_WIN32)
    if (_fseeki64(f, 0, SEEK_END) != 0) {
        return 0;
    }
    const __int64 end = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
#else
    if (fseeko(f, 0, SEEK_END) != 0) {
        return 0;
    }
    const off_t end = ftello(f);
    fseeko(f, 0, SEEK_SET);
#endif
    return end > 0 ? static_cast<NJ2K_UINT64>(end) : 0;
}

}  // namespace

extern "C" {

NJ2K_API nj2k_stream_t* NJ2K_CALLCONV nj2k_stream_create(NJ2K_SIZE_T p_buffer_size, NJ2K_BOOL p_is_input)
{
    return reinterpret_cast<nj2k_stream_t*>(new_stream(p_buffer_size, p_is_input));
}

NJ2K_API nj2k_stream_t* NJ2K_CALLCONV nj2k_stream_default_create(NJ2K_BOOL p_is_input)
{
    return nj2k_stream_create(NJ2K_J2K_STREAM_CHUNK_SIZE, p_is_input);
}

NJ2K_API void NJ2K_CALLCONV nj2k_stream_destroy(nj2k_stream_t* p_stream)
{
    Stream* s = Stream::from(p_stream);
    if (!s) {
        return;
    }
    if (s->free_user) {
        s->free_user(s->user);
    }
    nj2k::detail::heap_free(s->buffer);
    s->magic = 0;
    s->~Stream();
    nj2k::detail::heap_free(s);
}

NJ2K_API void NJ2K_CALLCONV nj2k_stream_set_read_function(nj2k_stream_t* p_stream, nj2k_stream_read_fn p_function)
{
    Stream* s = Stream::from(p_stream);
    if (s && s->input) {
        s->read_fn = p_function;
    }
}

NJ2K_API void NJ2K_CALLCONV nj2k_stream_set_write_function(nj2k_stream_t* p_stream, nj2k_stream_write_fn p_function)
{
    Stream* s = Stream::from(p_stream);
    if (s && !s->input) {
        s->write_fn = p_function;
    }
}

NJ2K_API void NJ2K_CALLCONV nj2k_stream_set_skip_function(nj2k_stream_t* p_stream, nj2k_stream_skip_fn p_function)
{
    if (Stream* s = Stream::from(p_stream)) {
        s->skip_fn = p_function;
    }
}

NJ2K_API void NJ2K_CALLCONV nj2k_stream_set_seek_function(nj2k_stream_t* p_stream, nj2k_stream_seek_fn p_function)
{
    if (Stream* s = Stream::from(p_stream)) {
        s->seek_fn = p_function;
    }
}

NJ2K_API void NJ2K_CALLCONV nj2k_stream_set_user_data(nj2k_stream_t* p_stream, void* p_data,
                                                      nj2k_stream_free_user_data_fn p_function)
{
    if (Stream* s = Stream::from(p_stream)) {
        s->user = p_data;
        s->free_user = p_function;
    }
}

NJ2K_API void NJ2K_CALLCONV nj2k_stream_set_user_data_length(nj2k_stream_t* p_stream, NJ2K_UINT64 data_length)
{
    if (Stream* s = Stream::from(p_stream)) {
        s->length = data_length;
    }
}

NJ2K_API nj2k_stream_t* NJ2K_CALLCONV nj2k_stream_create_file_stream(const char* fname, NJ2K_SIZE_T p_buffer_size,
                                                                    NJ2K_BOOL p_is_read_stream)
{
    if (!fname) {
        return nullptr;
    }
    std::FILE* f = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&f, fname, p_is_read_stream ? "rb" : "wb") != 0) {
        f = nullptr;
    }
#else
    f = std::fopen(fname, p_is_read_stream ? "rb" : "wb");
#endif
    if (!f) {
        return nullptr;
    }
    nj2k_stream_t* stream = nj2k_stream_create(p_buffer_size, p_is_read_stream);
    if (!stream) {
        std::fclose(f);
        return nullptr;
    }
    nj2k_stream_set_user_data(stream, f, file_close);
    nj2k_stream_set_user_data_length(stream, file_length(f));
    nj2k_stream_set_read_function(stream, file_read);
    nj2k_stream_set_write_function(stream, file_write);
    nj2k_stream_set_skip_function(stream, file_skip);
    nj2k_stream_set_seek_function(stream, file_seek);
    return stream;
}

NJ2K_API nj2k_stream_t* NJ2K_CALLCONV nj2k_stream_create_default_file_stream(const char* fname,
                                                                            NJ2K_BOOL p_is_read_stream)
{
    return nj2k_stream_create_file_stream(fname, NJ2K_J2K_STREAM_CHUNK_SIZE, p_is_read_stream);
}

}  // extern "C"

// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// A codec's message handlers (nj2k_set_info_handler and its siblings). A message is formatted only
// when a handler takes it, so a host without handlers pays nothing for them (fork audit P3), and
// nothing is shared between codecs: no process-wide handler.
#pragma once

#include <cstdarg>

#include <neoj2k/neoj2k.h>

#if defined(__GNUC__) || defined(__clang__)
#define NJ2K_PRINTF_LIKE(f, a) __attribute__((format(printf, f, a)))
#else
#define NJ2K_PRINTF_LIKE(f, a)
#endif

namespace nj2k::detail {

class Messages {
public:
    enum Kind { Info, Warning, Error };

    void set(Kind kind, nj2k_msg_callback callback, void* user) noexcept
    {
        handler_[kind] = callback;
        user_[kind] = user;
    }
    bool wanted(Kind kind) const noexcept { return handler_[kind] != nullptr; }

    // printf-style; the text ends with a newline, as OpenJPEG's messages do
    void emit(Kind kind, const char* format, ...) const noexcept NJ2K_PRINTF_LIKE(3, 4);
    void vemit(Kind kind, const char* format, va_list args) const noexcept;

private:
    nj2k_msg_callback handler_[3] = {nullptr, nullptr, nullptr};
    void* user_[3] = {nullptr, nullptr, nullptr};
};

}  // namespace nj2k::detail

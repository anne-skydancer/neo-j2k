// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
#include "messages.h"

#include <cstdio>

namespace nj2k::detail {

namespace {

// vsnprintf with standard behaviour. The Universal CRT's inline vsnprintf reads its options from a
// function-local static (__local_stdio_printf_options), a writable global in every object that
// calls it; the library holds none (tools/check_objects.py), so it asks the CRT directly.
int format(char* text, std::size_t size, const char* fmt, va_list args) noexcept
{
#if defined(_UCRT)
    return __stdio_common_vsprintf(_CRT_INTERNAL_PRINTF_STANDARD_SNPRINTF_BEHAVIOR, text, size, fmt, nullptr, args);
#else
    return std::vsnprintf(text, size, fmt, args);
#endif
}

}  // namespace

void Messages::vemit(Kind kind, const char* fmt, va_list args) const noexcept
{
    if (!handler_[kind]) {
        return;
    }
    char text[512];
    if (format(text, sizeof(text), fmt, args) < 0) {
        return;
    }
    handler_[kind](text, user_[kind]);
}

void Messages::emit(Kind kind, const char* fmt, ...) const noexcept
{
    if (!handler_[kind]) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vemit(kind, fmt, args);
    va_end(args);
}

}  // namespace nj2k::detail

// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
#include "memory.h"

#include <cstdlib>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace nj2k::detail {

void* heap_calloc(std::size_t count, std::size_t size) noexcept
{
    if (count == 0 || size == 0) {
        return nullptr;
    }
    return std::calloc(count, size);
}

void heap_free(void* ptr) noexcept
{
    std::free(ptr);
}

void* aligned_alloc(std::size_t size) noexcept
{
    if (size == 0) {
        return nullptr;
    }
#if defined(_WIN32)
    return _aligned_malloc(size, kPlaneAlignment);
#else
    void* ptr = nullptr;
    if (posix_memalign(&ptr, kPlaneAlignment, size) != 0) {
        return nullptr;
    }
    return ptr;
#endif
}

void aligned_free(void* ptr) noexcept
{
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

}  // namespace nj2k::detail

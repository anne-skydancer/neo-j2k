// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
#include "memory.h"

#include <cstdlib>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace nj2k::detail {

namespace {

void* aligned_alloc_crt(std::size_t size, std::size_t alignment) noexcept
{
    if (size == 0) {
        return nullptr;
    }
    if (alignment < alignof(std::max_align_t)) {
        alignment = alignof(std::max_align_t);
    }
#if defined(_WIN32)
    return _aligned_malloc(size, alignment);
#else
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) != 0) {
        return nullptr;
    }
    return ptr;
#endif
}

void aligned_free_crt(void* ptr) noexcept
{
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

void* system_alloc(void*, std::size_t size, std::size_t alignment)
{
    return aligned_alloc_crt(size, alignment);
}

void system_free(void*, void* ptr, std::size_t)
{
    aligned_free_crt(ptr);
}

bool power_of_two(std::size_t v) noexcept
{
    return v != 0 && (v & (v - 1)) == 0;
}

}  // namespace

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

void* plane_alloc(std::size_t size) noexcept
{
    return aligned_alloc_crt(size, kPlaneAlignment);
}

void plane_free(void* ptr) noexcept
{
    aligned_free_crt(ptr);
}

Allocator Allocator::system() noexcept
{
    Allocator a;
    a.alloc_fn = system_alloc;
    a.free_fn = system_free;
    return a;
}

bool Allocator::from_host(const nj2k_allocator_t* host, Allocator& out) noexcept
{
    if (!host || host->struct_size < sizeof(nj2k_allocator_t) || !host->allocate || !host->release) {
        return false;
    }
    out.alloc_fn = host->allocate;
    out.free_fn = host->release;
    out.user = host->user;
    return true;
}

void* Allocator::allocate(std::size_t size, std::size_t alignment) const noexcept
{
    if (size == 0 || !power_of_two(alignment) || alignment > kMaxAlignment) {
        return nullptr;
    }
    void* p = alloc_fn(user, size, alignment);
    // a hook that ignores the alignment would break later SIMD loads: give its block back
    if (p && (reinterpret_cast<std::uintptr_t>(p) & (alignment - 1)) != 0) {
        free_fn(user, p, size);
        return nullptr;
    }
    return p;
}

void Allocator::release(void* ptr, std::size_t size) const noexcept
{
    if (ptr) {
        free_fn(user, ptr, size);
    }
}

}  // namespace nj2k::detail

// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// Heap entry points of the library. What the API hands to the caller (images, streams, codestream
// information) comes from the C runtime's heap through heap_* and plane_*, as OpenJPEG's does,
// since the functions that destroy it take no codec. A codec's own memory comes from its Allocator:
// the host's hook (nj2k_allocator_t) or, without one, the same C runtime heap.
#pragma once

#include <cstddef>
#include <cstdint>

#include <neoj2k/neoj2k.h>

namespace nj2k::detail {

// Plain blocks: nullptr for a zero size, as OpenJPEG's opj_calloc.
void* heap_calloc(std::size_t count, std::size_t size) noexcept;
void heap_free(void* ptr) noexcept;

// Sample planes: 64-byte aligned (a cache line, and every SIMD width neo-j2k uses); nullptr for a
// zero size, as OpenJPEG's opj_aligned_malloc. Free with plane_free only.
inline constexpr std::size_t kPlaneAlignment = 64;
void* plane_alloc(std::size_t size) noexcept;
void plane_free(void* ptr) noexcept;

// A codec's allocator: the host's hook, or the C runtime heap. Alignment is a power of two up to
// kMaxAlignment; release gets back the size that was asked for.
inline constexpr std::size_t kMaxAlignment = 4096;

struct Allocator {
    void* (*alloc_fn)(void* user, std::size_t size, std::size_t alignment) = nullptr;
    void (*free_fn)(void* user, void* ptr, std::size_t size) = nullptr;
    void* user = nullptr;

    // The C runtime heap.
    static Allocator system() noexcept;
    // The host's hook; false when it is incomplete (struct size too small, no allocate or release).
    static bool from_host(const nj2k_allocator_t* host, Allocator& out) noexcept;

    // nullptr for a zero size, a bad alignment, or a failed allocation.
    void* allocate(std::size_t size, std::size_t alignment) const noexcept;
    void release(void* ptr, std::size_t size) const noexcept;
};

}  // namespace nj2k::detail

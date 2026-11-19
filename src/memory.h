// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// Heap entry points of the library. Everything neo-j2k allocates goes through here, so the
// allocator hook (stage 1b) has one place to land.
#pragma once

#include <cstddef>

namespace nj2k::detail {

// Plain blocks: nullptr for a zero size, as OpenJPEG's opj_calloc.
void* heap_calloc(std::size_t count, std::size_t size) noexcept;
void heap_free(void* ptr) noexcept;

// Sample planes: 64-byte aligned (a cache line, and every SIMD width neo-j2k uses); nullptr for a
// zero size, as OpenJPEG's opj_aligned_malloc. Free with aligned_free only.
inline constexpr std::size_t kPlaneAlignment = 64;
void* aligned_alloc(std::size_t size) noexcept;
void aligned_free(void* ptr) noexcept;

}  // namespace nj2k::detail

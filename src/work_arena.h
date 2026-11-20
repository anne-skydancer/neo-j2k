// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// A decoder's working memory for one decode: blocks from its Allocator, handed out by bumping a
// pointer, rewound to a mark after each tile and to the start after each decode, and kept for the
// next one. After a decode that needed more than one block the blocks are merged into one of their
// total size (when it is within what the decoder keeps), so a later decode that needs no more
// makes no heap call at all (the stage 1c gate). A budget (the decoder limits' max_memory) bounds
// what it reserves.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

#include "memory.h"

namespace nj2k::detail {

class WorkArena {
public:
    struct Mark {
        void* block;
        std::uint8_t* cur;
    };

    WorkArena() noexcept = default;
    WorkArena(const WorkArena&) = delete;
    WorkArena& operator=(const WorkArena&) = delete;
    ~WorkArena() { release_all(); }

    void init(const Allocator* allocator) noexcept { allocator_ = allocator; }
    // 0: no budget
    void set_budget(std::uint64_t bytes) noexcept { budget_ = bytes; }

    // Uninitialised memory, or nullptr when the budget or the allocator refuses (failed()).
    void* allocate(std::size_t size, std::size_t alignment = 16) noexcept;

    template <class T>
    T* make_array(std::size_t n) noexcept
    {
        static_assert(std::is_trivially_destructible_v<T>, "arena objects are never destroyed");
        if (n == 0) {
            return nullptr;
        }
        if (n > SIZE_MAX / sizeof(T)) {
            budget_failed_ = true;
            return nullptr;
        }
        void* p = allocate(n * sizeof(T), alignof(T) < 16 ? 16 : alignof(T));
        if (!p) {
            return nullptr;
        }
        T* t = static_cast<T*>(p);
        for (std::size_t i = 0; i < n; ++i) {
            ::new (static_cast<void*>(t + i)) T();
        }
        return t;
    }
    // n elements, not initialised
    template <class T>
    T* raw_array(std::size_t n) noexcept
    {
        static_assert(std::is_trivially_destructible_v<T> && std::is_trivially_default_constructible_v<T>);
        if (n == 0) {
            return nullptr;
        }
        if (n > SIZE_MAX / sizeof(T)) {
            budget_failed_ = true;
            return nullptr;
        }
        return static_cast<T*>(allocate(n * sizeof(T), alignof(T) < 16 ? 16 : alignof(T)));
    }

    Mark mark() const noexcept { return Mark{current_, cur_}; }
    void rewind(const Mark& m) noexcept;
    // Back to the start for the next decode; keeps at most retain bytes, merged into one block.
    void reset(std::size_t retain) noexcept;
    void release_all() noexcept;

    std::uint64_t reserved() const noexcept { return reserved_; }
    std::uint64_t high_water() const noexcept { return high_water_; }
    bool budget_failed() const noexcept { return budget_failed_; }
    void clear_failure() noexcept { budget_failed_ = false; }

private:
    struct Block {
        Block* next;
        std::size_t size;  // the whole block, this header included
    };
    static constexpr std::size_t kFirstBlock = 16 * 1024;
    static constexpr std::size_t kLargestBlock = 1024 * 1024;

    bool enter(Block* b) noexcept;
    std::size_t used_now() const noexcept;

    const Allocator* allocator_ = nullptr;
    Block* first_ = nullptr;
    Block* last_ = nullptr;
    Block* current_ = nullptr;
    std::uint8_t* cur_ = nullptr;
    std::uint8_t* end_ = nullptr;
    std::size_t next_block_ = kFirstBlock;
    std::size_t blocks_ = 0;
    std::uint64_t reserved_ = 0;
    std::uint64_t budget_ = 0;
    std::uint64_t used_before_ = 0;  // bytes of the blocks before current_
    std::uint64_t high_water_ = 0;
    bool budget_failed_ = false;
};

// A growing array in a WorkArena. Growing copies into a new array and leaves the old one to the
// arena, which costs at most the final size again.
template <class T>
bool grow(WorkArena& arena, T*& data, std::uint32_t size, std::uint32_t& capacity, std::uint32_t need) noexcept
{
    static_assert(std::is_trivially_copyable_v<T>);
    if (need <= capacity) {
        return true;
    }
    std::uint32_t c = capacity ? capacity : 4;
    while (c < need) {
        c = c > UINT32_MAX / 2 ? need : c * 2;
    }
    T* bigger = arena.raw_array<T>(c);
    if (!bigger) {
        return false;
    }
    if (size) {
        std::memcpy(bigger, data, std::size_t(size) * sizeof(T));
    }
    data = bigger;
    capacity = c;
    return true;
}

}  // namespace nj2k::detail

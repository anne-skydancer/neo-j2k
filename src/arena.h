// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// A codec's arena: blocks from its Allocator, handed out by bumping a pointer and released all
// together when the codec is done with them. A budget (the decoder limits' max_memory) bounds what
// the arena reserves, so header state can never grow past the limit, whatever a codestream says.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

#include "memory.h"

namespace nj2k::detail {

class Arena {
public:
    enum class Failure { none, budget, memory };

    Arena() noexcept = default;
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    ~Arena() { release_all(); }

    void init(const Allocator* allocator) noexcept { allocator_ = allocator; }
    // 0: no budget
    void set_budget(std::uint64_t bytes) noexcept { budget_ = bytes; }

    // Uninitialised memory, or nullptr when the budget or the allocator refuses (failure()).
    void* allocate(std::size_t size, std::size_t alignment = 16) noexcept;

    // n value-initialised elements, or nullptr (n == 0 gives nullptr too). The arena never runs
    // destructors, so only types that need none live in it.
    template <class T>
    T* make_array(std::size_t n) noexcept
    {
        static_assert(std::is_trivially_destructible_v<T>, "arena objects are never destroyed");
        if (n == 0 || n > SIZE_MAX / sizeof(T)) {
            if (n != 0) {
                failure_ = Failure::budget;
            }
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

    template <class T>
    T* make() noexcept
    {
        return make_array<T>(1);
    }

    // A copy of n bytes (nullptr for n == 0, which is not a failure).
    std::uint8_t* copy_bytes(const std::uint8_t* src, std::size_t n) noexcept;

    void release_all() noexcept;
    // Empty again for the next codestream: keeps at most retain bytes, as one block of the total
    // when there were several, so that use that needs no more makes no heap call.
    void reset(std::size_t retain) noexcept;

    std::uint64_t reserved() const noexcept { return reserved_; }
    Failure failure() const noexcept { return failure_; }

private:
    struct Block {
        Block* next;
        std::size_t size;  // the whole block, this header included
    };
    static constexpr std::size_t kFirstBlock = 2048;
    static constexpr std::size_t kLargestBlock = 64 * 1024;

    const Allocator* allocator_ = nullptr;
    Block* blocks_ = nullptr;
    std::uint8_t* cur_ = nullptr;
    std::uint8_t* end_ = nullptr;
    std::size_t next_block_ = kFirstBlock;
    std::uint64_t reserved_ = 0;
    std::uint64_t budget_ = 0;
    Failure failure_ = Failure::none;
};

// A growing list of trivial elements in an arena. Growing copies into a new array and leaves the
// old one to the arena, which costs at most the list's final size again.
template <class T>
struct ArenaList {
    static_assert(std::is_trivially_copyable_v<T>);
    T* data = nullptr;
    std::uint32_t size = 0;
    std::uint32_t capacity = 0;

    bool push(Arena& arena, const T& value) noexcept
    {
        if (size == capacity) {
            const std::uint32_t grown = capacity ? (capacity > (UINT32_MAX / 2) ? UINT32_MAX : capacity * 2) : 4;
            if (grown == capacity) {
                return false;
            }
            T* bigger = arena.make_array<T>(grown);
            if (!bigger) {
                return false;
            }
            if (size) {
                std::memcpy(bigger, data, std::size_t(size) * sizeof(T));
            }
            data = bigger;
            capacity = grown;
        }
        data[size++] = value;
        return true;
    }
    T* begin() const noexcept { return data; }
    T* end() const noexcept { return data + size; }
};

}  // namespace nj2k::detail

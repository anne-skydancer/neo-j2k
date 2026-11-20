// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
#include "arena.h"

namespace nj2k::detail {

namespace {

std::uintptr_t align_up(std::uintptr_t v, std::size_t alignment) noexcept
{
    return (v + (alignment - 1)) & ~std::uintptr_t(alignment - 1);
}

}  // namespace

void* Arena::allocate(std::size_t size, std::size_t alignment) noexcept
{
    if (size == 0) {
        return nullptr;
    }
    if (alignment == 0 || (alignment & (alignment - 1)) != 0 || alignment > kMaxAlignment) {
        failure_ = Failure::memory;
        return nullptr;
    }
    if (cur_) {
        const std::uintptr_t at = align_up(reinterpret_cast<std::uintptr_t>(cur_), alignment);
        if (at <= reinterpret_cast<std::uintptr_t>(end_) && size <= reinterpret_cast<std::uintptr_t>(end_) - at) {
            cur_ = reinterpret_cast<std::uint8_t*>(at + size);
            return reinterpret_cast<void*>(at);
        }
    }
    // a new block: the next size in the doubling series, or one of its own for a large request
    const std::size_t header = align_up(sizeof(Block), alignment);
    if (size > SIZE_MAX - header) {
        failure_ = Failure::budget;
        return nullptr;
    }
    std::size_t block = next_block_;
    if (size + header > block / 2) {
        block = size + header;
    } else if (next_block_ < kLargestBlock) {
        next_block_ *= 2;
    }
    if (budget_ != 0 && (block > budget_ || reserved_ > budget_ - block)) {
        failure_ = Failure::budget;
        return nullptr;
    }
    void* raw = allocator_ ? allocator_->allocate(block, alignment < 16 ? 16 : alignment) : nullptr;
    if (!raw) {
        failure_ = Failure::memory;
        return nullptr;
    }
    auto* b = static_cast<Block*>(raw);
    b->next = blocks_;
    b->size = block;
    blocks_ = b;
    reserved_ += block;
    auto* base = static_cast<std::uint8_t*>(raw);
    std::uint8_t* at = base + header;
    if (size + header > block / 2 && block == size + header) {
        // a block of its own: keep bumping in the current one, which may still have room
        return at;
    }
    cur_ = at + size;
    end_ = base + block;
    return at;
}

std::uint8_t* Arena::copy_bytes(const std::uint8_t* src, std::size_t n) noexcept
{
    if (n == 0) {
        return nullptr;
    }
    auto* p = static_cast<std::uint8_t*>(allocate(n, 16));
    if (p) {
        std::memcpy(p, src, n);
    }
    return p;
}

void Arena::reset(std::size_t retain) noexcept
{
    if (!blocks_) {
        failure_ = Failure::none;
        return;
    }
    const std::uint64_t total = reserved_;
    if (blocks_->next || total > retain) {
        release_all();
        if (total > retain) {
            return;
        }
        // one block of the total size, so the next use of no more fits it
        void* raw = allocator_ ? allocator_->allocate(std::size_t(total), 16) : nullptr;
        if (!raw) {
            return;
        }
        auto* b = static_cast<Block*>(raw);
        b->next = nullptr;
        b->size = std::size_t(total);
        blocks_ = b;
        reserved_ = total;
    }
    const std::size_t header = align_up(sizeof(Block), 16);
    cur_ = reinterpret_cast<std::uint8_t*>(blocks_) + header;
    end_ = reinterpret_cast<std::uint8_t*>(blocks_) + blocks_->size;
    next_block_ = kFirstBlock;
    failure_ = Failure::none;
}

void Arena::release_all() noexcept
{
    Block* b = blocks_;
    while (b) {
        Block* next = b->next;
        allocator_->release(b, b->size);
        b = next;
    }
    blocks_ = nullptr;
    cur_ = end_ = nullptr;
    next_block_ = kFirstBlock;
    reserved_ = 0;
    failure_ = Failure::none;
}

}  // namespace nj2k::detail

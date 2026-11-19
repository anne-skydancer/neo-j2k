// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
#include "work_arena.h"

namespace nj2k::detail {

namespace {

std::uintptr_t align_up(std::uintptr_t v, std::size_t a) noexcept
{
    return (v + (a - 1)) & ~std::uintptr_t(a - 1);
}

constexpr std::size_t kHeader = 64;  // the block header, rounded to a cache line

}  // namespace

bool WorkArena::enter(Block* b) noexcept
{
    current_ = b;
    cur_ = reinterpret_cast<std::uint8_t*>(b) + kHeader;
    end_ = reinterpret_cast<std::uint8_t*>(b) + b->size;
    return true;
}

std::size_t WorkArena::used_now() const noexcept
{
    return current_ ? std::size_t(used_before_) + std::size_t(cur_ - reinterpret_cast<std::uint8_t*>(current_)) : 0;
}

void* WorkArena::allocate(std::size_t size, std::size_t alignment) noexcept
{
    if (alignment == 0 || (alignment & (alignment - 1)) != 0 || alignment > 64) {
        budget_failed_ = true;
        return nullptr;
    }
    if (size > SIZE_MAX / 2) {
        budget_failed_ = true;
        return nullptr;
    }
    for (;;) {
        if (current_) {
            const std::uintptr_t at = align_up(reinterpret_cast<std::uintptr_t>(cur_), alignment);
            if (at <= reinterpret_cast<std::uintptr_t>(end_) && size <= reinterpret_cast<std::uintptr_t>(end_) - at) {
                cur_ = reinterpret_cast<std::uint8_t*>(at + size);
                const std::uint64_t used = used_now();
                if (used > high_water_) {
                    high_water_ = used;
                }
                return reinterpret_cast<void*>(at);
            }
            if (current_->next) {
                // the next kept block; one too small for this request is passed over for this decode
                used_before_ += current_->size;
                enter(current_->next);
                continue;
            }
        } else if (first_) {
            used_before_ = 0;
            enter(first_);
            continue;
        }
        break;
    }
    // a new block at the end: the next size in the doubling series, or the request's own size
    std::size_t block = next_block_;
    if (size + kHeader + alignment > block) {
        block = size + kHeader + alignment;
    } else if (next_block_ < kLargestBlock) {
        next_block_ *= 2;
    }
    if (budget_ != 0 && (block > budget_ || reserved_ > budget_ - block)) {
        budget_failed_ = true;
        return nullptr;
    }
    void* raw = allocator_ ? allocator_->allocate(block, 64) : nullptr;
    if (!raw) {
        return nullptr;
    }
    auto* b = static_cast<Block*>(raw);
    b->next = nullptr;
    b->size = block;
    if (last_) {
        last_->next = b;
    } else {
        first_ = b;
    }
    last_ = b;
    ++blocks_;
    reserved_ += block;
    if (current_) {
        used_before_ += current_->size;
    } else {
        used_before_ = 0;
    }
    enter(b);
    return allocate(size, alignment);
}

void WorkArena::rewind(const Mark& m) noexcept
{
    if (!m.block) {
        current_ = nullptr;
        cur_ = end_ = nullptr;
        used_before_ = 0;
        return;
    }
    // the bytes of the blocks before the marked one
    std::uint64_t before = 0;
    for (Block* b = first_; b && b != m.block; b = b->next) {
        before += b->size;
    }
    used_before_ = before;
    enter(static_cast<Block*>(m.block));
    cur_ = m.cur;
}

void WorkArena::reset(std::size_t retain) noexcept
{
    current_ = nullptr;
    cur_ = end_ = nullptr;
    used_before_ = 0;
    budget_failed_ = false;
    if (reserved_ > retain) {
        release_all();
        return;
    }
    if (blocks_ > 1) {
        // one block of the total, so that a decode needing no more fits it without a heap call
        const std::size_t total = std::size_t(reserved_);
        release_all();
        void* raw = allocator_ ? allocator_->allocate(total, 64) : nullptr;
        if (raw) {
            auto* b = static_cast<Block*>(raw);
            b->next = nullptr;
            b->size = total;
            first_ = last_ = b;
            blocks_ = 1;
            reserved_ = total;
        }
    }
    high_water_ = 0;
}

void WorkArena::release_all() noexcept
{
    Block* b = first_;
    while (b) {
        Block* next = b->next;
        allocator_->release(b, b->size);
        b = next;
    }
    first_ = last_ = current_ = nullptr;
    cur_ = end_ = nullptr;
    next_block_ = kFirstBlock;
    blocks_ = 0;
    reserved_ = 0;
    used_before_ = 0;
    high_water_ = 0;
    budget_failed_ = false;
}

}  // namespace nj2k::detail

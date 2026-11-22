// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The frozen reference Tier-1 decoder's interface (t1_frozen.cpp): the decoder of src/t1.cpp as it was before the
// MQ-NG redesign, in nj2k::frozen, with its own coder and scratch structs (src/t1.h's are redesigned).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../../src/t1.h"

namespace nj2k::frozen {

struct Mqc {
    std::uint32_t c, a, ct;
    std::uint32_t end_of_byte_stream_counter;
    std::uint8_t* bp;
    std::uint8_t* start;
    std::uint8_t* end;
    const nj2k::detail::MqState* ctxs[19];
    const nj2k::detail::MqState** curctx;
    const std::uint8_t* lut_ctxno_zc_orient;
    std::uint8_t backup[2];
};

struct T1 {
    Mqc mqc;
    std::int32_t* data = nullptr;
    std::uint32_t* flags = nullptr;
    std::uint32_t w = 0, h = 0;
    std::uint8_t* cblkdata = nullptr;
    std::size_t cblkdata_size = 0;
};

// A T1 that owns its buffers (the library's are the allocator's): data and flags of the largest code-block, bytes on
// demand with the two bytes of padding.
class Scratch {
public:
    Scratch() : data_(4096), flags_(3078)
    {
        t.data = data_.data();
        t.flags = flags_.data();
    }
    // Room for n bytes of code-block data and 2 of padding; the bytes of an earlier code-block are not kept.
    void reserve(std::size_t n)
    {
        bytes_.assign(n + 2, 0);
        t.cblkdata = bytes_.data();
        t.cblkdata_size = bytes_.size();
    }
    T1 t{};

private:
    std::vector<std::int32_t> data_;
    std::vector<std::uint32_t> flags_;
    std::vector<std::uint8_t> bytes_;
};

// opj_t1_decode_cblk as it was: false for the refused code-block (bit-planes plus ROI shift at or above 31). The chunks'
// total must fit what Scratch::reserve was given (+ 2 padding bytes); the segments' lengths must fit the chunks.
bool t1_decode_cblk(T1& t1, const nj2k::detail::T1Block& b, const nj2k::detail::Messages& msg) noexcept;

// One decision of the MQ decoder from the state of m (a, c, ct, bp and the context ctxno's state), as
// opj_mqc_decode_macro: the decoded bit.
std::uint32_t mq_decode_one(Mqc& m, std::uint32_t ctxno) noexcept;
// The state of the coder's context ctxno: its index (state * 2 + MPS) in kMqcStates; setstate sets it.
std::uint32_t mq_state_index(const Mqc& m, std::uint32_t ctxno) noexcept;
void mq_setstate(Mqc& m, std::uint32_t ctxno, std::uint32_t index) noexcept;

}  // namespace nj2k::frozen

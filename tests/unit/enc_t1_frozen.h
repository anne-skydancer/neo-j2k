// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The frozen reference Tier-1 encoder's interface (enc_t1_frozen.cpp), the same functions as
// src/enc_tile.h's, in nj2k::frozen.
#pragma once

#include <cstdint>

#include "../../src/enc_tile.h"
#include "../../src/memory.h"

namespace nj2k::frozen {

struct EncT1;
EncT1* enc_t1_create(const nj2k::detail::Allocator& alloc) noexcept;
void enc_t1_destroy(EncT1* t1, const nj2k::detail::Allocator& alloc) noexcept;
std::int32_t* enc_t1_data(EncT1* t1) noexcept;
bool enc_t1_encode(EncT1* t1, nj2k::detail::EncCblk& cblk, std::uint32_t orient, std::uint32_t compno,
                   std::uint32_t level, bool reversible, double stepsize, const double* mct_norms,
                   std::uint32_t mct_numcomps) noexcept;
bool enc_t1_commit(const EncT1* t1, nj2k::detail::Arena& arena, nj2k::detail::EncCblk& cblk) noexcept;

}  // namespace nj2k::frozen

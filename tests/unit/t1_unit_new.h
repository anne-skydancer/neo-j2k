// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The redesigned decoder's side of the single-decision tests of t1_unit.cpp (stage 0: the decoder is today's, so the
// hooks have nothing of the new one to check).
#pragma once

#include <cstdint>

template <class C2>
bool t1_unit_check_new_tables(const C2*)
{
    return true;
}

// The redesigned decision from the state (index state, interval a, code register c, bit counter ct, bytes by[0..3] then
// 0xFF) gives the bit d, the registers a, c, ct, the byte position bp and the next state index.
inline bool t1_unit_one_decision_new(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, const std::uint8_t*,
                                     std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                                     std::uint32_t)
{
    return true;
}

// The redesigned run of AGG decisions over cols empty columns from (a, c) with the AGG state (MPS 0) takes `run` of them.
inline bool t1_unit_agg_run_new(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t)
{
    return true;
}

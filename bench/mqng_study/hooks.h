// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The MQ-NG decoder study's hooks (bench/mqng_study, not built with the library): the stage clock and the
// code-block dump that the instrumented copies of decode.cpp and tile.cpp call. The copies are made at
// configure time by bench/mqng_study/CMakeLists.txt; src/ is never changed.
#pragma once

#include <cstdint>
#include <vector>

#include <intrin.h>

#include "blockio.h"

namespace nj2k::detail {
struct T1Block;
}

namespace mqs {

inline std::uint64_t tsc() noexcept { return __rdtsc(); }

enum Stage { kT2, kT1, kDwt, kStages };

struct Clock {
    std::uint64_t at = 0;
    std::uint64_t cycles[kStages] = {};
};
extern Clock g_clock;

inline void mark_start() noexcept { g_clock.at = tsc(); }
inline void mark(Stage s) noexcept
{
    const std::uint64_t now = tsc();
    g_clock.cycles[s] += now - g_clock.at;
    g_clock.at = now;
}

// one code-block as Tier-1 receives it: its chunks gathered (Tier-1 gathers them itself)
extern bool g_dump;
extern std::vector<DumpBlock> g_blocks;

void dump(const nj2k::detail::T1Block& b) noexcept;

}  // namespace mqs

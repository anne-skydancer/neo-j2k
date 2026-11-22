// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The encoder Tier-1 study's hooks (bench/t1_study, not built with the library): the stage clock and the
// code-block dump that the instrumented copy of encode.cpp calls. The copy is made at configure time by
// bench/t1_study/CMakeLists.txt; src/ is never changed.
#pragma once

#include <cstdint>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

namespace study {

inline std::uint64_t tsc() noexcept { return __rdtsc(); }

enum Stage { kBuild, kDcMct, kDwt, kT1Copy, kT1Kernel, kT1Commit, kT2, kStages };

struct Clock {
    std::uint64_t at = 0;
    std::uint64_t cycles[kStages] = {};
    std::uint64_t t1_begin = 0, t1_kernel = 0, t1_copy = 0, job_begin = 0;
};
extern Clock g_clock;

inline void mark_start() noexcept { g_clock.at = tsc(); }
inline void mark(Stage s) noexcept
{
    const std::uint64_t now = tsc();
    g_clock.cycles[s] += now - g_clock.at;
    g_clock.at = now;
}

struct Block {
    std::uint32_t w, h, orient, compno, level, reversible, mct_numcomps, mct_table;  // 0 none, 1 rev, 2 real
    double stepsize;
    std::vector<std::int32_t> data;
};
extern bool g_dump;
extern std::vector<Block> g_blocks;

void t1_job_begin() noexcept;
void t1_before(const std::int32_t* data, std::uint32_t w, std::uint32_t h, std::uint32_t orient, std::uint32_t compno,
               std::uint32_t level, bool reversible, float stepsize, const double* mct_norms,
               std::uint32_t mct_numcomps) noexcept;
void t1_after() noexcept;
// the Tier-1 stage's wall clock is split by the job hooks into copy-in, kernel and commit
void t1_stage_end() noexcept;

}  // namespace study

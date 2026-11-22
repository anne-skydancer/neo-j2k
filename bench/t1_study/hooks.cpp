// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The study hooks (hooks.h). One thread only: the profile runs the encoder without an executor.
#include "hooks.h"

#include <neoj2k/neoj2k.h>

namespace nj2k::detail {
extern const double kEncMctNorms[3];
extern const double kEncMctNormsReal[3];
}  // namespace nj2k::detail

namespace study {

Clock g_clock;
bool g_dump = false;
std::vector<Block> g_blocks;

void t1_job_begin() noexcept { g_clock.job_begin = tsc(); }

void t1_before(const std::int32_t* data, std::uint32_t w, std::uint32_t h, std::uint32_t orient, std::uint32_t compno,
               std::uint32_t level, bool reversible, float stepsize, const double* mct_norms,
               std::uint32_t mct_numcomps) noexcept
{
    if (g_dump) {
        Block b;
        b.w = w;
        b.h = h;
        b.orient = orient;
        b.compno = compno;
        b.level = level;
        b.reversible = reversible ? 1 : 0;
        b.mct_numcomps = mct_numcomps;
        b.mct_table = !mct_norms ? 0 : mct_norms == nj2k::detail::kEncMctNorms ? 1 : 2;
        b.stepsize = static_cast<double>(stepsize);
        b.data.assign(data, data + std::size_t(w) * h);
        g_blocks.push_back(std::move(b));
    }
    const std::uint64_t now = tsc();
    g_clock.t1_copy += now - g_clock.job_begin;
    g_clock.t1_begin = now;
}

void t1_after() noexcept { g_clock.t1_kernel += tsc() - g_clock.t1_begin; }

void t1_stage_end() noexcept
{
    const std::uint64_t now = tsc();
    const std::uint64_t total = now - g_clock.at;
    g_clock.cycles[kT1Copy] += g_clock.t1_copy;
    g_clock.cycles[kT1Kernel] += g_clock.t1_kernel;
    g_clock.cycles[kT1Commit] += total - g_clock.t1_copy - g_clock.t1_kernel;
    g_clock.t1_copy = g_clock.t1_kernel = 0;
    g_clock.at = now;
}

}  // namespace study

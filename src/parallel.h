// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// Threads in one image: the host's executor (nj2k_decoder_set_executor or nj2k_encoder_set_executor),
// as the stages of a large tile use it. The library starts no thread and keeps no pool (plan T2): a stage
// cuts its work into parts whose outputs never overlap, the host runs them, and the stage goes on
// when every part has returned. A part's result depends only on its own inputs, never on which
// thread ran it, when, or what the other parts did, and nothing is summed across parts, so a decode
// gives the same samples, status and messages for any executor and any concurrency. Small tiles
// (below the codec's threshold) and a codec without an executor never come here; nor does a decode
// into caller's planes that share bytes (planes_apart, decode.cpp), whose writes only one thread
// orders as before.
#pragma once

#include <cstddef>
#include <cstdint>

#include <neoj2k/neoj2k.h>

#include "fpenv.h"

namespace nj2k::detail {

// The most parts a stage is cut into (the executor's concurrency is clamped to it).
inline constexpr std::uint32_t kMaxJobs = 64;
// The host decides busy or idle: it lends the executor only when it has idle cores, so the library has
// no image-level guard. With an executor, a tile is decoded through it from this many samples in its
// largest component on (512 x 512): below it a stage's parts would take less than the tens of
// microseconds an executor needs to start and join them (performance.md section 6), as in an image of
// many small tiles.
inline constexpr std::uint64_t kParallelTileFloor = std::uint64_t(1) << 18;
// Encoding has substantially more Tier-1 work per sample than decoding. Its measured crossover is
// one 256 x 256 component: below this, executor dispatch remains a loss.
inline constexpr std::uint64_t kParallelEncodeFloor = std::uint64_t(1) << 16;
// Inside such a tile, a wavelet level is split from this area (samples of the level) on; smaller
// levels run on the calling thread, as before. An estimate (not probed on its own): it applies only
// inside a tile already split.
inline constexpr std::uint64_t kParallelLevelArea = std::uint64_t(1) << 18;
// And an output region (the colour transform, the DC shift, the copy) from this many samples a
// component. An estimate as well, and inside a tile already split.
inline constexpr std::uint64_t kParallelOutputSamples = std::uint64_t(1) << 16;
// An image plane cleared because the first tile written into it does not cover it (a multi-tile
// image) is cleared in bands from this many bytes on, whatever its tiles' sizes: below it the bands
// cost more than they save (doc/bench.md, stage 5: a fresh 1 MiB plane cleared in 8 bands took as long
// as on one thread, a 4 MiB one half as long).
inline constexpr std::uint64_t kParallelClearBytes = std::uint64_t(1) << 22;

// A stage's view of the executor: how many parts to cut it into, and the run itself.
struct Jobs {
    const nj2k_executor_t* ex = nullptr;
    std::uint32_t width = 1;                             // parts a stage is cut into, 2 to kMaxJobs
    std::uint64_t level_area = kParallelLevelArea;       // wavelet levels split from this area on
    std::uint64_t output_samples = kParallelOutputSamples;  // output regions split from this size on
    std::uint64_t clear_bytes = kParallelClearBytes;        // plane clears split from this size on

    // f(i) for every i < count, through the executor; when it returns, every part's writes are seen
    // by the caller (the executor's contract, neoj2k.h)
    template <class F>
    void run(std::uint32_t count, F& f) const noexcept
    {
        if (count == 1) {
            f(0u);
        } else if (count > 1) {
            ex->parallel_for(ex->user, count, &call<F>, &f);
        }
    }

private:
    template <class F>
    static void call(void* ctx, NJ2K_UINT32 i)
    {
        // a host's worker may keep another floating-point environment: the job runs under the default
        const DefaultFpEnv fp;
        (*static_cast<F*>(ctx))(i);
    }
};

// Part i of n of [0, total): [part_begin(total, n, i), part_begin(total, n, i + 1)).
inline std::uint32_t part_begin(std::uint32_t total, std::uint32_t n, std::uint32_t i) noexcept
{
    return static_cast<std::uint32_t>(std::uint64_t(total) * i / n);
}

// For the tests: the sizes a codec splits from (tile samples of the largest component, wavelet level area,
// output samples a component, plane clear bytes), so that the corpus's small files take every threaded path; false for a null codec. Not exported from the shared library (only nj2k_* names are).
bool set_parallel_thresholds(nj2k_codec_t* codec, std::uint64_t tile_samples, std::uint64_t level_area,
                             std::uint64_t output_samples, std::uint64_t clear_bytes) noexcept;

}  // namespace nj2k::detail

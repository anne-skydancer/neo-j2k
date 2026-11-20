// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// Threads in one image (stage 5): the host's executor (nj2k_decoder_set_executor), as the stages of
// a large tile's decode use it. The library starts no thread and keeps no pool (plan T2): a stage
// cuts its work into parts whose outputs never overlap, the host runs them, and the stage goes on
// when every part has returned. A part's result depends only on its own inputs, never on which
// thread ran it, when, or what the other parts did, and nothing is summed across parts, so a decode
// gives the same samples, status and messages for any executor and any concurrency. Small tiles
// (below the codec's threshold) and a codec without an executor never come here.
#pragma once

#include <cstddef>
#include <cstdint>

#include <neoj2k/neoj2k.h>

namespace nj2k::detail {

// The most parts a stage is cut into (the executor's concurrency is clamped to it).
inline constexpr std::uint32_t kMaxJobs = 64;
// A tile is decoded through the executor from this many decoded samples over its components on (a
// 512 x 512 grey or a 296 x 296 RGB tile): below it a stage's parts would take less than the tens of
// microseconds an executor needs to start and join them (performance.md section 6).
inline constexpr std::uint64_t kParallelTileSamples = std::uint64_t(1) << 18;
// Inside such a tile, a wavelet level is split from this area (samples of the level) on; smaller
// levels run on the calling thread, as before.
inline constexpr std::uint64_t kParallelLevelArea = std::uint64_t(1) << 18;
// And an output region (the colour transform, the DC shift, the copy) from this many samples a
// component.
inline constexpr std::uint64_t kParallelOutputSamples = std::uint64_t(1) << 16;

// A stage's view of the executor: how many parts to cut it into, and the run itself.
struct Jobs {
    const nj2k_executor_t* ex = nullptr;
    std::uint32_t width = 1;                             // parts a stage is cut into, 2 to kMaxJobs
    std::uint64_t level_area = kParallelLevelArea;       // wavelet levels split from this area on
    std::uint64_t output_samples = kParallelOutputSamples;  // output regions split from this size on

    // f(i) for every i < count, through the executor (one part runs on the calling thread)
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
        (*static_cast<F*>(ctx))(i);
    }
};

// Part i of n of [0, total): [part_begin(total, n, i), part_begin(total, n, i + 1)).
inline std::uint32_t part_begin(std::uint32_t total, std::uint32_t n, std::uint32_t i) noexcept
{
    return static_cast<std::uint32_t>(std::uint64_t(total) * i / n);
}

// For the tests: the sizes a codec splits from (tile samples, wavelet level area, output samples a
// component), so that the corpus's small files take every threaded path; false for a null codec.
bool set_parallel_thresholds(nj2k_codec_t* codec, std::uint64_t tile_samples, std::uint64_t level_area,
                             std::uint64_t output_samples) noexcept;

}  // namespace nj2k::detail

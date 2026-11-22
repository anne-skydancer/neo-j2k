// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The study copies' counters (enc_t1_count_body.inc); included inside namespace nj2k::STUDY_NS.
struct Stats {
    std::uint64_t blocks, samples, bitplanes;
    std::uint64_t passes[3], decisions[3], ctx_hist[19];
    std::uint64_t mps, mps_renorm, lps, renorms, renorm_shifts, byteouts;
    std::uint64_t pass_cycles[3];
    std::uint32_t passtype;
};
inline Stats g_stats;

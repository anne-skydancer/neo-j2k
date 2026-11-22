// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The tallies of the counting variant (variant.cpp, STUDY_V 8), the recording, and the pass clock.
#include "common.h"

namespace mqs {

Counts g_counts;
Recording g_rec;
const std::uint8_t* g_bits = nullptr;
std::uint64_t g_pass_cycles[4] = {}, g_pass_count[4] = {};

void count_segment(const std::uint8_t* bp, std::uint32_t len, const std::uint8_t* st) noexcept
{
    ++g_counts.segments;
    g_counts.run_ctx = 99;
    g_counts.run_len = 0;
    if (!g_rec.on) {
        return;
    }
    RecSeg s{};
    s.bytes = g_rec.bytes.size();
    s.len = len;
    s.first = g_rec.ctx.size();
    std::memcpy(s.states, st, 19);
    s.usable = true;
    g_rec.bytes.insert(g_rec.bytes.end(), bp, bp + len + 2);  // with the sentinel the decoder wrote
    g_rec.bytes.insert(g_rec.bytes.end(), 6, 0);
    g_rec.segs.push_back(s);
}

static void end_run(Counts& k) noexcept
{
    if (k.run_len) {
        ++k.run_hist[bucket(k.run_len)];
        if (k.run_len >= 2) {
            k.run_hist[8] += k.run_len;
        }
    }
    k.run_len = 0;
}

void count_decision(std::uint32_t cx, std::uint32_t i, std::uint32_t a0, std::uint32_t c0, std::uint32_t d) noexcept
{
    Counts& k = g_counts;
    const std::uint32_t qe = kPacked.s[i].qe;
    const std::uint32_t an = a0 - qe;
    const bool lpsi = (c0 >> 16) < qe;
    const bool small = an < qe;
    const bool norenorm = !lpsi && (an & 0x8000) != 0;
    ++k.decisions;
    ++k.ctx[cx];
    ++k.pass_dec[k.pass_now];
    k.lps_interval += lpsi;
    k.mps_interval += !lpsi;
    k.mps_norenorm += norenorm;
    k.ctx_mps_norenorm[cx] += norenorm;
    k.d_one += d;
    k.d_lps_symbol += d != (i & 1U);
    site(0, lpsi);
    site(1, !norenorm);
    site(2, lpsi ^ small);
    site(3, d);
    // runs of MPS-without-renormalisation in one context
    if (norenorm && cx == k.run_ctx) {
        ++k.run_len;
    } else {
        end_run(k);
        if (norenorm) {
            k.run_ctx = cx;
            k.run_len = 1;
        } else {
            k.run_ctx = 99;
        }
    }
    // AGG runs (context 17)
    if (cx == T1_CTXNO_AGG && norenorm) {
        ++k.agg_run;
    } else if (k.agg_run) {
        ++k.agg_run_hist[bucket(k.agg_run)];
        if (k.agg_run >= 2) {
            k.agg_run_hist[8] += k.agg_run;
        }
        k.agg_run = 0;
    }
    if (g_rec.on) {
        g_rec.ctx.push_back(static_cast<std::uint8_t>(cx));
        g_rec.sym.push_back(static_cast<std::uint8_t>(d));
        ++g_rec.segs.back().count;
    }
}

}  // namespace mqs

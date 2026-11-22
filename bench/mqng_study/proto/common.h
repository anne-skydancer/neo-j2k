// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// What the MQ-NG study's decoder variants share: the Tier-1 scratch they run on, the packed state tables
// (built at compile time from the library's kMqcStates, so the numbers cannot drift), the counters of the
// counting variant and the recording the MQ-alone and modelling-alone replays read.
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <immintrin.h>
#include <intrin.h>

#include "messages.h"
#include "t1.h"
#include "t1_flags.h"
#include "t1_tables.h"

namespace mqs {

// the scratch every variant decodes into (the library's T1 layout of the fields the body reads)
template <class M>
struct T1V {
    M mqc;
    std::int32_t* data = nullptr;
    std::uint32_t* flags = nullptr;
    std::uint32_t w = 0, h = 0;
    std::uint8_t* cblkdata = nullptr;
    std::size_t cblkdata_size = 0;
};

// the MQ decoder of the packed variants: each context slot a Slot (a state index byte, or a state word)
template <class Slot>
struct MqcV {
    std::uint32_t c, a, ct;
    std::uint32_t end_of_byte_stream_counter;
    std::uint8_t* bp;
    std::uint8_t* start;
    std::uint8_t* end;
    Slot st[19];
    Slot* curctx;
    const std::uint8_t* lut_ctxno_zc_orient;
    std::uint8_t backup[2];
};

struct Scratch {
    std::int32_t* data;
    std::uint32_t* flags;
    std::uint8_t* cblkdata;
    std::size_t cblkdata_size;
};

// one state a byte: its index into kMqcStates (state * 2 + MPS); 4 bytes a row
struct PackedState {
    std::uint16_t qe;
    std::uint8_t nmps, nlps;
};
struct PackedTable {
    PackedState s[94];
};
constexpr PackedTable make_packed()
{
    PackedTable t{};
    for (int i = 0; i < 94; ++i) {
        t.s[i].qe = static_cast<std::uint16_t>(nj2k::detail::kMqcStates[i].qeval);
        t.s[i].nmps = static_cast<std::uint8_t>(nj2k::detail::kMqcStates[i].nmps - nj2k::detail::kMqcStates);
        t.s[i].nlps = static_cast<std::uint8_t>(nj2k::detail::kMqcStates[i].nlps - nj2k::detail::kMqcStates);
    }
    return t;
}
inline constexpr PackedTable kPacked = make_packed();
static_assert(kPacked.s[0].qe == 0x5601 && kPacked.s[0].nmps == 2 && kPacked.s[0].nlps == 3, "packed table");

// the state held in the context slot itself: qe (16 bits), MPS (bit 16), the successors' indices (17-23 the
// MPS's, 24-30 the LPS's)
struct WordTable {
    std::uint32_t w[94];
};
constexpr WordTable make_words()
{
    WordTable t{};
    for (int i = 0; i < 94; ++i) {
        t.w[i] = std::uint32_t(kPacked.s[i].qe) | (std::uint32_t(i & 1) << 16) | (std::uint32_t(kPacked.s[i].nmps) << 17) |
                 (std::uint32_t(kPacked.s[i].nlps) << 24);
    }
    return t;
}
inline constexpr WordTable kWords = make_words();
static_assert((nj2k::detail::kMqcStates[5].mps) == 1 && (nj2k::detail::kMqcStates[4].mps) == 0, "MPS is the index's low bit");

// the counting variant's tallies (one thread)
struct Counts {
    std::uint64_t decisions = 0, lps_interval = 0, mps_interval = 0, mps_norenorm = 0, renorms = 0, shifts = 0;
    std::uint64_t byteins = 0, markers = 0, d_one = 0, d_lps_symbol = 0;
    std::uint64_t ctx[19] = {}, ctx_mps_norenorm[19] = {};
    std::uint64_t pass_dec[3] = {}, raw_decisions = 0;
    std::uint64_t segments = 0, raw_segments = 0, passes[3] = {}, raw_passes = 0;
    // the MQ branch sites, outcome and changes of outcome (a last-outcome predictor's misses)
    std::uint64_t site_taken[4] = {}, site_flips[4] = {};
    std::uint32_t site_last[4] = {};
    // runs of MPS-without-renormalisation decisions in one context, back to back
    std::uint64_t run_hist[9] = {};  // 1, 2, 3-4, 5-8, 9-16, 17-32, 33-64, 65+, and decisions in runs >= 2
    std::uint32_t run_ctx = 99, run_len = 0;
    // AGG decisions that are MPS without renormalisation, back to back (the run-mode fast path's target)
    std::uint64_t agg_run_hist[9] = {};
    std::uint32_t agg_run = 0;
    std::uint32_t pass_now = 0;
    // the stall predictor of the "skip": AGG MPS-no-renorm decisions that a run decoder could take at once
};
extern Counts g_counts;

inline void site(std::uint32_t k, std::uint32_t taken) noexcept
{
    g_counts.site_taken[k] += taken;
    g_counts.site_flips[k] += taken != g_counts.site_last[k];
    g_counts.site_last[k] = taken;
}

inline unsigned bucket(std::uint32_t n) noexcept
{
    return n <= 1 ? 0 : n == 2 ? 1 : n <= 4 ? 2 : n <= 8 ? 3 : n <= 16 ? 4 : n <= 32 ? 5 : n <= 64 ? 6 : 7;
}

// the recording: every MQ segment (its bytes with the sentinel, its contexts' starting states) and every
// decision (context, symbol), for the MQ-alone and the modelling-alone replays
struct RecSeg {
    std::size_t bytes;  // offset in Recording::bytes
    std::uint32_t len;
    std::size_t first, count;
    std::uint8_t states[19];
    bool usable;
};
struct Recording {
    bool on = false;
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> ctx, sym;
    std::vector<RecSeg> segs;
};
extern Recording g_rec;

// the modelling-alone variant's bit source
extern const std::uint8_t* g_bits;

}  // namespace mqs

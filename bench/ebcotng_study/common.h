// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// EBCOT-NG study (doc/ebcotng_study.md): what the encoder and decoder prototypes share. The MQ tables are
// built at compile time from the library's kMqcStates (the numbers cannot drift): the encoder's packed 4-byte
// table of stage 2 (doc/enc_t1_study.md) and the decoder's state words of MQ-NG stage 1 (doc/mqng_study.md).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <immintrin.h>
#include <intrin.h>

#include "t1.h"
#include "t1_flags.h"
#include "t1_tables.h"

namespace ebc {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using i32 = std::int32_t;
using u64 = std::uint64_t;

// src/t1.cpp's mq_shifts, for a in [1, 0xffff] (0 when bit 15 is set)
__forceinline u32 mq_shifts(u32 a) noexcept
{
#if defined(__AVX2__)
    return _lzcnt_u32(a) - 16U;
#else
    unsigned long msb;
    _BitScanReverse(&msb, a);
    return 15U - static_cast<u32>(msb);
#endif
}

__forceinline u32 ctz32(u32 x) noexcept
{
#if defined(__AVX2__)
    return _tzcnt_u32(x);
#else
    unsigned long i;
    _BitScanForward(&i, x);
    return static_cast<u32>(i);
#endif
}

// the encoder's stage-2 table: a state as a byte index (state * 2 + MPS) into 94 rows of qe, nmps, nlps
struct Packed {
    std::uint16_t qe;
    u8 nmps, nlps;
};
struct PackedTable {
    Packed t[94];
};
constexpr PackedTable make_packed()
{
    PackedTable t{};
    for (int i = 0; i < 94; ++i) {
        t.t[i].qe = static_cast<std::uint16_t>(nj2k::detail::kMqcStates[i].qeval);
        t.t[i].nmps = static_cast<u8>(nj2k::detail::kMqcStates[i].nmps - nj2k::detail::kMqcStates);
        t.t[i].nlps = static_cast<u8>(nj2k::detail::kMqcStates[i].nlps - nj2k::detail::kMqcStates);
    }
    return t;
}
inline constexpr PackedTable kPacked = make_packed();

// the decoder's state words: qe (bits 0-15), MPS (16), next-MPS index (17-23), next-LPS index (24-30)
struct WordTable {
    u32 w[94];
};
constexpr WordTable make_words()
{
    WordTable t{};
    for (int i = 0; i < 94; ++i) {
        t.w[i] = u32(kPacked.t[i].qe) | (u32(i & 1) << 16) | (u32(kPacked.t[i].nmps) << 17) |
                 (u32(kPacked.t[i].nlps) << 24);
    }
    return t;
}
inline constexpr WordTable kWords = make_words();
static_assert(kPacked.t[0].qe == 0x5601 && kPacked.t[0].nmps == 2 && kPacked.t[0].nlps == 3, "packed table");

// The counting variants' tallies (one thread). Sites are the data-dependent branches of the passes; each
// keeps its outcomes and a last-outcome predictor's misses (an upper bound of a real predictor's).
enum Site {
    kSppCol,    // significance pass: column word non-zero
    kSppElig,   // significance pass: sample eligible (insignificant, unvisited, a significant neighbour)
    kSppBit,    // significance pass: the significance bit
    kRefCol,    // refinement pass: column has refinement work
    kRefElig,   // refinement pass: sample refined
    kClnAgg,    // cleanup: column in run mode (word 0)
    kClnAggBit, // cleanup: the AGG decision (1 = run broken)
    kClnElig,   // cleanup: sample coded (outside run mode)
    kClnBit,    // cleanup: the significance bit (outside run mode's first sample)
    kSites
};
struct Counts {
    u64 blocks, samples, bitplanes, passes[3];
    u64 cols[3], cols_work[3];            // columns visited, with work
    u64 tested[3], dec[3], sc[3];         // samples tested, decisions, sign decisions (of them)
    u64 agg, uni, sig_events, upd_rmw;    // AGG and UNI decisions, newly significant samples, flag RMWs
    u64 lut_loads, nmse_loads, data_loads;
    u64 site_n[kSites], site_taken[kSites], site_miss[kSites];
    u32 site_last[kSites];
    u64 dec_ctx[19];
};
inline Counts g_counts;
__forceinline void site(int k, u32 taken) noexcept
{
    g_counts.site_n[k] += 1;
    g_counts.site_taken[k] += taken;
    g_counts.site_miss[k] += taken != g_counts.site_last[k];
    g_counts.site_last[k] = taken;
}

// per-pass clock of the timed copies (TSC cycles), and the stage clock of the encoder's parts
inline u64 g_pass_cycles[4];
inline u64 g_stage_cycles[4];

// the decoder's recording (bits of every decision, in order) and the model-only replay's cursor
struct Recording {
    bool on = false;
    std::size_t n = 0, cap = 0;
    u8* bits = nullptr;
};
inline Recording g_rec;
inline const u8* g_bits = nullptr;

// the model-only copies' sink: what an MQ coder would receive, folded so the work is not removed
inline u32 g_sink;

}  // namespace ebc

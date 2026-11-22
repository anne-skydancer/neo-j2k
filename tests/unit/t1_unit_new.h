// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The redesigned decoder's side of the table and single-decision tests of t1_unit.cpp: the decision as src/t1_mq.h's
// macro expands it, on the library's own coder struct.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../src/t1.h"
#include "../../src/t1_mq.h"

// The 94 state words and the contexts' initial states against Table C.2 (the caller's rows: qe, nmps, nlps, sw).
template <class C2>
bool t1_unit_check_new_tables(const C2* rows)
{
    namespace d = nj2k::detail;
    bool ok = true;
    for (std::uint32_t s = 0; s < 47; ++s) {
        for (std::uint32_t m = 0; m < 2; ++m) {
            const std::uint32_t want = rows[s].qe | (m << 16) | ((rows[s].nmps * 2 + m) << 17) |
                                       ((rows[s].nlps * 2 + (rows[s].sw ? 1 - m : m)) << 24);
            if (d::kMqWords.w[s * 2 + m] != want) {
                std::fprintf(stderr, "kMqWords[%u] = %08x, Table C.2 gives %08x\n", s * 2 + m, d::kMqWords.w[s * 2 + m], want);
                ok = false;
            }
        }
    }
    for (std::uint32_t i = 0; i < 19; ++i) {
        const std::uint32_t idx = i == T1_CTXNO_UNI ? 92 : i == T1_CTXNO_AGG ? 6 : i == T1_CTXNO_ZC ? 8 : 0;
        if (d::kMqContextsInit.slot[i] != d::kMqWords.w[idx]) {
            std::fprintf(stderr, "context %u starts at the wrong state\n", i);
            ok = false;
        }
    }
    return ok;
}

inline std::uint32_t t1_unit_index_of(std::uint32_t word)
{
    std::uint32_t i = 0;
    while (i < 94 && nj2k::detail::kMqWords.w[i] != word) {
        ++i;
    }
    return i;
}

// The redesigned decision from the state (index state, interval a, code register c, bit counter ct, bytes by[0..3] then
// 0xFF) gives the bit d, the registers a, c, ct, the byte position bp and the next state index.
inline bool t1_unit_one_decision_new(std::uint32_t a, std::uint32_t state, std::uint32_t c, std::uint32_t ct,
                                     const std::uint8_t* by, std::uint32_t d, std::uint32_t ea, std::uint32_t ec,
                                     std::uint32_t ect, std::uint32_t ebp, std::uint32_t estate)
{
    nj2k::detail::Mqc m{};
    m.ctx = nj2k::detail::kMqContextsInit;
    m.ctx.slot[0] = nj2k::detail::kMqWords.w[state];
    std::uint8_t buf[5];
    std::memcpy(buf, by, 4);
    buf[4] = 0xFF;
    m.bp = buf;
    std::uint32_t aa = a, cc = c, cct = ct, dd;
    std::uint32_t* curctx = &m.ctx.slot[0];
    opj_mqc_decode_macro(dd, &m, curctx, aa, cc, cct);
    const std::uint32_t es = t1_unit_index_of(m.ctx.slot[0]);
    if (dd != d || aa != ea || cc != ec || cct != ect || static_cast<std::uint32_t>(m.bp - buf) != ebp || es != estate) {
        std::fprintf(stderr, "the new decision differs: a=%04x state=%u c=%08x ct=%u: d %u/%u a %04x/%04x c %08x/%08x ct %u/%u bp %u/%td state %u/%u\n",
                     a, state, c, ct, dd, d, aa, ea, cc, ec, cct, ect, ebp, m.bp - buf, es, estate);
        return false;
    }
    return true;
}

// The redesigned run of AGG decisions over cols empty columns from (a, c) with the AGG state (MPS 0) takes `run` of them
// (the decisions one at a time did), and takes no more where a column has flags; its registers fall by n * qe.
inline bool t1_unit_agg_run_new(std::uint32_t state, std::uint32_t a, std::uint32_t c, std::uint32_t cols,
                                std::uint32_t run)
{
    const std::uint32_t qe = nj2k::detail::kMqcStates[state].qeval;
    for (std::uint32_t at = 0; at <= cols; at += (at < 2 ? 1 : 3)) {
        // a column with flags at index `at` (none when at == cols); column 0 is the one being decided
        std::uint32_t f[66] = {};
        if (at < cols) {
            f[at] = 0x10;
        }
        const std::uint32_t word = nj2k::detail::kMqWords.w[state];
        std::uint32_t slot = word, aa = a, cc = c, n;
        opj_mqc_agg_run_macro(n, aa, cc, &slot, f, cols);
        // a flag at column 0 is not looked at (the caller found that column empty); at k stops the run before column k
        const std::uint32_t want = (at > 0 && at < run) ? at : run;
        if (n != want || aa != a - n * qe || cc != c - ((n * qe) << 16) || slot != word) {
            std::fprintf(stderr, "AGG run macro: state %u a=%04x c=%08x cols %u flags at %u: %u columns, want %u\n", state, a, c,
                         cols, at, n, want);
            return false;
        }
    }
    return true;
}

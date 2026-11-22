// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
// The pass-timing copy of src/enc_t1.cpp (TSC cycles a pass type; no counters).
#define STUDY_NS study_passtime
#define STAT(x) ((void)0)
#define PASSTIME_BEGIN() const std::uint64_t pt0 = __rdtsc()
#define PASSTIME_END(t) (g_stats.pass_cycles[t] += __rdtsc() - pt0)
#include "enc_t1_count_body.inc"

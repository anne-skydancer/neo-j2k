// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
// The counting copy of src/enc_t1.cpp (decisions, MPS/LPS, renormalisations, byte-outs).
#define STUDY_NS study_count
#define STAT(x) (g_stats.x)
#define PASSTIME_BEGIN() ((void)0)
#define PASSTIME_END(t) ((void)0)
#include "enc_t1_count_body.inc"

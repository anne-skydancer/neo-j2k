// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
// Fused-design candidate f6: locals+idx+select (bit loop) (tools/make_fused.py, tools/fused_mq.inc).
#define STUDY_NS study_f6
#define STUDY_LOCALS 1
#define STUDY_CLZ 0
#define STUDY_IDX 1
#define STUDY_BRANCHLESS 1
#include "enc_t1_fused_body.inc"

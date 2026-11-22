// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
// Fused-design candidate f4: locals+clz+idx+select (tools/make_fused.py, tools/fused_mq.inc).
#define STUDY_NS study_f4
#define STUDY_LOCALS 1
#define STUDY_CLZ 1
#define STUDY_IDX 1
#define STUDY_BRANCHLESS 1
#include "enc_t1_fused_body.inc"

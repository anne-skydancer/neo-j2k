// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
// Fused-design candidate f3: locals+clz+idx (tools/make_fused.py, tools/fused_mq.inc).
#define STUDY_NS study_f3
#define STUDY_LOCALS 1
#define STUDY_CLZ 1
#define STUDY_IDX 1
#define STUDY_BRANCHLESS 0
#include "enc_t1_fused_body.inc"

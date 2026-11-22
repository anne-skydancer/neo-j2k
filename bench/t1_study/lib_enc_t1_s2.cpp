// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
// Stage candidate s2 as the library's enc_t1.cpp (nj2k::detail), for the end-to-end encode: clz, packed index states, MPS/LPS by select (f5).
#define STUDY_NS detail
#define STUDY_LOCALS 0
#define STUDY_CLZ 1
#define STUDY_IDX 1
#define STUDY_BRANCHLESS 1
#include "proto/enc_t1_fused_body.inc"

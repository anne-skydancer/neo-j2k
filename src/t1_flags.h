// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// OpenJPEG 2.5.4's Tier-1 context numbers and flag bits (t1.h), shared by the decoder (t1.cpp) and the
// encoder (enc_t1.cpp); their notice is in t1.cpp.
#pragma once

// t1.h: the contexts and the flags of opj_flag_t
#define T1_NUMCTXS_ZC 9
#define T1_NUMCTXS_SC 5
#define T1_NUMCTXS_MAG 3
#define T1_NUMCTXS_AGG 1
#define T1_NUMCTXS_UNI 1
#define T1_CTXNO_ZC 0
#define T1_CTXNO_SC (T1_CTXNO_ZC + T1_NUMCTXS_ZC)
#define T1_CTXNO_MAG (T1_CTXNO_SC + T1_NUMCTXS_SC)
#define T1_CTXNO_AGG (T1_CTXNO_MAG + T1_NUMCTXS_MAG)
#define T1_CTXNO_UNI (T1_CTXNO_AGG + T1_NUMCTXS_AGG)
#define T1_TYPE_MQ 0
#define T1_TYPE_RAW 1
#define T1_SIGMA_0 (1U << 0)
#define T1_SIGMA_1 (1U << 1)
#define T1_SIGMA_2 (1U << 2)
#define T1_SIGMA_3 (1U << 3)
#define T1_SIGMA_4 (1U << 4)
#define T1_SIGMA_5 (1U << 5)
#define T1_SIGMA_6 (1U << 6)
#define T1_SIGMA_7 (1U << 7)
#define T1_SIGMA_8 (1U << 8)
#define T1_SIGMA_10 (1U << 10)
#define T1_SIGMA_13 (1U << 13)
#define T1_SIGMA_15 (1U << 15)
#define T1_SIGMA_16 (1U << 16)
#define T1_SIGMA_17 (1U << 17)
#define T1_CHI_0_I 18
#define T1_CHI_1 (1U << 19)
#define T1_CHI_1_I 19
#define T1_MU_0 (1U << 20)
#define T1_PI_0 (1U << 21)
#define T1_CHI_2 (1U << 22)
#define T1_CHI_2_I 22
#define T1_PI_1 (1U << 24)
#define T1_PI_2 (1U << 27)
#define T1_PI_3 (1U << 30)
#define T1_CHI_5_I 31
#define T1_SIGMA_NW T1_SIGMA_0
#define T1_SIGMA_N T1_SIGMA_1
#define T1_SIGMA_NE T1_SIGMA_2
#define T1_SIGMA_W T1_SIGMA_3
#define T1_SIGMA_THIS T1_SIGMA_4
#define T1_SIGMA_E T1_SIGMA_5
#define T1_SIGMA_SW T1_SIGMA_6
#define T1_SIGMA_S T1_SIGMA_7
#define T1_SIGMA_SE T1_SIGMA_8
#define T1_SIGMA_NEIGHBOURS \
    (T1_SIGMA_NW | T1_SIGMA_N | T1_SIGMA_NE | T1_SIGMA_W | T1_SIGMA_E | T1_SIGMA_SW | T1_SIGMA_S | T1_SIGMA_SE)
#define T1_CHI_THIS T1_CHI_1
#define T1_CHI_THIS_I T1_CHI_1_I
#define T1_MU_THIS T1_MU_0
#define T1_PI_THIS T1_PI_0
#define T1_NMSEDEC_BITS 7
#define T1_NMSEDEC_FRACBITS (T1_NMSEDEC_BITS - 1)

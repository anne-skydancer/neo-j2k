<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026, Anne Skydancer (neo-j2k) -->

# Next-Generation codec program (alternative) vs the staged MQ plans

Status: under comparison, nothing decided. Written 2026-11-22.

## Two candidate directions

**Plan S (staged, measured):** fix the MQ coders where measurements show gains.
- Encoder: `doc/studies/enc_t1_study.md`, decisions in `doc/studies/mq-encoder-redesign-decisions.md`.
  Kernel 0.57-0.61x, whole encode 0.60-0.67x.
- Decoder: `doc/studies/mqng_study.md`. Tier-1 0.72x lossless, 0.79-0.85x lossy.
- About 4 days per side. Byte/coefficient-identical, gated on exact, faster, leaner.

**Plan N (the NG briefs, Revision 0.1, 22 Nov 2026):** a C++20 program of five components.

| Brief | Component | Relation to Plan S |
|---|---|---|
| Brief 5 | MQ-NG Encode | Same target as the encoder study. |
| Brief 4 | MQ-NG Decode | Same target as the decoder study. Brief 4's per-call `Result<bool>` conflicts with the study (sticky `exhausted` bit per segment). |
| Brief 3 | EBCOT-NG Tier-1 (bit-packed stripe/column masks, one engine for both directions) | New. Modelling is the larger half of Tier-1 once the MQ stages land. Under study. |
| Brief 2 | Encoder-NG (task graph, SIMD DWT/quant, rate control) | neo-j2k already has code-block parallelism; wavelet is 2.5-6.6 % of one-thread encode. Low expected gain. |
| Brief 1 | Decoder-NG (Tier-2, scheduler, SIMD inverse DWT) | Tier-1 is 93-99 % of one-thread decode. Low expected gain. |

Shared across both: Part 1 only, no HTJ2K, scalar reference as oracle, SIMD opportunistic,
"architectural obsolescence motivates redesign, not a presumed exploit".

## Measured facts that bear on the comparison

- Tier-1 share of one-thread encode 86-95 %, of decode 93-99 %.
- After the MQ stages, modelling (the passes) is the larger part of Tier-1 on both sides.
- 8-lane AVX2 across blocks rejected: 1.41x on MQ alone, loses before modelling is added.
- Parallel passes inside a code-block are impossible (significance dependencies).

## Decoder study decisions (owner, 2026-11-22)

- Stage 2 (cleanup-pass MPS runs): accepted as measured (6-11 % lossy at 512² and above,
  2-5 % at 256² lossy, neutral on lossless).
- Byte input: keep OpenJPEG's 0xFF 0xFF sentinel write and restore (bounds-checked
  read-only input is worth only 0-2 %, so noise). Add one length check per block, which
  closes the contract gap with Tier-2. The padded read-only layout and sticky `exhausted`
  bit wait for the Rust port. (Recommended default; owner asked what it meant and said
  to accept it as noise unless there is a good reason.)
- Shared state table: separate now (decoder 94 words, encoder 376 bytes), converge later.
- Decision form: macros in `t1.cpp` as today (owner changed this from the recommended
  inline functions in `src/mq.h`; easier to integrate). The backend seam stays at
  `t1_decode_cblk`. Inline functions measured within 0.8-1.8 %, so this costs no speed;
  macro-versus-function form is a code-layout effect to re-measure at stage 1.
- Defaults taken from the surveyor, not asked: SIMD lanes closed for AVX2; over-long
  segments cut to the bytes present; Rust port after the C++ stages land.

## EBCOT-NG study result and decisions (owner, 2026-11-22)

Study: `doc/studies/ebcotng_study.md`. Verdict: a bit-packed Tier-1 redesign does not pay (state is
at most 10-13 % of encoder Tier-1, 3-7 % of decoder Tier-1). Plan N is therefore folded into
Plan S; only its MQ-NG parts remain, plus three targeted encoder steps.

- Program: no EBCOT-NG program. Three encoder steps after the MQ stages, one PR each:
  E1 branch-free preprocessing, E2 cleanup driven by a mask per column, E3 refinement as a
  (context, bit) list then one MQ loop. Together 0.83-0.87 of the stage-2 kernel.
- E1 form: branch-free scalar code, no AVX2 intrinsics.
- E3 form: pair list (+4 KB scratch per concurrent encoder job).
- Decoder refinement step D1: re-measure after MQ-NG lands; keep only if at least 3 % on
  every configuration (surveyor default, not asked).
- Significance-pass split: not adopted (surveyor default, not asked).
- Order: MQ encoder stages 1-2, MQ-NG decoder, E1-E3, then D1 if it still clears 3 %.
- Gate for each step: exact, faster beyond noise, leaner (more done with less).
- Estimates: encoder kernel 0.49-0.51 of today's, whole one-thread encode 0.52-0.56.

## Open

- EBCOT-NG study (surveyor, in progress): measures where modelling time goes, prototypes
  packed-state designs, and compares Plan N against Plan S.
- Pending owner decisions from the decoder study (stage 2 gate, byte input layout, shared
  table, decision form) are re-asked after this comparison.
- Rust port follows the C++ in either plan, written fresh for neoj2k-rust.

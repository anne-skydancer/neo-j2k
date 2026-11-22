# MQ encoder redesign: owner decisions

Status: decided, not started. Idea A of two being compared (idea B is still to come).
Study with all numbers: `doc/enc_t1_study.md`. Benches: `bench/t1_study/` (uncommitted).

## Idea

Redesign the encoder's MQ coder and Tier-1 so the codestream stays byte-identical
(OpenJPEG-compatible, Part 1) but encoding is faster and leaner.

## Scope decisions

| Question | Decision |
|---|---|
| Which coder | Encoder only. Decoder MQ tweaks are a later, separately benched follow-up. |
| Which implementation | C++ first. The Rust port follows later through the rust-transcriber. |
| What gets faster | One-thread speed per code-block. No bit-plane parallelism. |
| HTJ2K | Out of scope (different codestream). |
| SIMD lanes across code-blocks | Rejected: after stage 2 the coder is about 40 % of Tier-1. |
| (context, bit) buffer | Rejected in favour of the fused loop (0.61-0.64x and much more scratch). |

## Gate rule (every step)

- **Exact:** byte-identical codestream.
- **Faster:** beyond the measured noise band (the study uses 3 % or more).
- **Leaner:** does more and better with less. It does not mean less code. Measured as
  work per symbol: instructions, branches, memory accesses, scratch bytes per block,
  table footprint.

## Implementation decisions

1. `mq_shifts` (count-leading-zeros helper, `src/t1.cpp:188`): move to `t1.h` and share.
2. Packed MQ state table: a new encoder-only table (+376 bytes read-only), built at
   compile time from `kMqcStates`. The decoder is not moved to it.
3. Stage 2 (1-byte state index plus branch-free selects) is one commit. The index alone is
   within noise, so a split would fail the "faster" gate. (Default taken from the
   surveyor's recommendation; not separately asked.)
4. Exhaustive single-decision test: a trimmed version (a few seconds) in ctest, the full
   ~20-second run nightly.
5. Passes stage (register-held refinement contexts, sample-mask-driven scans, est. 5-15 %,
   unprototyped): not now, a separate study later.

## Plan

- **Stage 0, tests first (about 1 day):** frozen copy of today's `MqEnc` as reference;
  per-pass-end `numbytes()`, bytes and overflow flag checks at full and short capacities;
  exhaustive single-decision test; `enc_t1_encode` on generated blocks (all sizes and
  orientations, 5/3 and 9/7).
- **Stage 1 (about 0.5 day):** renormalise by count-leading-zeros. Kernel 0.825-0.830x.
- **Stage 2 (about 1 day):** 1-byte state index into a 376-byte table plus selects.
  Kernel 0.573-0.591x; whole encode 0.60-0.67x. Per-block context state 152 to 19 bytes.
- **Stage 3 (about 0.5 day):** `doc/bench.md` section and README row.

Target: kernel at most 0.62x and encode at most 0.68x on every configuration, AVX2 and
SCALAR, byte-identical. Pipeline for the code: coder, then reviewer + auditor, coder
fixes, checker.

## Measured baseline (Ryzen 9 9950X3D, MSVC, one thread)

- Tier-1 is 86-95 % of the one-thread encode (wavelet 2.5-6.6 %, rate allocation plus
  Tier-2 0.1-3.6 %). Stop gate passes.
- Inside Tier-1: MQ about 60 % (33 cycles a decision), modelling about 40 %.
- 5.0-7.05 decisions a sample; about 35 % LPS; 66-70 % renormalise.
- Today's encode is about 1.14x OpenJPEG's time; after stage 2 about 0.7x.

## Risks and open checks

- Measured on MSVC only. Run stage 2's gate on GCC and Clang too: selects may turn back
  into branches.
- No hardware counters; branch and memory figures are derived.
- Synthetic texture; corpus timing not measured (corpus bytes are pinned by `encode_unit`).
- The output-limit clamp is unreachable with 8-bit samples; its exactness rests on the
  short-buffer and exhaustive tests.
- One Zen 5 machine only.

## Housekeeping

`doc/encoder-tier1-plan.md` is untracked in the repo and was not written by the surveyor.
Check where it came from before committing anything.

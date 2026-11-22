<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026, Anne Skydancer (neo-j2k) -->

# The encoder's MQ coder and Tier-1: study and plan (2026-11-22)

Scope (owner): the encoder only (`src/enc_t1.cpp`: `EncT1`, `MqEnc`, the three passes), C++ first, one thread
per code-block faster, output byte for byte today's (OpenJPEG 2.5.4's, Part 1), no bit-plane parallelism, no
HTJ2K, no SIMD lanes across code-blocks unless the MQ coder still dominates after the redesign. Gate of every
step: **exact** (byte-identical), **faster** (beyond the measured noise band), **leaner** (less work per
symbol: branches, memory accesses, renormalisation steps, scratch and table bytes; code size is not the measure).

Machine: Ryzen 9 9950X3D, Windows 11, MSVC 14.51, the library's flags (`/O2 /arch:AVX2 /fp:precise`, no
exceptions, `/guard:cf`, no LTCG as the static library), one thread pinned to CPU 7, high priority, TSC cycles
(4.292 GHz nominal). Not a quiet machine (Discord, Logitech and other background processes; about 40 % total
load at the start), which is why every number below is a ratio of interleaved runs or a median of repeats.
Nothing was measured on other hardware; the expectations for other CPUs at the end are estimates.

## 1. Measure first

### Tools (bench/t1_study, not part of the main build, uncommitted)

- `CMakeLists.txt`: a standalone project that builds the library from `src/` twice with the library's flags:
  `lib_head` unchanged, and `lib_instr`, whose `encode.cpp` is a copy with a stage clock and a code-block dump
  inserted at configure time (anchors checked; `src/` is never written). It also builds the stage candidates
  as whole libraries (`lib_s1`, `lib_s2`: `enc_t1.cpp` replaced), for the end-to-end encode.
- `t1_profile`: the viewer's upload encode (the texture and parameters of `bench/nj2k_encode_bench.cpp` and
  `tests/common/replay.inl`, comment "no comment") with the stage clock, and the dump of every code-block's
  Tier-1 input.
- `t1_replay`: the dumped code-blocks through the library's `enc_t1_encode` and through the study's copies
  (`proto/`), every result checked against the library's (bytes, and every pass's rate, len, term and
  distortion bit for bit), then timed in interleaved rounds, the order rotating each round. It also runs a
  counting copy (decisions, MPS/LPS, renormalisations, byte-outs, contexts), a pass-timing copy, the split
  design (modelling into a (context, bit) buffer, then the MQ coder), the MQ variants alone on the recorded
  decision streams, and the pass-end and output-limit check (below).
- `t1_e2e`, `t1_e2e_s1`, `t1_e2e_s2`: the whole upload encode through the unchanged library and the two
  stage candidates: median cycles and the codestream's FNV-1a hash.
- `mq_exhaustive`: one MQ decision of the stage-2 coder against today's `MqEnc` over every interval, state,
  bit and bit counter (section 4).
- `tools/make_fused.py` + `tools/fused_mq.inc`: generate the fused candidates from `src/enc_t1.cpp` (the
  passes' text unchanged; the coder switched by `STUDY_LOCALS`, `STUDY_CLZ`, `STUDY_IDX`, `STUDY_BRANCHLESS`).

The study's encode is checked against the oracle: its codestreams have the oracle's size and hash at all six
configurations (`build/s6a` `nj2k_encode_bench --backend oracle`: 24,385 / e903a0d31c9f708e at 256² lossy,
129,529 / 5a78ddcfde5d2f9e at 256² lossless, 98,295, 595,252, 393,225, 2,398,977 bytes at 512² and 1,024²,
hashes equal).

### Stage shares, one thread (three components, median of 15 runs, 7 at 1,024²; verified)

| Stage | 256² lossy | 256² lossless | 512² lossy | 512² lossless | 1,024² lossy | 1,024² lossless |
|---|---|---|---|---|---|---|
| upload total, Mcycles | 64.1 | 68.0 | 292.7 | 307.1 | 1,192.7 | 1,235.1 |
| start_compress (copy, header) | 0.6 % | 0.5 % | 0.4 % | 0.4 % | 0.4 % | 0.3 % |
| DC shift + colour transform | 0.4 % | 0.1 % | 0.4 % | 0.1 % | 0.4 % | 0.1 % |
| wavelet | 6.6 % | 2.8 % | 5.6 % | 2.5 % | 6.0 % | 2.6 % |
| Tier-1 copy-in and quantisation | 0.7 % | 0.2 % | 0.6 % | 0.2 % | 0.6 % | 0.3 % |
| **Tier-1 kernel (`enc_t1_encode`)** | **86.1 %** | **94.2 %** | **89.1 %** | **95.3 %** | **88.6 %** | **95.2 %** |
| Tier-1 commit | 0.2 % | 0.2 % | 0.2 % | 0.2 % | 0.2 % | 0.2 % |
| rate allocation + Tier-2 | 3.6 % | 0.2 % | 2.5 % | 0.1 % | 2.6 % | 0.1 % |

**Stop gate (Tier-1 under about half of the encode means stop or shrink): passes by far.** Tier-1 is 86-95 %
of the one-thread encode at every size and mode.

### Inside Tier-1: MQ coder against modelling (verified, `t1_replay`)

The split copy shows the two halves are additive: the passes writing one byte a decision (modelling alone)
take 0.36-0.39 of the kernel, today's `MqEnc` coding the recorded decisions alone 0.58-0.60, and the two
together (split M0) 0.97-0.98. **Today the MQ coder is about 60 % of Tier-1 and the modelling about 40 %.**

| Per code-block set | 256² lossy | 256² lossless | 512² lossy | 512² lossless | 1,024² lossy | 1,024² lossless |
|---|---|---|---|---|---|---|
| decisions | 983,683 | 1,155,862 | 4,805,178 | 5,446,302 | 19,676,737 | 22,182,015 |
| decisions a sample | 5.00 | 5.88 | 6.11 | 6.93 | 6.26 | 7.05 |
| bit-planes a code-block | 6.15 | 6.01 | 7.39 | 7.85 | 7.48 | 8.13 |
| decisions sig / ref / cln % | 47 / 24 / 29 | 42 / 33 / 25 | 41 / 30 / 28 | 38 / 37 / 25 | 41 / 30 / 29 | 37 / 37 / 26 |
| LPS % of decisions | 35.5 | 36.8 | 35.2 | 36.2 | 34.8 | 35.8 |
| MPS with renormalisation % | 32.4 | 33.5 | 32.0 | 32.8 | 31.7 | 32.5 |
| renormalisations % of decisions | 67.9 | 70.3 | 67.3 | 69.0 | 66.5 | 68.3 |
| shifts a renormalisation | 1.28 | 1.27 | 1.27 | 1.26 | 1.27 | 1.26 |
| byte-outs a decision | 0.109 | 0.112 | 0.107 | 0.109 | 0.106 | 0.108 |
| kernel cycles a decision, today | 57.7 | 56.9 | 55.3 | 54.7 | 55.0 | 54.4 |
| MQ alone (today's `MqEnc`), cycles a decision | 33.6 | 34.2 | 32.7 | 33.1 | 32.4 | 32.7 |
| modelling alone, cycles a decision | 22.6 | 21.0 | 21.2 | 19.9 | 20.9 | 19.8 |
| pass shares today (time) sig / ref / cln % | 52 / 28 / 20 | 48 / 34 / 19 | 48 / 33 / 19 | 45 / 37 / 18 | 48 / 33 / 19 | 45 / 37 / 18 |

What the numbers say: a third of all decisions are LPS and two thirds renormalise, so the coder's MPS/LPS
branch and its conditional-exchange branch are unpredictable (inferred from the rates; no PMU access on this
machine: uProf is not installed and the TSC is all the bench reads). The contexts most used are the
refinement contexts 15 and 16 (14 % and 19 % of decisions at 256² lossless), then ZC 7, 8 and the AGG context.

## 2. Candidates measured (verified, all byte-identical)

Kernel time against the library, the median of 11 interleaved rounds (9 at 1,024²). The noise band: the
library's own source compiled into another object (`copy`) gives 0.993-0.999; the same coder written another
way with the same operations (`f0`) gives 0.977-0.981, so **code layout moves the kernel about 2 %, and a step
must gain at least 3 % on every configuration** to count. Run-to-run spread of one variant: 0.3-2 %.

| Candidate (fused: the MQ coder inside the passes, as today) | 256² y | 256² l | 512² y | 512² l | 1,024² y | 1,024² l |
|---|---|---|---|---|---|---|
| f0 control: today's coder rewritten, same operations | 0.977 | 0.979 | 0.981 | 0.979 | 0.979 | 0.977 |
| f10 packed index states only | 0.986 | 0.980 | 0.985 | 0.977 | 0.981 | 0.976 |
| f1 coder in locals a pass only | 0.903 | 0.898 | 0.902 | 0.897 | 0.899 | 0.893 |
| **f9 renormalisation by clz (stage 1)** | **0.825** | **0.826** | **0.829** | **0.830** | **0.828** | **0.827** |
| f11 clz + packed index | 0.829 | 0.829 | 0.835 | 0.832 | 0.833 | 0.829 |
| f2 locals + clz | 0.830 | 0.833 | 0.836 | 0.833 | 0.832 | 0.831 |
| f7 MPS/LPS by select, pointer states | 0.867 | 0.865 | 0.872 | 0.871 | 0.869 | 0.869 |
| f8 select + packed index, bit loop | 0.771 | 0.760 | 0.773 | 0.765 | 0.776 | 0.765 |
| **f5 clz + packed index + select (stage 2)** | **0.589** | **0.573** | **0.591** | **0.578** | **0.591** | **0.578** |
| f4 f5 + coder in locals | 0.599 | 0.581 | 0.598 | 0.586 | 0.598 | 0.587 |
| split M0: (context, bit) buffer, today's coder | 0.980 | 0.973 | 0.983 | 0.975 | 0.979 | 0.973 |
| split M7: buffer, clz + packed + select | 0.630 | 0.611 | 0.636 | 0.623 | 0.638 | 0.625 |

The MQ coder alone on the recorded decisions (fraction of today's kernel; M0 is today's `MqEnc`):

| MQ alone | 256² y | 256² l | 512² y | 512² l | 1,024² y | 1,024² l |
|---|---|---|---|---|---|---|
| M0 today | 0.582 | 0.601 | 0.592 | 0.604 | 0.589 | 0.602 |
| M1 clz | 0.481 | 0.500 | 0.495 | 0.510 | 0.492 | 0.503 |
| M2 locals + clz | 0.373 | 0.387 | 0.379 | 0.390 | 0.378 | 0.389 |
| M3 M2 + last byte in a register | 0.395 | 0.414 | 0.403 | 0.414 | - | - |
| M4 M2 + packed index | 0.372 | 0.385 | 0.378 | 0.390 | 0.376 | 0.388 |
| M6 select, pointer states | 0.449 | 0.457 | 0.462 | 0.473 | - | - |
| **M7 clz + packed index + select** | **0.238** | **0.244** | **0.252** | **0.257** | **0.254** | **0.259** |

M7 codes a decision in about 14 cycles against today's 33.

What was learnt:
- **The encoder knows its bit**, so the MPS/LPS choice, the conditional exchange and the state update are
  selects (`cmov`), not branches. This is the big lever (f5 against f9: 0.70x). The decoder could not use it
  (`doc/bench.md` stage 4: its select was 3-5 % slower, the decoded bit being branched on right after); the
  encoder has no such branch. It pays only with index states (f7, pointer selects: 0.87) and with the clz
  renormalisation (f8, bit loop: 0.77), because a renormalisation that is always run (shift 0 when bit 15
  stays) must cost one compare, not a loop.
- **Renormalisation by count-leading-zeros** alone: 0.83x, the decoder's stage-4 change carried to the encoder.
- **Locals** (the coder copied into a local for each pass, against MSVC's lack of type-based alias analysis)
  pay only before the select (f1: 0.90); after it, f4 and f5 are within layout noise of each other (f4 1-1.5 %
  slower in the first build, 1.3-1.8 % faster in the second): not kept, as it adds a copy in and out a pass
  for no gain beyond the noise band.
- **The (context, bit) buffer loses to the fused loop** once the coder is fast (split M7 0.62-0.64 against
  f5 0.57-0.59), and it needs a buffer: 39 KB a code-block on these images (the largest trace was 39,079
  bytes), 307 KB at the worst-case bound (three decisions a sample a bit-plane, 25 bit-planes), or about
  10 KB if coded a pass at a time (2.5 decisions a sample at most in a pass). It is the fused loop.
- Last byte in a register (M3, M5) is slower: the byte-out is rare (0.11 a decision) and the reload is a
  store-forward. Dropped.

After stage 2, per pass (f4, Mcycles): significance 15.4, refinement 11.2, cleanup 9.2 at 256² lossless
(today 29.2, 21.1, 12.2); 258.9, 181.5, 166.5 at 1,024² lossy (today 482.7, 337.5, 203.3). The coder is now about 0.25 of today's kernel against the modelling's 0.37: **the
MQ coder no longer dominates, so by the owner's rule SIMD lanes across code-blocks stay out** (and stage 4's
lane prototype already bounded them at 1.4x on the coder alone).

## 3. Current state (verified, file:line at bee41d4)

- `src/enc_t1.cpp:97-230` `MqEnc`: OpenJPEG's `mqc.c` encoder as members: `c, a, ct, bp, start, limit,
  overflow`, 19 context slots of `const MqState*` (152 bytes), `curctx` a pointer to the slot.
  `byteout` (133-167) clamps at the output limit (135-139, OD-4: `overflow`, `bp = limit - 1`) and then
  stuffs and carries as OpenJPEG. `renorme` (169-179) shifts one bit a turn with `byteout` when `ct` reaches 0.
  `encode` (181-207) branches on MPS/LPS, the MPS's bit 15 and the conditional exchange. `flush` (218-229).
- `src/t1_tables.h:15` `kMqcStates[94]`, 24 bytes each (2,256 bytes: qeval, mps, two pointers), shared
  with the decoder (`src/t1.cpp:345-352`, `1222`, `1274`).
- The passes (`enc_t1.cpp:328-553`): `enc_sigpass`, `enc_refpass`, `enc_clnpass` and their steps, calling
  `mqc->setcurctx` and `mqc->encode` through `t1->mqc` (the members in memory, behind every flag store).
- `enc_t1_encode` (604-759): flags set-up, sign-magnitude and `numbps`, the states reset (687-691), the pass
  loop, **the rates read at every pass end** (712-721: `numbytes() + 3`, the flushed `numbytes()` for the last
  cleanup pass), `overflow` gives `false` (729-731), the rates made non-decreasing (735-746) and the 0xFF rule
  (748-756). So `bp` must be exact at every pass boundary (planner's finding confirmed), and the clamp must
  set `overflow` at the same byte; the bytes after a clamp do not matter, since the code-block then fails
  (`NJ2K_E_LIMIT`, `src/encode.cpp:795-797`).
- `nmsedec` is an `int32` sum of table values (`enc_t1.cpp:344`, `401`, `495`) and the distortion a double
  from it per pass; stages 1 and 2 do not touch it (the passes' text is unchanged).
- No mode switches: `src/encode.cpp:215-216` refuses them (planner's finding confirmed), so only the MQ coder
  with one termination at the end exists here; RAW, RESTART, ERTERM and the rest are the j2k-extra module's.
- Callers and threads (`src/encode.cpp`): `t1_job` (582-656) fills `enc_t1_data` and calls `enc_t1_encode`;
  `encode_tile` (725-814) runs the code-blocks in batches of at most the executor's width (64), one `EncT1` a
  concurrent job (`jobs.t1[index]`, created 748-753, destroyed 808-810), on the host's executor threads from
  2^16 samples (`kParallelEncodeFloor`, 701) and serially below; `enc_t1_commit` (761-776) runs on the calling
  thread in code-block order. `EncT1` is never shared between jobs; the tables are `constexpr`. The new table
  must stay `constexpr` (no dynamic initialisation, no first-use race between executor threads).
- `src/t1.cpp:184-201` `mq_shifts`: the decoder's clz (lzcnt in the AVX2 build, `_BitScanReverse` in the
  scalar build, whose object check refuses the runtime lzcnt of MSVC's `std::countl_zero`).
- Builds: AVX2 (x86-64-v3) and SCALAR (baseline x86-64) on MSVC, clang-cl, GCC and Clang (`CMakePresets.json`),
  sanitizer and fuzz presets. Tests that pin the bytes: `tests/unit/encode_unit.cpp` (against the oracle, every
  shape and setting the viewer sends, the corpus images), `fuzz/fuzz_encode.cpp`, `fuzz/fuzz_roundtrip.cpp`.
  None reaches the output clamp: 8-bit samples cannot fill 4 bytes a sample.

## 4. Design

### Stage 1: renormalisation by count-leading-zeros

`renorme` becomes: `k = mq_shifts(a)`; `a <<= k`; while `k >= ct`: `c <<= ct`, `k -= ct`, `byteout()`; then
`c <<= k`, `ct -= k`. The byte-outs happen at the same `c` and `ct` as the one-bit loop (it calls `byteout`
exactly when `ct` reaches 0; shifting by `ct` at once leaves the same `c` modulo 2^32), so the bytes, `bp` at
every pass end and the clamp are the loop's. `mq_shifts` is the decoder's helper (`t1.cpp:188`), moved to
`t1.h` and its domain comment widened to [1, 0xffff] (0 when bit 15 is set, needed by stage 2).

- Kernel 0.825-0.830 of today's, end to end 0.855-0.865 (below).
- Leaner: a renormalisation's loop of 1.27 turns (2 conditional branches and 3 shifts a turn) becomes one
  `lzcnt`, one compare and 2 shifts, plus a compare a byte-out: conditional branches in the coder from about
  4.3 to about 3.4 a decision. No state or table change.

### Stage 2: packed index states, MPS/LPS by select

- Each context's state is one byte, the index `state * 2 + mps` into `kMqcStates`. A new `constexpr` table of
  94 entries of 4 bytes (`qe` 16 bits, `nmps`, `nlps` 8 bits each; the MPS is the index's low bit), built at
  compile time from `kMqcStates` (`constexpr` constructor, so the numbers cannot drift from the decoder's).
- `encode(d)`: `i = ctx[cx]`, `e = table[i]`, `lps = (i ^ d) & 1`, `an = a - qe`,
  `swap = (an < qe) ^ lps`, `a = swap ? qe : an`, `c += swap ? 0 : qe`,
  `ctx[cx] = (!lps && (an & 0x8000)) ? i : (lps ? e.nlps : e.nmps)`, then stage 1's renormalisation with no
  test before it (`mq_shifts` gives 0 when bit 15 stays). This is the same function as the three branches:
  the MPS takes `qe` when `an < qe` and adds `qe` to `c` otherwise, the LPS the opposite, and an MPS that keeps
  bit 15 neither renormalises nor changes state.
- `setcurctx` keeps the context number (the passes' text is unchanged); `resetstates`/`setstate` write
  indices. `curctx` and the 19 pointers go.
- Kernel 0.573-0.591 of today's (0.70x of stage 1), end to end 0.602-0.649 of today's.
- Leaner: the coder's data-dependent branches (MPS/LPS, bit 15 of the MPS, the exchange) go, 3 of them, to
  none: about 1.3 conditional branches a decision are left (the byte-out loop's compare and the byte-out's
  own). Memory a decision: a 1-byte state load, a 4-byte table load and a 1-byte state store, instead of an
  8-byte slot load, a 24-byte state's fields and an 8-byte slot store plus the `curctx` store. Per-block
  context state 152 to 19 bytes (`sizeof(EncT1)` 39,872 to 39,744). Tables: 376 bytes added, 2,256 still
  there for the decoder (**the one cost of the plan: +376 bytes of read-only data, flagged**; it goes when
  the decoder takes the same table in the benched follow-up; reading `kMqcStates` itself by index, with its
  24-byte entries and pointer successors, was not measured).
- Exactness: `mq_exhaustive` compares one decision of the new coder with today's `MqEnc` for every `a` in
  [0x8000, 0xffff], every state (94), both bits, every `ct` in [1, 12], four low words `c` (0, under the
  carry, at the carry, mixed), three previous bytes (0x00, 0xFE, 0xFF) and with the output limit at the
  previous byte or not: 1,774,190,592 single decisions, none differs (20 s on one thread). The sequences of decisions are then the 49 million
  decisions of the six configurations (identical bytes and passes on every code-block), the short-buffer
  runs (the clamp exercised 150-1,554 times a configuration with identical `overflow`, bytes and pass-end
  rates), and the whole codestreams (identical hashes, equal to the oracle's).

### Rejected, with the measurements above

The (context, bit) buffer (slower than fused and 10-307 KB of scratch), the coder in locals (no gain after
the select, adds a copy a pass), the last byte in a register (slower), pointer states with selects (slower),
SIMD lanes across code-blocks (the coder no longer dominates).

### Not designed here: the passes (a possible stage 3)

After stage 2 the modelling is about 60 % of Tier-1 (0.37 of today's kernel). Two directions, both estimates:
(a) the context-state chain: most refinement decisions use contexts 15 and 16 back to back, so the coder is
latency-bound on store, load, table load (about 12 cycles; M7 averages 14 a decision), which a refinement
pass holding its three MAG states in registers would shorten (estimated up to 10 % of Tier-1, not measured);
(b) mask-driven column scans (the 4-bit sets of samples a pass codes, iterated with `tzcnt`) instead of the
per-sample flag tests, for the significance and cleanup passes' unpredictable branches (estimated 5-15 %, not
measured). Each needs its own prototype in this harness before it is planned; it is not in the plan below.

## 5. Costs (measured, Ryzen 9 9950X3D; one thread)

End to end, the whole upload encode through the library, head and the two candidates as whole libraries,
interleaved (5 rotations of 9 encodes, 5 at 1,024²), median Mcycles; every codestream the oracle's:

| Size | Mode | Bytes | Same hash | Today | Stage 1 | Stage 2 | S1 / today | S2 / today | S2 / S1 |
|---|---|---|---|---|---|---|---|---|---|
| 256² | lossy | 24,385 | yes | 64.7 | 56.0 | 42.0 | 0.864 | 0.649 | 0.751 |
| 256² | lossless | 129,529 | yes | 68.6 | 58.8 | 41.3 | 0.857 | 0.602 | 0.703 |
| 512² | lossy | 98,295 | yes | 295.8 | 255.9 | 189.1 | 0.865 | 0.639 | 0.739 |
| 512² | lossless | 595,252 | yes | 309.8 | 264.9 | 186.4 | 0.855 | 0.602 | 0.704 |
| 1,024² | lossy | 393,225 | yes | 1,206.3 | 1,039.0 | 773.2 | 0.861 | 0.641 | 0.744 |
| 1,024² | lossless | 2,398,977 | yes | 1,250.1 | 1,071.9 | 757.4 | 0.857 | 0.606 | 0.707 |

Run-to-run spread of each cell (min to max of 5): 0.1-0.6 %.

The SCALAR build (baseline x86-64, no `/arch`, `bsr` for the clz; 3 rotations; same hashes):

| Size | Mode | Today | Stage 1 | Stage 2 | S1 / today | S2 / today |
|---|---|---|---|---|---|---|
| 256² | lossy | 65.0 | 57.4 | 43.6 | 0.883 | 0.670 |
| 256² | lossless | 68.5 | 59.0 | 42.0 | 0.862 | 0.613 |
| 1,024² | lossy | 1,205.3 | 1,065.5 | 796.1 | 0.884 | 0.661 |
| 1,024² | lossless | 1,244.4 | 1,080.2 | 765.5 | 0.868 | 0.615 | In milliseconds at 4.292 GHz, stage 2 encodes
256² lossless in 9.6 ms (today 16.0) and 1,024² lossy in 180 ms (today 281); OpenJPEG took 14.0 and 245.7 ms
in this session (separate processes, not interleaved), so neo-j2k would go from about 1.14x OpenJPEG's time to
about 0.7x. Memory: `EncT1` 128 bytes smaller a concurrent job; read-only data +376 bytes. No GPU work.

**Overall target (set from the measurements): the Tier-1 kernel at 0.62 or less of today's and the
one-thread encode at 0.68 or less of today's on every configuration, AVX2 and SCALAR builds, byte-identical.**
Stage 2 meets it with 1-7 % to spare (the small lossy encode of the SCALAR build is nearest, 0.670). The
kernel ratio of stage 2 moved from 0.591 to 0.613 at 1,024² lossy between two builds of the same operations
(code layout), so the target leaves that much room.

Other hardware (estimated): the gain comes from removing mispredicted branches and one load chain, so CPUs
with costlier mispredicts (other Zen, recent Intel cores) should see similar or larger ratios; the scalar
build uses `bsr` for `lzcnt` and keeps most of the gain (above). Arm (Rust port later) has `clz` and
`csel`; not measured.

## 6. Compatibility

Nothing a user can see changes but the time: the codestreams are byte for byte today's (and OpenJPEG's), the
rates, distortions and pass counts the rate allocation reads are bit for bit today's, the failure on an
overflowing code-block (`NJ2K_E_LIMIT`) happens at the same code-block. The API, ABI, memory bound and threading
are unchanged; each concurrent Tier-1 job needs 128 bytes less.

## 7. Staged plan

| Stage | Scope | Gate (tests, performance) | Owner's check | Effort |
|---|---|---|---|---|
| 0 | Tests first: `tests/unit/enc_t1_unit.cpp` (or a section of `encode_unit`), linking the internals: (a) a frozen copy of today's `MqEnc` as the reference, driven with recorded and random decision streams with pass ends: every pass-end `numbytes()`, the bytes and `overflow` equal, at full and at short capacities (the clamp at every byte position of a short buffer); (b) the exhaustive single-decision comparison of `mq_exhaustive` (a few seconds; nightly if too slow for every run); (c) `enc_t1_encode` on generated code-blocks (all sizes 1..64 a side, heights not a multiple of 4, every orient, 9/7 and 5/3, `numbps` 0 to the maximum) against the frozen copy of today's `enc_t1.cpp`: passes (rate, len, term, distortion bit for bit) and bytes. Today's code passes it. | Green on all presets (MSVC, clang-cl, GCC, Clang, AVX2 and SCALAR, ASan/UBSan, TSan) | that the frozen copy is today's file | 1 day |
| 1 | clz renormalisation: `mq_shifts` moved to `t1.h`, `MqEnc::renorme` rewritten (about 12 lines) | stage 0, `encode_unit`, the fuzz seeds, the oracle hashes; kernel at most 0.85 of today's on all six configurations and the end-to-end encode at most 0.90, AVX2 and SCALAR (measured 0.825-0.830; 0.855-0.865 AVX2, 0.862-0.884 SCALAR); interleaved, cycles, 5+ rotations | the numbers in `doc/bench.md` | 0.5 day |
| 2 | Packed index states and the select: the `constexpr` table (in `enc_luts.h`, built from `kMqcStates`), `MqEnc` slots as bytes, `encode` as in section 4 | as stage 1; kernel at most 0.75 of stage 1's (measured 0.70-0.74) and at most 0.62 of today's; end to end at most 0.68 of today's, AVX2 and SCALAR (measured 0.602-0.649 and 0.613-0.670); the GCC/Clang Linux build measured too | the +376 bytes of read-only data | 1 day |
| 3 | `doc/bench.md` section, README "How fast" row, `src/enc_t1.cpp` header comment (what departs from OpenJPEG's text: the operations, not the results) | review | wording | 0.5 day |
| later | Rust port of stages 1-2 by the rust-transcriber; decoder's use of the packed table, benched; a passes study (section 4) if wanted | their own gates | go-ahead | - |

## 8. Decisions for the owner

1. **Where `mq_shifts` lives.** (a, recommended) Move it from `t1.cpp` to `t1.h` and use it in both coders:
   one helper, the scalar build's object check already satisfied; it touches the decoder's file without
   changing its code. (b) A copy in `enc_t1.cpp`: the decoder's file untouched, two copies.
2. **The packed table.** (a, recommended) A new encoder table of 376 bytes built at compile time from
   `kMqcStates`: the decoder untouched; +376 bytes of read-only data until the decoder follow-up decides.
   (b) Switch the decoder to it now: no added bytes, but a decoder change outside this scope, to be benched.
3. **Stage 2 as one step.** (a, recommended) Packed index and select together: the index alone is within noise
   (f10: 0.98, f11 against f9: no gain) and the select pays only with it; splitting would give a step that
   fails the "faster" gate. (b) Two commits, the first one accepted at noise.
4. **The exhaustive test in the suite.** (a, recommended) In `ctest`, against the library's coder, with the
   low words and previous bytes cut to the ones that change a path (a few seconds); the full 1.8 billion
   (20 s) nightly. (b) A bench tool only, the stream tests in the suite.
5. **Stage 3 (the passes).** (a, recommended) Not now: stages 1-2 meet the target; a separate prototype study
   of the refinement-state registers and mask scans later, with its own gate. (b) Study it now, before
   stage 1 is written.

## 9. Risks, and what was not verified

- Code layout: the kernel moves 2 % between builds of the same operations; the gates above are 10-15 points
  wide, so no step depends on layout, but final numbers must be measured on the library build, interleaved.
- Compilers: measured on MSVC 14.51 only. The select must become `cmov`/`csel`; GCC and Clang may turn the
  ternaries back into branches (`-O2` usually keeps `cmov` for this pattern; not checked). Stage 2's gate
  must be measured on the GCC/Clang Linux build too, and the code may need a branch-free formulation
  (mask arithmetic) if a compiler branches.
- Hardware counters were not read (no PMU access); branch and memory counts are counted from the algorithm
  and the measured frequencies, not measured.
- The study texture is synthetic (gradients, edges, noise); the corpus images and real viewer uploads were
  not timed (their bytes are pinned by `encode_unit`).
- The clamp at the output limit is unreachable with 8-bit samples; its exactness rests on the short-buffer
  tests and the exhaustive single-decision test, not on an end-to-end case.
- Machine load: not quiet; ratios are interleaved and spreads under 2 %, but absolute cycles may be a few
  percent high.

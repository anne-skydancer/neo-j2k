<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026, Anne Skydancer (neo-j2k) -->

# EBCOT-NG Tier-1: study and plan (2026-11-22)

Scope (owner): the NG program's EBCOT-NG brief as an alternative to the staged MQ plans: a redesign of the
three Tier-1 passes (encoder and decoder) around bit-packed state, exact (encoder byte-identical, decoder
coefficient-identical on every valid stream, safe on malformed input). Gate of every step as in
`doc/studies/mq-encoder-redesign-decisions.md`: **exact**, **faster** beyond the noise band (about 3 %), **leaner**
(less work per symbol, fewer branches, less state and memory traffic; not fewer lines).

Machine and method as in `doc/studies/enc_t1_study.md` and `doc/studies/mqng_study.md`: Ryzen 9 9950X3D, Windows 11, MSVC 14.51
with the library's flags (`/O2 /arch:AVX2 /fp:precise`, no exceptions, `/guard:cf`, no LTCG), also the SCALAR
build (baseline x86-64, `bsr`); one thread pinned to CPU 7, high priority, TSC cycles, interleaved rounds with
the order rotating, the median of per-round ratios (11 rounds, 9 at 1,024²; SCALAR 9 / 7). Not a quiet machine.
Same inputs: the encoder study's dumps (the viewer's upload encode of the synthetic texture, three
components, 256², 512², 1,024², lossy and lossless) and the decoder study's dumps of the same codestreams
plus 35,390 code-blocks of 109 corpus and conformance files with every mode switch. Nothing was measured on
other hardware. Labels: **[m]** measured here, **[r]** read in the code (file:line at `bee41d4`), **[i]**
inferred.

## 0. Summary

- Every prototype composed with the MQ stages is exact: all 26 exact encoder variants on every code-block
  of the six configurations (bytes, and each pass's rate, length, termination and distortion bit for bit),
  and all 20 exact decoder variants on the six configurations and on all 35,390 corpus blocks (styles 00,
  01, 04, 05, 0a, 10, 12, 19, 20, 28, 2f, 34, 3f), AVX2 and SCALAR builds. [m]
- **Where the modelling time goes after the MQ stages [m]:**
  - Encoder: the modelling is 0.64-0.68 of the stage-2 Tier-1 kernel. Its parts:
    - preprocessing 5-7 %;
    - context formation 5-6 %;
    - nmsedec 2.5-4 %;
    - flag updates about 3 %;
    - the remaining 46-50 % is scanning and control flow, about 0.72 last-outcome mispredicts a decision.
  - Decoder: the modelling is 0.44-0.54 of the stage-1+2 Tier-1. Context formation, coefficient writes and
    flag updates are 1-3 % each; the rest is control flow.
- **What a bit-packed state changes (contexts, updates, stores, nmsedec) is at most about 13 % of the
  encoder's Tier-1 and 7 % of the decoder's, even if it were free.** OpenJPEG's column word, which neo-j2k already
  has, is a packed stripe-column state with shift-and-LUT contexts. A new state layout cannot pay for
  itself as a program. [m + i]
- **What pays is control flow, and only in the encoder.** The encoder knows every bit, so three changes
  to the code that drives the passes take 13.5-17 % off the stage-2 kernel on every configuration
  (`ehyb` 0.831-0.865 of f5, AVX2). With no intrinsics it is 0.849-0.880 (`ehybs`, AVX2 and SCALAR builds). The three
  changes:
  - branch-free preprocessing;
  - a mask-driven cleanup pass;
  - the refinement pass as a branch-free pair list, then one MQ loop.

  The decoder must branch on each decoded bit. Its best change is the mask-driven refinement pass,
  0.948-0.972 of wagg, about 3-4 % against the same-code control: at the gate's edge. [m]
- The two estimates on record:
  - **refinement contexts in registers up to 10 %: does not hold.** It is 4-5 % slower in the encoder and
    0-6 % slower in the decoder;
  - **4-bit sample masks 5-15 %: holds only at the low end and only for some passes.** Cleanup (encoder)
    3.8-4.5 %, refinement (decoder) 2.8-5.2 %; the significance pass is 3-4 % slower. [m]
- **Stop-gate verdict: stop EBCOT-NG as a program; keep three encoder steps after the MQ stages, and one
  optional decoder step.** The estimated whole one-thread encode is 0.865-0.879 of stage 2's, or
  0.52-0.56 of today's.

## 1. Measure first

### Tools (`bench/ebcotng_study`, standalone, uncommitted; `src/` and the other studies only read)

- `CMakeLists.txt` builds the library from `src/` (`lib_head`). It also builds the two bases from the
  earlier studies' own sources, read only:
  - the encoder's f5, stage 2: `bench/t1_study/proto/enc_t1_fused_body.inc`;
  - the decoder's ws (stage 1) and wagg (stages 1+2): `bench/mqng_study/proto/variant.cpp`, with its
    `make_body.py` writing into this build's `gen/`.

  It then builds this study's prototypes, one object per variant (the switch list is in the file).
- `enc_ng.inc` is the encoder rewritten cleanly on the stage-2 MQ coder: OpenJPEG's passes with the same
  operations in the same order, the coder in a local for each pass. With every switch off it is the
  control `e0`; each switch changes one thing (header of the file):
  - ablations: `E_MODEL` (MQ replaced by a sink), `E_NO_NMSE`, `E_NO_CTX`, `E_DUP_UPD` (flag update twice);
  - `E_COUNT`, `E_PASSCLK`;
  - the designs: `E_MAGREG`, `E_REF_MASK`, `E_SPP_MASK`, `E_CLN_MASK`, `E_SIMD_PRE`, `E_PRE_BRFREE`,
    `E_SPLIT` (a mask of passes), `E_SPP_SKIP`, `E_BRFREE`.
- `dec_ng.inc` is the decoder rewritten the same way on MQ-NG stages 1+2: word slots, select decision, AGG
  runs, and every mode switch (BYPASS, RESET, RESTART/TERMALL, VSC, SEGSYM). Control `d0`, plus:
  - `D_MODEL` (bits replayed from a recording) and `D_REC`;
  - ablations `D_NO_CTX`, `D_NO_STORE`, `D_DUP_UPD`;
  - `D_MAGREG`, `D_REF_MASK`, `D_REF_LIST`, `D_SPP_MASK`, `D_CLN_MASK`, `D_NOAGG`, `D_COUNT`, `D_PASSCLK`.
- `replay.cpp` (`ebc_replay enc --dump F.bin` and `ebc_replay dec --blk F.blk --check-dir D`) runs the
  identity checks against the library, the tallies, the pass clocks and the interleaved timing.
  `tools/tables.py` prints the tables below from its output.

Build and run: `cmake -S bench/ebcotng_study -B <dir> -G "Visual Studio 18 2026" -A x64` (add
`-DSTUDY_SCALAR=ON`), `cmake --build <dir> --config Release`, then `ebc_replay enc --dump <t1_study dump>`
and `ebc_replay dec --blk <mqng dump> --check-dir <mqng dump>/files`.

Controls [m]:
- Encoder: `e0` is 1.001-1.004 of f5 and `e0copy` 0.994-1.002 (AVX2); SCALAR 0.976-0.990.
- Decoder: `d0` is 0.987-1.003 of wagg and `d0copy` 0.976-1.000.

So a rewrite with the same operations moves by up to 2.4 %. As before, a step must gain at least 3 % on
every configuration.

### Encoder, on the stage-2 kernel (f5) [m]

| | 256² y | 256² l | 512² y | 512² l | 1,024² y | 1,024² l |
|---|---|---|---|---|---|---|
| stage-2 kernel, cycles a sample | 169 | 190 | 198 | 217 | 202 | 220 |
| stage-2 kernel, cycles a decision | 33.7 | 32.3 | 32.4 | 31.3 | 32.3 | 31.3 |
| modelling alone (MQ a sink), of the kernel | 0.678 | 0.658 | 0.666 | 0.649 | 0.662 | 0.643 |
| modelling alone, cycles a decision | 22.8 | 21.3 | 21.6 | 20.3 | 21.4 | 20.1 |
| time sig / ref / cln / preprocessing % | 43/25/24/7 | 41/30/23/6 | 41/29/25/6 | 39/32/24/5 | 41/29/25/6 | 39/32/24/5 |
| preprocessing (sign-magnitude, maximum), cycles a sample | 11.8 | 11.2 | 11.9 | 11.3 | 12.0 | 11.4 |
| ablation: nmsedec, of the kernel | 2.5 % | 3.6 % | 3.4 % | 4.1 % | 3.7 % | 4.2 % |
| ablation: every context a constant, of the kernel | 5.2 % | 5.4 % | 5.5 % | 5.5 % | 5.8 % | 5.4 % |
| flag update done twice (full kernel), added | 2.6 % | 3.1 % | 2.6 % | 3.0 % | 2.7 % | 3.0 % |

What the numbers say:
- The MQ coder inside the fused stage-2 kernel accounts for about 11 cycles a decision (kernel minus
  modelling). The modelling is 20-23 cycles a decision, two thirds of Tier-1.
- Of the modelling, the parts a state layout touches are small:
  - contexts 5-6 %, updates about 3 %, nmsedec 2.5-4 %;
  - preprocessing 5-7 %. That is a loop over the samples before the passes, branching on each sample's
    sign, 11-12 cycles a sample.
- The remaining 46-50 % of the kernel is the scan and its control flow.
- The model-only copy run with the update doubled came out 1.1-1.7 % faster: a layout artefact, so
  ablations have about ±2 % uncertainty.

Tallies [m], encoder (first two lines 256² l, 1,024² y, 1,024² l; the last line the range over all six):
- 0.87-0.96 newly significant samples a sample.
- 0.61-0.72 flag read-modify-writes a decision, 0.66-0.74 context-table loads a decision, 1.03-1.19
  coefficient loads a decision.
- Per pass, one decision for each 1.6-1.9 samples tested in the significance pass, 1.5-1.7 in refinement and
  2.5-3.0 in cleanup.

Branches [m]: each data-dependent site of the passes, with a last-outcome predictor's misses (an upper bound
of a real predictor's; no PMU on this machine). Encoder 1,024² lossy / lossless; decoder 1,024² lossy, 512²
lossless:

| Site | enc miss % | enc misses a decision | dec miss % | dec misses a decision |
|---|---|---|---|---|
| sig pass: sample eligible | 38.5 / 34.8 | 0.248 / 0.232 | 44.2 / 34.8 | 0.198 / 0.233 |
| sig pass: the significance bit | 45.8 / 45.4 | 0.134 / 0.119 | 41.2 / 45.6 | 0.100 / 0.120 |
| ref pass: sample refined | 38.8 / 34.6 | 0.202 / 0.195 | 27.5 / 29.5 | 0.108 / 0.200 |
| ref pass: column has work | 12.8 / 11.0 | 0.033 / 0.028 | 3.3 / 2.2 | 0.011 / 0.005 |
| cleanup: run mode | 5.8 / 5.1 | 0.017 / 0.015 | 5.6 / 5.2 | 0.026 / 0.015 |
| cleanup: sample coded / its bit | 7.4 / 21.4 | 0.054 / 0.029 | 15.5 / 18.5 | 0.073 / 0.036 |
| **sum (all sites)** | | **0.73 / 0.72** | | **0.58 / 0.66** |

Two sites dominate: "is this sample in the pass" in the significance and refinement passes (35-44 %
misses) and the significance bit (41-47 %). In the decoder the bit is the format's; the membership tests are
not. In the encoder all three are known in advance.

### Decoder, on MQ-NG stages 1+2 (wagg) [m]

| | 256² y | 256² l | 512² y | 512² l | 1,024² y | 1,024² l |
|---|---|---|---|---|---|---|
| Tier-1, cycles a sample | 37 | 184 | 49 | 207 | 45 | 210 |
| Tier-1, cycles a decision | 22.1 | 31.3 | 24.8 | 29.9 | 23.4 | 29.8 |
| modelling alone (bits replayed), of Tier-1 | 0.436 | 0.529 | 0.544 | 0.527 | 0.536 | 0.524 |
| time sig / ref / cln % | 38/9/52 | 50/32/18 | 52/17/30 | 47/35/18 | 49/18/33 | 47/35/18 |
| ablation: contexts constant | 1.2 % | 2.4 % | 1.5 % | 2.2 % | 1.4 % | 2.0 % |
| ablation: no coefficient writes | 2.3 % | 2.8 % | 1.7 % | 3.2 % | 1.7 % | 3.1 % |
| flag update done twice (full Tier-1), added | 0.0 % | -0.4 % | -1.2 % | 0.3 % | -1.1 % | 0.9 % |

### Stop gate for this program

EBCOT-NG continues as a program only if both hold:
1. After the MQ stages, the modelling is at least a third of Tier-1.
2. Either what its state redesign changes is at least 10 % of Tier-1, or a prototype takes at least 10 % off
   the stage-2 Tier-1 on every configuration of that direction.

**Verdict:**
- (1) holds on both sides (0.64-0.68 and 0.44-0.54).
- (2) fails for the state: contexts plus updates plus stores (plus nmsedec in the encoder) are 10-13 % of
  the encoder's Tier-1 and 3-7 % of the decoder's. Those are the costs of today's representation, so a
  better one saves only part of them.
- The encoder passes (2) through control flow (section 2), not through the state. The decoder fails it.

**So the program stops; three encoder steps and one optional decoder step remain** (section 6).

## 2. Prototypes, composed with the MQ stages [m]

Every number is the variant's Tier-1 time against the stage-2 base: f5 for the encoder, wagg for the
decoder. The base's own ratio to today's library is in the first row.

Encoder, AVX2 (against f5):

| Variant (what changes) | 256² y | 256² l | 512² y | 512² l | 1,024² y | 1,024² l |
|---|---|---|---|---|---|---|
| f5 against today's library (head) | 0.594 | 0.575 | 0.596 | 0.580 | 0.596 | 0.582 |
| e0 control (rewrite, same operations) | 1.002 | 1.001 | 1.004 | 1.003 | 1.002 | 1.001 |
| emagreg: magnitude states in registers (state words, selects) | 1.044 | 1.049 | 1.045 | 1.047 | 1.046 | 1.043 |
| erefmask: refined samples as a 4-bit mask, tzcnt | 0.956 | 0.983 | 0.979 | 0.998 | 0.981 | 0.998 |
| esppmask: eligible samples as a mask, recomputed | 1.030 | 1.041 | 1.033 | 1.040 | 1.029 | 1.038 |
| **eclnmask**: cleanup non-run columns as a fixed mask | 0.961 | 0.961 | 0.962 | 0.955 | 0.961 | 0.955 |
| **esimdpre**: preprocessing with AVX2 | 0.929 | 0.941 | 0.945 | 0.950 | 0.946 | 0.952 |
| **eprebf**: preprocessing branch-free, scalar code | 0.952 | 0.958 | 0.961 | 0.965 | 0.959 | 0.966 |
| esplit: all three passes as branch-free pairs, then one MQ loop | 1.043 | 1.096 | 1.097 | 1.144 | 1.100 | 1.145 |
| **esplitref**: the refinement pass alone split that way | 0.940 | 0.950 | 0.951 | 0.960 | 0.953 | 0.960 |
| ebest: erefmask + eclnmask + esimdpre | 0.849 | 0.881 | 0.878 | 0.902 | 0.880 | 0.902 |
| **ehyb**: esplitref + eclnmask + esimdpre | 0.831 | 0.851 | 0.849 | 0.865 | 0.853 | 0.863 |
| ehyb3: ehyb + significance split (columns with no eligible row skipped) | 0.801 | 0.835 | 0.838 | 0.865 | 0.841 | 0.866 |
| **ehybs**: ehyb with eprebf (no intrinsics) | 0.849 | 0.864 | 0.863 | 0.874 | 0.864 | 0.875 |
| ehyb, against today's library | 0.494 | 0.491 | 0.506 | 0.501 | 0.508 | 0.502 |

Encoder, SCALAR build (against its own f5; 256² and 1,024²):

| Variant | 256² y | 256² l | 1,024² y | 1,024² l |
|---|---|---|---|---|
| e0 / e0copy | 0.987 / 0.982 | 0.982 / 0.976 | 0.990 / 0.989 | 0.987 / 0.986 |
| eclnmask | 0.955 | 0.946 | 0.953 | 0.948 |
| esplitref | 0.931 | 0.939 | 0.946 | 0.956 |
| eprebf | 0.923 | 0.929 | 0.942 | 0.945 |
| **ehybs** | 0.850 | 0.864 | 0.867 | 0.880 |
| ehyb3s (ehyb3 with eprebf) | 0.818 | 0.845 | 0.858 | 0.881 |

Decoder, AVX2 (against wagg; SCALAR in the second table):

| Variant | 256² y | 256² l | 512² y | 512² l | 1,024² y | 1,024² l |
|---|---|---|---|---|---|---|
| wagg against today's library | 0.857 | 0.725 | 0.792 | 0.718 | 0.800 | 0.721 |
| d0 control / d0copy | 0.996 / 0.976 | 0.996 / 0.992 | 0.987 / 0.978 | 1.003 / 1.000 | 0.987 / 0.985 | 1.000 / 1.000 |
| dmagreg: magnitude words in registers | 1.002 | 1.056 | 1.013 | 1.053 | 1.014 | 1.050 |
| **drefmask**: refined samples as a mask | 0.969 | 0.956 | 0.948 | 0.972 | 0.953 | 0.972 |
| dlist: refined samples listed branch-free, then decoded | 0.985 | 0.956 | 0.959 | 0.972 | 0.961 | 0.968 |
| dsppmask | 1.009 | 1.015 | 0.989 | 1.021 | 0.990 | 1.018 |
| dclnmask (on top of the AGG run) | 1.011 | 0.995 | 0.993 | 1.002 | 0.994 | 1.000 |
| dmasks: all three masks | 1.018 | 0.981 | 0.961 | 0.993 | 0.969 | 0.992 |

| Decoder, SCALAR | 256² y | 256² l | 1,024² y | 1,024² l |
|---|---|---|---|---|
| d0 / d0copy | 0.976 / 0.961 | 0.988 / 0.983 | 0.977 / 0.975 | 0.997 / 0.999 |
| drefmask | 0.949 | 0.950 | 0.946 | 0.967 |
| dlist | 0.949 | 0.952 | 0.947 | 0.971 |

What was learnt:

- **The encoder knows every bit, so membership and the bit can stop being branches. Whether that pays
  depends on the pass.** Each split pass writes its (context, bit) pairs with no data-dependent branch:
  every pair is stored and the count advances by its "coded" bit. The flags are updated by masks; the
  column word is held in a register and the neighbours' words are updated from accumulators at the
  column's end. Measured per pass with the clock (1,024² lossless):
  - Refinement: from 222 to 197 Mcycles. Its membership (significant and not visited) and its contexts
    (neighbour significance, own μ) cannot change during the pass, so the list is cheap: about 8 operations
    a row and no sign context.
  - Significance: about even (270 to 287).
  - Cleanup: much slower (166 to 284), because every row of a visited column pays for the sign context,
    three table loads and eight accumulators, whether or not it is coded.

  So the whole-block split loses (1.04-1.15, as the encoder study's buffer did) and the refinement split
  wins (0.94-0.96).
- **The cleanup pass's non-run columns have a fixed candidate set**: not significant and not visited at
  the column's start (a row becomes significant only by being coded itself). Iterating it with `tzcnt`
  replaces the library's run-time-row loop: per row a flag reload, a test, a PI clear and run-time shifts.
  The gain is 3.8-4.5 % (AVX2) and 4.5-5.4 % (SCALAR).
- **Preprocessing:** the loop that turns each coefficient into sign-magnitude and finds the maximum branches
  on every sign (11-12 cycles a sample on these signals). Branch-free scalar code gives 3.4-4.8 % (AVX2
  build) and 5.5-7.7 % (SCALAR); AVX2 intrinsics give 4.8-7.1 %. INT32_MIN has to map to 0x7FFFFFFF with the
  sign bit, as today (`enc_t1.cpp:664-667`).
- **The significance pass resists both designs.** As a mask it recomputes eligibility after every new
  significant sample, and the row becomes a run-time value in the update and sign-index paths: 3-4 %
  slower (2 % with branch-free north/south updates, 256² l). As a split it is even. Skipping the columns
  with no eligible row at their start is exact and adds 0-3.6 % (`ehyb3` against `ehyb`), below the gate
  on lossless.
- **State in registers does not help.** The magnitude contexts' states held in registers with selects
  (state words in the encoder too) are 4-5 % slower in the encoder and up to 6 % in the decoder. The store
  to load path it removes is not the bound: stage 2's decision is latency-bound on the interval registers
  and the table load, and the selects add work to that chain. The estimate on record (up to 10 %) does not
  hold. [m; the cause is inferred]
- **Decoder:** only the refinement pass's membership is free of the decoded bits. As a mask (`drefmask`)
  or a list (`dlist`) it saves 2.8-5.2 % against wagg, and 2.7-4.0 % against the same-code control
  `d0`: just above the noise band on most configurations, and not clearly on 256² lossy (d0copy itself
  moved 2.4 % there). The cleanup mask adds nothing on top of MQ-NG's AGG runs, and the significance mask
  is slower, as in the encoder.
- **Skipping empty stripes or columns:** the library already skips zero column words (significance,
  refinement) and the run mode covers empty columns in cleanup. `doc/bench.md` stage 4 found an AVX2
  eight-column skip lost (1.004-1.021). Nothing here re-opens it.
- **SIMD across blocks:** still closed. After the MQ stages the modelling dominates, and it is control
  flow that differs from block to block, the lanes prototype's argument (`doc/bench.md` "lane-per-code-block").

## 3. Current state, what the steps touch [r]

- Encoder: `src/enc_t1.cpp`.
  - `enc_t1_encode` (604-759): flags set-up 624-652, preprocessing 656-675, the pass loop 693-727, rates
    and the 0xFF rule 735-756.
  - Refinement pass 392-453, cleanup 456-553, significance 328-390.
  - `EncT1` (240-247): about 39.7 KB, one per concurrent job.
  - The passes and contexts are OpenJPEG 2.5.4's (column words of `src/t1_flags.h`: 18 neighbourhood σ
    bits, 6 χ bits, μ, 4 π bits for four samples).
- Callers and threads: `t1_job` (`src/encode.cpp:582-656`, data copied in stripe-interleaved order
  615-650). It runs in batches of at most the executor's width, one `EncT1` a concurrent job
  (`encode.cpp:740-808`), serially below 2^16 samples (701), committed on the calling thread in block
  order (790-806). `EncT1` is never shared and the tables are `constexpr`, so the steps add no shared state.
- Decoder: `src/t1.cpp`.
  - Refinement pass `opj_t1_dec_refpass_mqc_internal` 778-818 (macro) and the partial stripe 811-817.
  - Callers: `decode_store` / `decode_blocks` / `decode_run` (`src/tile.cpp:497-555`), runs of blocks per
    executor part with their own `T1` (`t1_jobs` 560-575). Tiny blocks are already grouped into runs there.
- Rust port: `neoj2k-rust/neo-j2k/src/t1/passes.rs:8-15` uses the same column-word layout. The decoder
  step carries over as it is.
- Tests that pin the output:
  - `tests/unit/encode_unit.cpp` (oracle bytes);
  - `fuzz/fuzz_encode.cpp` and `fuzz_roundtrip.cpp`;
  - the decoder's oracle hashes;
  - the MQ plans' planned stage 0 (frozen copies of `enc_t1.cpp` and `t1.cpp`, `fuzz_t1`).

## 4. Design of what remains (applied after the MQ stages)

1. **E1, branch-free preprocessing** (`enc_t1.cpp:656-675`), as `eprebf`:
   `neg = 0 - (x >> 31); y = x == 0x80000000 ? 0x80000001 : x; a = (y ^ neg) - neg; max = max(max, a);
   out = a | (x & 0x80000000)`. Same function, no branch. An AVX2 form (`esimdpre`) gains 1-2 % more, but
   it is a second code path (decision 2).
2. **E2, mask-driven cleanup outside run mode** (`enc_clnpass`, the `agg == 0` columns of full stripes).
   For each column:
   - `cand = ~((f >> 4) | (f >> 21)) & 0x249` (row r at bit 3r);
   - for each set bit: the zero-coding decision on the current word, then the sign decision and the
     immediate update (the library's `update_flags`);
   - then `f &= ~πall`.

   Run-mode columns, the partial stripe and the "all significant and visited" case are as today (the last
   gives `cand = 0`).
3. **E3, refinement pass as a pair list, then one MQ loop** (`enc_refpass`). For each column of a full
   stripe:
   - `m = f & ~(f >> 17) & 0x2490` (σ and not π, at bit 4 + 3r);
   - when it is non-zero, four rows are each stored unconditionally: a byte `ctx << 1 | bit` at `n`, then
     `n += refined`. nmsedec is added masked, from the table chosen once per pass;
   - `f |= m << 16` (μ).

   Then `for (i < n) encode(p >> 1, p & 1)`, and the partial stripe as today. `EncT1` gains a 4 KB pair
   buffer (a pass refines each sample at most once). The pass end, its `numbytes()`, the distortion and the
   rates are unchanged.
4. **D1 (optional), mask-driven refinement in the decoder** (`t1.cpp:778-818`). Per column word, the same
   `m`, iterated with `tzcnt`; the magnitude context from the column word; `f |= m << 16` once; the
   coefficient update as today. No scratch. The list form (`dlist`) times the same but needs 8 KB, so
   the mask is the one to build.

Not designed, with the measurements above:
- a new bit-packed state layout (bounded at 3-13 % even if free);
- one engine for both directions (the winning steps differ, because only the encoder knows its bits);
- state in registers (slower);
- significance-pass masks or split (slower, or below the gate);
- SIMD lanes across blocks (closed).

Threads and failure behaviour are unchanged. Each step is local to one code-block's pass, so no new shared
state, no new error path, and the same `NJ2K_E_LIMIT` at the same block (E3 codes the same decisions, so
the clamp falls on the same byte).

## 5. Costs [m on Tier-1; end to end estimated]

| | 256² y | 256² l | 512² y | 512² l | 1,024² y | 1,024² l |
|---|---|---|---|---|---|---|
| Encoder kernel: ehyb / stage 2 (AVX2) | 0.831 | 0.851 | 0.849 | 0.865 | 0.853 | 0.863 |
| Encoder kernel: ehyb / today's library | 0.494 | 0.491 | 0.506 | 0.501 | 0.508 | 0.502 |
| Whole one-thread encode, est.: / stage 2 | 0.867 | 0.865 | 0.876 | 0.876 | 0.879 | 0.875 |
| Whole one-thread encode, est.: / today | 0.563 | 0.521 | 0.560 | 0.527 | 0.564 | 0.530 |
| Whole encode with ehybs (no intrinsics), est.: / stage 2 | 0.881 | 0.877 | 0.887 | 0.885 | 0.888 | 0.886 |
| Decoder Tier-1: drefmask / wagg | 0.969 | 0.956 | 0.948 | 0.972 | 0.953 | 0.972 |
| Whole decode, est.: stages 1+2 → + D1 | 0.867 → 0.842 | 0.728 → 0.696 | 0.802 → 0.760 | 0.720 → 0.700 | 0.809 → 0.773 | 0.723 → 0.703 |

The end-to-end figures are stage 2's measured whole encode (`doc/studies/enc_t1_study.md` section 5) minus the
kernel time saved, and the decoder study's Tier-1 shares. At 4.292 GHz the 1,024² lossy upload encode
would go from about 180 ms (stage 2) to about 158 ms.

Memory:
- encoder +4 KB per `EncT1`, one per concurrent job (about 256 KB at 64 jobs);
- decoder none;
- read-only data none.

Leaner, per symbol:
- E1 removes one mispredictable branch a sample (about half of signs are negative).
- E2 replaces four flag reloads, tests and PI clears per column with one mask computation and one
  iteration per coded row.
- E3 removes the per-sample membership branch (35-39 % misses, about 0.2 misses a decision of the whole
  block) and the column's second test. It adds a byte store and a byte load per refinement decision.
- D1 removes the decoder's membership branch (27-30 % misses).

No GPU work.

## 6. Comparison with the staged MQ plans

| | Encoder | Decoder |
|---|---|---|
| MQ stages (decided / proposed) | kernel 0.573-0.596 of today | Tier-1 0.718-0.857 of today |
| EBCOT-NG steps on top (measured composed) | × 0.831-0.865 (E1-E3) | × 0.948-0.972 (D1) |
| Together | 0.49-0.51 of today's kernel | 0.69-0.83 of today's Tier-1 |
| Overlap | none: E1-E3 change the modelling, the MQ stages the coder | MQ-NG stage 2's AGG run is the cleanup's run-mode lever; D1 is the refinement pass's |
| Effort | MQ stages about 3 days; E1-E3 about 3 days with tests | MQ-NG about 4 days; D1 about 1 day |
| Risk | E3 adds a buffer and a second loop; exactness argued and proven by traces (section 7) | D1 at the gate's edge |

The gains stack: every prototype runs on the stage-2 coders, so the ratios are already relative to the
improved MQ. The EBCOT-NG steps were not measured on today's coder, so their gain before the MQ stages is
unknown; the MQ stages are larger, already planned, and come first.

**Plainly: as a program (new state layout, one engine for both directions) the alternative adds little.**
Its state levers are bounded at a few percent, and the decoder gains 3-5 % at best. What is worth having
is three targeted encoder steps worth 13.5-17 % of the stage-2 kernel (12-13 % of the whole encode), and
possibly the decoder's refinement mask.

Order:
1. MQ encoder stages 1-2 (decided).
2. MQ-NG decoder stage 1 (+2).
3. E1, E2, E3, one PR each.
4. D1, if the owner accepts a step at the gate's edge, after MQ-NG lands, re-measured on the library build.

## 7. Staged plan

| Stage | Scope | Gate (tests, performance) | Owner's check | Effort |
|---|---|---|---|---|
| T | Tests first, extending the MQ plans' stage 0. A test-only trace hook in the frozen `enc_t1.cpp` and the live one: (pass, context, bit) for every decision. Generated blocks: every w, h in 1..64 (heights not a multiple of 4), orient 0-3, 5/3 and 9/7 step sizes, numbps 1-25, sparse/dense/all-negative/alternating-sign data and INT32_MIN; the study's dumps as vectors. Comparison of each pass's decision trace, rate, len, term and distortion bit for bit, and the bytes, against the frozen copy. Decoder: the 35,390 corpus blocks with coefficient hashes and `fuzz_t1` (MQ-NG plan). Today's code passes. | Green on all presets (MSVC, clang-cl, GCC, Clang; AVX2, SCALAR; ASan/UBSan, TSan, fuzz) | that the frozen copies are today's files | 1 day |
| E1 | Branch-free preprocessing (`enc_t1.cpp:656-675`) | T, `encode_unit`, oracle hashes; kernel ≤ 0.97 of the stage-2 kernel on all six configurations, AVX2 and SCALAR (measured 0.952-0.966 / 0.923-0.945) | decision 2 | 0.25 day |
| E2 | Cleanup mask (`enc_clnpass`) | as E1; ≤ 0.97 of E1's kernel (measured 0.955-0.962 alone) | none | 0.5 day |
| E3 | Refinement pair list + MQ loop (`enc_refpass`, `EncT1` +4 KB) | as E1; ≤ 0.97 of E2's kernel (measured 0.940-0.960 alone); E1-E3 together ≤ 0.89 of stage 2 on every configuration, AVX2 and SCALAR (measured with scalar E1: 0.849-0.875 / 0.850-0.880); whole encode measured on the library build; GCC/Clang Linux measured | the 4 KB | 1 day |
| E4 | `doc/bench.md` section, `enc_t1.cpp` header (what departs from OpenJPEG's text: operations, not results) | review | wording | 0.25 day |
| D1 | Decoder refinement mask (`t1.cpp:778-818`), after MQ-NG | T; ≥ 3 % against MQ-NG on every configuration and build, else dropped (measured 2.8-5.2 % against wagg, 2.7-4.0 % against the control) | decision 4 | 1 day |
| later | Rust: D1 in `passes.rs`; E1-E3 in the Rust encoder when it is written; the vectors | bit-exact against the C++ vectors, its own benches | go-ahead | - |

Exactness proofs, beyond the differential and trace tests:
- **E2:** the cleanup candidate set of a non-run column is fixed at the column's start, because a row's σ
  changes only when it is coded itself. The contexts read the column word as earlier rows updated it,
  immediately.
- **E3:** during the refinement pass no σ or π bit changes, and the μ a context reads is the sample's own,
  set after its own decision. So the list built from the words equals the sequence the library visits, and
  μ can be set at once.
- **E1:** the same function, including INT32_MIN, checked exhaustively over the 2^32 inputs by a test (a
  few seconds).
- Malformed input concerns only the decoder. D1 reads only the flags the passes wrote, so its safety is
  today's plus MQ-NG's segment check.

## 8. What carries over to a fresh Rust port

- The steps themselves: E1 as safe scalar code (no intrinsics needed: `ehybs` keeps 12-15 % of the 13.5-17 %),
  E2 and D1 as `trailing_zeros` loops on the same column words as `passes.rs`, and E3 with `[u8; 4096]` and
  indices below 4096 by construction.
- The exactness arguments above, the vectors (`NJT1` encoder dumps, `MQNG` decoder dumps, the corpus
  blocks) and the trace comparison.
- What does not carry: MSVC's layout findings and every speed ratio; they have to be measured again in Rust
  against the C++.

## 9. Decisions for the owner

1. **The program.**
   - (a, recommended) No EBCOT-NG program: E1-E3 after the MQ encoder stages, D1 optional.
   - (b) The full redesign: new state layout and a shared engine. Its state levers are bounded at 3-13 %
     and the decoder gains 3-5 %.
2. **Preprocessing form.**
   - (a, recommended) Branch-free scalar: one code path, the SCALAR build's largest single gain
     (5.5-7.7 %), and as it is for Rust.
   - (b) AVX2 intrinsics with the scalar fallback: 1-2 % more on the AVX2 build, two paths.
3. **Encoder refinement step.**
   - (a, recommended) The pair list (E3): 4-6 %, +4 KB per job.
   - (b) The 4-bit mask: no buffer, but 0.2-4.4 %, failing the gate on lossless.
4. **Decoder refinement mask (D1).**
   - (a, recommended) Re-measure after MQ-NG lands and keep it only if it clears 3 % everywhere.
   - (b) Accept it now at 2.8-5.2 %.
   - (c) Drop it.
5. **Significance-pass split with the column skip (`ehyb3`).**
   - (a, recommended) Not adopted: 0-3.6 %, below the gate on lossless.
   - (b) Adopt it for the lossy gain (up to 3.6 % at 256²).
6. **Order.**
   - (a, recommended) MQ stages first, then E1-E3.
   - (b) E1-E3 first: not measured on today's coder.

## 10. Risks, and what was not verified

- Only MSVC was measured, AVX2 and SCALAR; clang-cl, GCC and Clang were not. The steps are masks, counts
  and `tzcnt` loops a compiler should not turn into branches, but the gates must be measured on Linux too.
- No PMU counters: branch misses are a last-outcome predictor's on the outcome streams, an upper bound;
  the cycle cost of mispredicts is inferred.
- Ablations carry about ±2 % layout noise; one came out with the wrong sign. The state-lever bound is a
  sum of ablations, so it is approximate.
- The inputs are the synthetic texture and the conformance corpus. The owner's 212 real textures are not on
  this machine; their sign and refinement statistics may move E1 and E3 by a few points.
- The end-to-end figures are estimates from the kernel and Tier-1 shares, not whole-library builds with
  the steps.
- D1 is at the noise band's edge; the 256² lossy set is small (its controls moved 2.4 %).
- Not built: a full stripe-bitmask state with shift-gathered contexts. The bound above says it cannot pay,
  but the bound is inferred from ablations of today's layout, not from such a prototype.

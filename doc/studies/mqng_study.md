<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026, Anne Skydancer (neo-j2k) -->

# MQ-NG: the MQ decoder and its Tier-1 coupling, study and plan (2026-11-22)

Scope (owner): the Part 1 MQ **decoder** (`src/t1.cpp`, OpenJPEG 2.5.4's port: MQ, raw/BYPASS, RESET, SEGSYM,
VSC, RESTART/ERTERM segments), its byte input and its coupling to the three passes. C++ first, in neo-j2k;
a fresh Rust version for `neoj2k-rust` later. Output decision for decision today's on every valid stream;
malformed streams safe and deterministic. "Firewall" means against obsolescence: stable parts with seams,
designs that keep paying on other CPUs, compilers and core counts. Gate of every step: **exact**, **faster**
beyond the noise band (about 3 %), **leaner** (less work per decision, fewer branches, less state).

Machine: Ryzen 9 9950X3D, Windows 11, MSVC 14.51 (`/O2 /arch:AVX2 /fp:precise`, no exceptions, `/guard:cf`, no
LTCG), also the SCALAR build (baseline x86-64, `bsr`) and clang-cl (VS toolset ClangCL); one thread pinned to
CPU 7, high priority, TSC cycles (4.292 GHz). Not a quiet machine, so every speed number is a median of
interleaved per-round ratios. Nothing measured on other hardware; statements about it are estimates.
Labels: **[m]** measured here, **[r]** read in the code (file:line at `bee41d4`), **[i]** inferred.

## 1. Measure first

### Tools (`bench/mqng_study`, standalone, uncommitted)

- `CMakeLists.txt`: builds the library from `src/` as `lib_head` (unchanged) and `lib_instr` (copies of
  `decode.cpp` and `tile.cpp` with a stage clock and a code-block dump inserted at configure time, anchors
  checked). `src/` is only read.
- `mqng_profile`: encodes the encoder study's texture (`bench/t1_study/driver.cpp`, the viewer's upload
  parameters, three components) at 256², 512², 1,024², lossy and lossless, decodes it through `lib_instr`
  (stage shares, median of 15 runs, 8 at 1,024²), and dumps every code-block as Tier-1 receives it
  (`blockio.h`). With `--files` it dumps the code-blocks of a file list (here the 68 files of
  `corpus/generated` and the 41 J2K files of the OpenJPEG conformance set: 35,390 blocks with every mode switch).
- `tools/make_body.py`: writes the decoder of `src/t1.cpp` (MQ region, passes, `t1_decode_cblk`) as a body
  that a variant includes in its own namespace, with seams for the MQ region, single pass macros, the AGG
  decision and a pass clock. `proto/variant.cpp` is compiled once per variant (19 objects).
- `mqng_replay`: every dumped block through the unchanged library's `t1_decode_cblk` and through every
  variant, coefficients and result compared; the counting variant's tallies; the MQ decoder alone and the
  passes alone on the recorded decisions; a pass clock; the variants timed in interleaved rounds.

Build and run: `cmake -S bench/mqng_study -B <dir> -G "Visual Studio 18 2026" -A x64` (add `-DSTUDY_SCALAR=ON`
or `-T ClangCL`), `cmake --build <dir> --config Release`, `mqng_profile --out D --files LIST`,
`mqng_replay --dir D --check-dir D/files --rounds 11`.

### Stage shares, one thread [m]

| Config | Bytes | Decode Mcycles | Tier-2 | **Tier-1** | Wavelet | Rest |
|---|---|---|---|---|---|---|
| 256² lossy | 24,385 | 8.84 | 0.8 % | **92.8 %** | 4.6 % | 1.7 % |
| 256² lossless | 129,529 | 50.13 | 0.1 % | **98.9 %** | 0.6 % | 0.3 % |
| 512² lossy | 98,295 | 42.86 | 0.4 % | **95.4 %** | 3.1 % | 1.1 % |
| 512² lossless | 595,252 | 228.95 | 0.0 % | **99.3 %** | 0.5 % | 0.2 % |
| 1,024² lossy | 393,225 | 172.10 | 0.3 % | **95.6 %** | 3.0 % | 1.1 % |
| 1,024² lossless | 2,398,977 | 926.68 | 0.0 % | **99.3 %** | 0.5 % | 0.2 % |

**Stop gate (stop or shrink if Tier-1 is under half of the decode, or if the MQ decoder is under a third of
Tier-1): passes by far.** Tier-1 is 93-99 % of the decode, and the MQ decoder 61-73 % of Tier-1 (below).

### Inside Tier-1 [m]

| | 256² y | 256² l | 512² y | 512² l | 1,024² y | 1,024² l |
|---|---|---|---|---|---|---|
| decisions a sample | 1.66 | 5.88 | 1.99 | 6.93 | 1.91 | 7.05 |
| Tier-1 cycles a decision, today | 25.6 | 43.2 | 31.3 | 41.8 | 29.3 | 41.4 |
| decisions sig / ref / cln % | 27 / 4 / 69 | 42 / 33 / 25 | 36 / 9 / 55 | 38 / 37 / 25 | 32 / 9 / 59 | 37 / 37 / 26 |
| pass time sig / ref / cln % | 43 / 10 / 47 | 49 / 35 / 16 | 54 / 19 / 27 | 46 / 39 / 15 | 52 / 20 / 29 | 46 / 39 / 15 |
| raw (BYPASS) decisions | 0 | 0 | 0 | 0 | 0 | 0 |
| **MQ alone (today's decoder), of Tier-1** | 0.73 | 0.63 | 0.61 | 0.64 | 0.62 | 0.64 |
| **passes alone (bits from the record), of Tier-1** | 0.38 | 0.39 | 0.44 | 0.38 | 0.44 | 0.39 |
| LPS interval (`c_hi < qe`) % | 19.9 | 37.8 | 21.7 | 37.0 | 20.2 | 36.6 |
| MPS without renormalisation % | 58.7 | 29.7 | 56.4 | 31.0 | 59.1 | 31.7 |
| renormalisations % (shifts each) | 41.3 (1.41) | 70.3 (1.27) | 43.6 (1.36) | 69.0 (1.26) | 40.9 (1.37) | 68.3 (1.26) |
| byte-ins a decision | 0.073 | 0.112 | 0.074 | 0.109 | 0.070 | 0.108 |
| AGG context share % | 26.3 | 7.2 | 31.5 | 9.5 | 34.5 | 10.2 |
| decisions in same-context MPS-no-renorm runs ≥ 2 % | 32.0 | 10.3 | 38.3 | 12.9 | 41.5 | 13.9 |
| of them AGG runs (the run-mode target) % | 19.4 | 5.5 | 28.9 | 8.0 | 31.9 | 8.8 |

The split is additive (MQ alone + passes alone = 1.02-1.11 of Tier-1): **today the MQ decoder is about two
thirds of Tier-1 and the modelling one third.** Branch behaviour, derived [m] (a last-outcome predictor's
misses per decision, an upper bound of a real predictor's): the LPS-interval branch 23-25 % lossy and 40-42 %
lossless, the renormalisation branch 30-35 %, the decoded bit 23-25 % / 40-42 %. So today's decision has
three near-random branches; the one on the decoded bit is the only one the passes need. The most used contexts
are the refinement contexts 16 and 15 (24 % and 12 % lossless) and AGG (26-35 % lossy). No PMU access (uProf
not installed): miss counts are derived from the outcome stream, not read from counters.

## 2. Baseline audit of byte input, termination and segments (cheap; no flaw needed)

The premise "the existing MQ decoder has a critical flaw" **does not hold here**: no reachable memory error,
UB or unbounded work was found. What was checked [r]:

1. **Reads stay inside the buffer (confirmed safe).** `T1::reserve` keeps 2 bytes beyond the gathered data
   (`t1.cpp:1114-1137`); `opj_mqc_init_dec_common` writes `0xFF 0xFF` at each segment's end (`t1.cpp:297-299`);
   `bytein` reads `bp[0]`, `bp[1]` and advances only past a non-marker (`t1.cpp:161-182`), the raw decoder
   likewise (`t1.cpp:133-158`), so `bp <= end` and the last read is `end[1]`.
2. **Segment lengths are not checked in Tier-1 (contract gap, unreachable today).** `t1_decode_cblk` never
   checks that the segments' lengths fit the gathered bytes (`t1.cpp:1205-1229`); if they did not, the sentinel
   write would land past the buffer. Tier-2 makes it hold by construction: every `seg->len += newlen` comes with a
   chunk of the same length (`t2.cpp:1044-1051`), the skip path changes no length (`t2.cpp:1115-1117`), new
   segments are zeroed (`t2.cpp:674`), and a cut keeps only completed segments (`t2.cpp:1034`). Label:
   **plausible only if Tier-2 changes; not reachable now.** MQ-NG checks it once a block.
3. **No length overflow (confirmed safe).** `cblk_len` is 32-bit (`t1.cpp:1192`), but a block's chunks lie
   in its tile's data, whose size is 32-bit (`tile.h:139-140`), so the sum cannot wrap.
4. **Work is bounded but not by the bytes (by design, as OpenJPEG).** A decision is O(1) (at most 15 shifts,
   two byte-ins); a block is at most about 3 decisions a sample a bit-plane over at most 30 bit-planes
   (`bitplanes_ok`, `t1.cpp:1088-1096`), about 340,000 decisions for 64×64 whatever its byte count: a 1-byte
   segment drives as much work as a full one (past the end the decoder reads the marker forever). This is the
   format's; the cap belongs to the image limits, not the MQ decoder. Not a defect.
5. **Dead state (confirmed).** `end_of_byte_stream_counter` is written (`t1.cpp:171`, `312`) and never read.
6. **The decoder writes into its input** (sentinel, then restore from `backup`, `t1.cpp:297`, `338`): safe,
   since the bytes are Tier-1's own copy (`t1.h:9-10`), but it ties the input to a mutable buffer.
7. **Threads (confirmed).** One `T1` per job (`tile.cpp:563-570`, `decode.cpp:222`, `1413`), the tables
   `constexpr`, `g_t1_trace` only in trace builds: no shared mutable state.

## 3. Prototypes against the gate [m]

Every variant was checked against the library on all 35,390 blocks of the mode-switch corpus (code-block
styles 00, 01, 04, 05, 0a, 10, 12, 19, 20, 28, 2f, 34, 3f: BYPASS, RESET, RESTART, VSC, ERTERM, SEGSYM and
combinations) and on the six study configurations, in all three builds: **0 blocks differ**. Ratios: Tier-1
time against the library, median of 11 interleaved rounds (7 for SCALAR and clang-cl). Noise: the library's
own text in another object (`copy`) gives 0.989-1.002, but 0.956-0.970 on the small 256² lossy set.

| Variant (what changes) | 256² y | 256² l | 512² y | 512² l | 1,024² y | 1,024² l |
|---|---|---|---|---|---|---|
| copy (noise control) | 0.958 | 0.990 | 0.989 | 0.997 | 0.997 | 1.000 |
| pb: states as byte indices into a packed table, branches | 0.980 | 0.946 | 0.967 | 0.944 | 0.974 | 0.948 |
| wb: the state word in the context slot, branches | 0.928 | 0.889 | 0.917 | 0.887 | 0.920 | 0.888 |
| ps: byte indices, **decision by selects, renormalisation always run** | 0.926 | 0.742 | 0.910 | 0.746 | 0.940 | 0.751 |
| **ws: state words + selects (stage 1)** | **0.869** | **0.721** | **0.838** | **0.715** | **0.857** | **0.718** |
| mix: branches where the bit steers the pass, selects in refinement | 0.962 | 0.844 | 0.931 | 0.833 | 0.939 | 0.836 |
| wmixs: wmix + selects for the sign | 0.901 | 0.789 | 0.871 | 0.777 | 0.871 | 0.776 |
| err: ps + a per-decision error check (idea 6's alternative) | 0.922 | 0.738 | 0.896 | 0.739 | 0.923 | 0.743 |
| **wagg: ws + AGG MPS runs at once (idea 2, stage 2)** | **0.849** | **0.721** | **0.786** | **0.712** | **0.790** | **0.712** |
| wspec: ws + sign context and slot loaded early (idea 3) | 0.857 | 0.709 | 0.831 | 0.707 | 0.849 | 0.710 |
| wchk: ws, input bounds-checked and read-only, no sentinel (idea 1 reversed) | 0.877 | 0.730 | 0.848 | 0.724 | 0.865 | 0.727 |
| wall: wagg + wspec + wchk | 0.843 | 0.707 | 0.778 | 0.700 | 0.783 | 0.701 |
| wfn: wagg with the decision an inline function (the seam), 2nd run | 0.863 | 0.727 | 0.789 | 0.718 | 0.794 | 0.718 |

Other builds (same variants, each against its own build of the library):

| | 256² y | 256² l | 512² y | 512² l | 1,024² y | 1,024² l |
|---|---|---|---|---|---|---|
| SCALAR ws / wagg | 0.874 / 0.856 | 0.723 / 0.725 | 0.845 / 0.791 | 0.720 / 0.715 | 0.863 / 0.798 | 0.720 / 0.714 |
| clang-cl ws / wagg | 0.876 / 0.831 | 0.768 / 0.754 | 0.880 / 0.800 | 0.769 / 0.749 | 0.895 / 0.800 | 0.774 / 0.751 |

MQ decoder alone on the recorded decisions (fraction of the library's Tier-1): today 0.61-0.73; byte-index
selects 0.35-0.57; so the select decoder codes a lossless decision in about 15 cycles against today's 27.

What was learnt:
- **Selects win in the decoder too, once the state is an index or word and the renormalisation needs no loop.**
  `doc/bench.md` stage 4 found selects 3-5 % slower; with the state word and the always-run renormalisation
  (shift 0 when bit 15 is set, a branch only when a byte must come in: 0.07-0.11 a decision) they take 28 %
  off lossless and 13-16 % off lossy Tier-1, on all three builds. Inferred cause of the old result: pointer
  states and the renormalisation's loop branch stayed (the encoder study saw the same: f7 against f5).
- **The decoded bit's own branch is the one to keep.** Putting branches back where the bit steers the pass
  (mix, wmix) is 4-14 % slower than selects everywhere; the remaining branch on the bit is the format's.
- **The state word in the slot beats the byte index** by 3-9 % (one dependent load fewer before `qe`), with
  branches too (wb 0.89 against pb 0.95). The Rust port already holds its contexts this way
  (`neoj2k-rust/neo-j2k/src/t1/mq.rs:16-28`, same bit layout).
- **Idea 1 (sentinel padding):** today's decoder already has no bounds check on the hot path. Checking every
  byte read instead and leaving the input read-only (wchk) costs 0.9-1.3 % (MSVC), -0.6 to +0.8 % (SCALAR),
  0.9-2.1 % (clang-cl): the sentinel is worth about 1 %, the check is nearly free. Byte-ins are rare.
- **Idea 2 (MPS runs):** general same-context runs are short (most runs are 1), but the cleanup pass's AGG
  runs over empty columns are long in lossy code-blocks: taken at once (step j valid while
  `j*qe <= min(a - 0x8000, c_hi)`, no division) they give 6-11 % on 512² and 1,024² lossy over stage 1
  (2-5 % at 256²) and are neutral on lossless (0-3 % on clang-cl).
- **Idea 3 (speculation):** the sign's context formed and its slot loaded before the significance decision:
  0.8-1.7 %, below the band, and depends on out-of-order scheduling: rejected.
- **Idea 4 (fused table-driven step):** its measurable part is the state word (the slot holds `qe`, MPS and
  both successors; a change is one table load): kept in stage 1. A table over (state × neighbourhood) would be
  512 × 94 × 4 bytes = 192 KB, out of L1/L2 on most CPUs; not built [i]. The flag updates are already six ORs
  at fixed offsets.
- **Idea 5 (passes in parallel under RESTART/ERTERM): not possible.** Each pass's contexts and which samples
  it visits come from the significance flags the earlier passes wrote (`opj_t1_update_flags_macro`,
  `t1.cpp:414-440`); RESTART only re-initialises the arithmetic registers [r]. Census: 8.4 % of the 35,390
  test-corpus blocks are TERMALL, all in conformance and generated test files; the viewer's encoder writes no
  mode switches (`doc/studies/enc_t1_study.md` section 3).
- **Idea 6 (one sticky error):** a per-decision check costs nothing measurable (err against ps: -1.8 to -0.4 %
  MSVC, 0 to +0.8 % SCALAR, +0.2-1.0 % clang-cl), but the decoder has nothing to report per decision: a sticky "exhausted" bit set in
  the marker path (already a branch of its own) and read once a segment is enough.
- **The seam is free.** The decision as a `__forceinline` function instead of a macro (wfn) is within 0.6-1.8 %
  of the macro (layout band).
- **SIMD lanes across code-blocks stay out.** After stage 1 the MQ decoder alone is 0.35-0.57 of today's Tier-1
  and the passes 0.38-0.44: it no longer dominates. The 8-lane AVX2 prototype was 1.41x a branchy index decoder
  (`doc/bench.md:327-329`, other corpus); the select decoder alone is already 1.58-1.66x that kind of decoder
  on lossless and 1.13-1.21x on lossy here [m: pb-like M1 against M2], so lanes would lose before modelling is
  added [i].
- Not explained: the word-select decoder alone is slower than the byte-select one alone (0.52-0.67 against
  0.35-0.57) though in the whole Tier-1 the word form wins; not investigated (likely code generation of the
  replay loop).

## 4. Design

Five stable parts, each swappable without touching the others:

1. **Tables** (`src/mq_tables.h`, `constexpr`, built from `kMqcStates`, the standard's Table C.2): `kMqWords[94]`
   of 4 bytes: `qe` (bits 0-15), MPS (16), next-MPS index (17-23), next-LPS index (24-30); 376 bytes. Same layout
   as the Rust port. `static_assert`s pin a few rows. No dynamic initialisation (no first-use race).
2. **Contexts** (`struct MqContexts { uint32_t slot[19]; }`, 76 bytes): `reset()` writes the three non-zero
   initial states (UNI 92, AGG 6, ZC 8) from a `constexpr` array. Replaces 19 pointers and `curctx` (160 bytes);
   a pass keeps the slot address of the context it decides, as today's `curctx`.
3. **Byte input** (`struct MqInput { const uint8_t* bp; const uint8_t* end; uint8_t exhausted; }`): the
   code-block's bytes are gathered once (as today) into a layout with **each segment followed by `0xFF 0xFF`**,
   after checking once that the segments fit the bytes. Read-only afterwards: no sentinel written into the next
   segment, no backup and restore, no counter; `exhausted` is set in the marker branch.
4. **The decision** (the backend seam): `inline uint32_t mq_decide(MqCoder&, uint32_t& slot)`, the select form
   and the always-run renormalisation; `mq_raw(MqCoder&)` for BYPASS; `mq_agg_run(...)` for the cleanup's run
   mode. `MqCoder` is `a`, `c`, `ct` and the input, copied into locals for a pass as today
   (DOWNLOAD/UPLOAD). The scalar form is the reference and the only backend now; a per-CPU form replaces this
   part alone.
5. **Tier-1 coupling** (`src/t1.cpp`): the passes stay OpenJPEG's text and call only the three functions above;
   the cleanup's `flags == 0` branch calls `mq_agg_run`. A backend that decodes whole code-blocks differently
   (lanes, GPU) plugs in at `t1_decode_cblk(T1&, const T1Block&, const Messages&)`, the seam that the
   code-block threading already uses (`tile.cpp:498-555`).

The decision, exactly (the same function as `opj_mqc_decode_macro`; proof in section 6):
```
w = slot; qe = w & 0xFFFF; a -= qe;
lpsi = (c >> 16) < qe;  flip = lpsi ^ (a < qe);  d = ((w >> 16) & 1) ^ flip;
slot = (lpsi | (a < 0x8000)) ? kMqWords[flip ? w >> 24 : (w >> 17) & 0x7F] : w;
c -= lpsi ? 0 : qe << 16;  a = lpsi ? qe : a;
s = clz16(a);  if (s <= ct) { a <<= s; c <<= s; ct -= s; } else renorm_loop();   // byte-in as today, lazily
```
The AGG run: with the AGG slot's MPS 0 and `room = min(a - 0x8000, c >> 16)`, the columns from the current
one while their flags are 0 and `n*qe <= room` are each an MPS 0 without renormalisation; `a -= n*qe`,
`c -= (n*qe) << 16`, skip `n` columns (they have no other effect), else the normal decision.

Data flow and threads: unchanged. Tier-2 builds `T1Block`s; each Tier-1 job owns a `T1` (now with
`MqContexts` and the padded segment buffer), decodes its run of blocks and stores into its own place in the
tile buffers. Failure behaviour: no error per decision; `t1_decode_cblk` keeps its one refusal (bit-planes plus
ROI shift at or above 31); segments that do not fit the bytes (unreachable today) are cut to the bytes present,
deterministic, documented.

**The firewall contract, short.** For any `T1Block` (any bytes, segment lengths and passes, styles, sizes up to
the header's limits, `numbps`, ROI shift): reads only inside the gathered bytes plus their padding, writes only
inside the block's coefficients, flags and contexts; O(1) work a decision and at most about 3 decisions a sample
a bit-plane; no state survives from one block to the next (contexts reset, coder initialised per segment,
scratch cleared); the output is a function of the block alone; no UB (unsigned arithmetic, shifts below 32,
magnitudes below 2^31). Valid streams: the same coefficients as today, bit for bit.

Alternatives considered: byte-index states with selects (`ps`, 2-8 % slower than words); a (context, bit)
buffer between passes and MQ (the encoder study found it slower than fused, and it costs scratch); lanes
across code-blocks (above); a separate hand-tuned AVX2 decision (nothing to vectorise in one decision).

## 5. Costs

CPU [m, replay; end-to-end estimated from the Tier-1 shares as `1 - share × (1 - ratio)`]:

| Config | Today decode | Stage 1 Tier-1 / decode (est.) | Stage 1+2 Tier-1 / decode (est.) | Stage 1+2 ms (est.) |
|---|---|---|---|---|
| 256² lossy | 8.84 Mcycles, 2.06 ms | 0.869 / 0.88 | 0.849 / 0.86 | 1.77 |
| 256² lossless | 50.1, 11.7 ms | 0.721 / 0.72 | 0.721 / 0.72 | 8.4 |
| 512² lossy | 42.9, 10.0 ms | 0.838 / 0.85 | 0.786 / 0.80 | 8.0 |
| 512² lossless | 229.0, 53.3 ms | 0.715 / 0.72 | 0.712 / 0.71 | 38.0 |
| 1,024² lossy | 172.1, 40.1 ms | 0.857 / 0.86 | 0.790 / 0.80 | 32.0 |
| 1,024² lossless | 926.7, 215.9 ms | 0.718 / 0.72 | 0.712 / 0.71 | 153.9 |

Memory: per `T1` (one per concurrent Tier-1 job) about 90 bytes less (19 pointers and `curctx`, the backup
and the counter go; 76 bytes of slots come); the padded segment layout needs 2 bytes a segment more than
today's 2 bytes a block (at most 109 segments a block: under 220 bytes, inside the existing doubling buffer).
Read-only data: +376 bytes (`kMqWords`); `kMqcStates` (2,256 bytes) stays while the encoder uses it. No GPU work.
Threads: none added; more cores scale as today through the code-block jobs.

## 6. Compatibility and proof of exactness

Users see only time. The coefficients are today's on every valid stream (and so OpenJPEG's); messages, status
and the refused-block warning are unchanged; malformed streams decode as today on every input reachable from
Tier-2, and the one unreachable case (segments longer than the bytes) is cut instead of writing past the buffer.

Proof plan:
- **Differential (the oracle):** a frozen copy of today's `t1.cpp` decoder in the test (namespace `ref`), and
  `tests/unit/t1_unit.cpp` comparing coefficients and result on: the 35,390 blocks of the mode-switch corpus
  (dumped by the study tool, stored as test vectors with their coefficient hashes), the six study
  configurations, and generated blocks (every style 0-63, sizes 1..64 and 1024×4/4×1024, heights not a multiple
  of 4, `numbps` 0-31, ROI shifts, random and adversarial bytes: all `0xFF`, `0xFF 0x90` markers mid-segment,
  zero-length segments, 109 one-byte segments).
- **Exhaustive decision test:** `mq_decide` against today's macro for every state (94), every `a` in
  [0x8000, 0xFFFF], `c_hi` at `qe - 1`, `qe`, `qe + 1` and a spread, `ct` 0-8, the next two bytes from
  {00, 7F, 8F, 90, FE, FF}: about 10^9 single decisions, d, `a`, `c`, `ct`, `bp`, slot equal (tens of seconds;
  a reduced set in `ctest`, the full one nightly), as the encoder study's `mq_exhaustive`.
- **AGG run property test:** for every state with MPS 0, `a`, a spread of `c_hi` and 1-64 empty columns, the run
  equals the decisions one at a time (registers and slot).
- **Fuzzing:** a new `fuzz/fuzz_t1.cpp` (libFuzzer and the standalone main): the input becomes a `T1Block`
  (header bytes for size, style, `numbps`, ROI, segment lengths and passes, then the bytes) and runs the frozen
  reference and MQ-NG, asserting equal coefficients and result; under ASan+UBSan (`linux-clang-asan-ubsan`,
  `windows-msvc-asan`, `windows-clang-cl-asan`) and `linux-clang-fuzz`; seeds from the dumped blocks. The
  existing `fuzz_decode` keeps covering Tier-2 to Tier-1; its regressions and bombs stay in the suite.
- **Threads:** `linux-clang-tsan` on the threaded decode tests (no new shared state; the table `constexpr`).

## 7. Staged plan

| Stage | Scope | Gate (tests, performance) | Owner's check | Effort |
|---|---|---|---|---|
| 0 | Tests first: the frozen reference, `t1_unit` with the corpus vectors and generated blocks, `fuzz_t1`, the exhaustive decision test, the AGG property test (against today's code it passes trivially). | Green on all presets (MSVC, clang-cl, GCC, Clang; AVX2, SCALAR; ASan/UBSan, TSan, fuzz) | that the frozen copy is today's file | 1.5 days |
| 1 | `src/mq_tables.h` (`kMqWords`), `MqContexts`, `mq_decide` (select, always-run renorm) and `mq_raw` as inline functions; the padded read-only segment layout, the once-a-block length check, `exhausted`; the passes call them | stage 0, `decode_unit`, oracle hashes, fuzz seeds; Tier-1 ≤ 0.78 lossless and ≤ 0.91 lossy of today, whole decode ≤ 0.79 / 0.92, on AVX2, SCALAR and clang-cl (measured 0.715-0.774 / 0.838-0.895); GCC/Clang Linux measured | the numbers in `doc/bench.md` | 1.5 days |
| 2 | `mq_agg_run` in the cleanup pass's run mode | as stage 1; ≥ 3 % faster than stage 1 on 512² and 1,024² lossy on every build (measured 6-11 %), no slower than 1 % elsewhere (measured +0.4 % to -3 %) | decision 1 | 0.5 day |
| 3 | `doc/bench.md` section, `src/t1.cpp` header (what departs from OpenJPEG's text: operations, not results), README row | review | wording | 0.5 day |
| later | Rust: `neoj2k-rust` `t1/mq.rs` `symbol()` to the select form, `agg_run`, the test vectors and `fuzz_t1` corpus | its own gates (bit-exact against the C++ vectors) | go-ahead | - |

Total about 4 days. If stage 1's whole-decode numbers on GCC/Clang Linux miss the gate (a compiler turning the
selects into branches), stop and report before stage 2.

## 8. What ages well, what is tuned for today

| Decision | Kind | Why | Cost |
|---|---|---|---|
| State word in the context slot | ages well | one dependent load fewer per decision on any core; small table (376 B) fits any L1 | +376 B read-only |
| Select decision, renormalisation without loop | ages well | removes data-dependent branches; `cmov`/`csel`/RISC-V `czero`; gains grow with pipeline depth; measured on two compilers and baseline x86-64 | none |
| AGG run | ages well | removes work per empty column (algorithmic) | about 20 lines |
| Padded read-only segments, check once | ages well | no writes into the input, slice-shaped for Rust, explicit contract | 2 B a segment |
| Code-block seam for other backends and threads | ages well | already used by the jobs; more cores scale | none |
| Frozen scalar reference as oracle | ages well | every future backend proves against it | test code only |
| Early sign-context load (wspec) | tuned for today | 0.8-1.7 %, out-of-order dependent | rejected |
| Macro versus inline function | layout | ±2 % | functions chosen |
| `lzcnt` versus `bsr` | build | both kept (AVX2 / SCALAR) | none |
| 8-lane AVX2 MQ | tuned, and loses | slower than the scalar select decoder; revisit only with vectorised modelling (AVX-512 scatter, SVE) | closed |

## 9. What carries over to the Rust version

Carries over: the five-part structure and the seams; the word table and its layout (already the Rust port's);
the select decision and the AGG run, line for line in safe Rust (no `unsafe` needed: the slot array is
`[u32; 19]` indexed by a context below 19, the table index below 128 by mask); the byte input as a slice (the
bounds-checked read measured within 0-2 %, so the sentinel copy is optional in Rust); the test vectors (the
`.blk` dumps and their coefficient hashes: a plain little-endian format), the exhaustive decision cases, the
fuzz seeds and the malformed-block corpus. Does not carry over: the macro/DOWNLOAD idiom (Rust keeps the coder in
a struct the optimiser holds in registers), MSVC-specific layout findings, the `__forceinline` seam (Rust
`#[inline(always)]`), and every speed ratio (to be measured again on the Rust code against the C++).

## 10. Decisions for the owner

1. **Stage 2's gate.** (a, recommended) Accept: 6-11 % on lossy 512² and up, 2-5 % at 256² lossy, neutral on
   lossless. (b) Every configuration ≥ 3 %: then drop it.
2. **Byte input.** (a, recommended) Fold the padded read-only layout, the check and `exhausted` into stage 1,
   so stage 1's gain carries a step that is neutral on its own (wchk: within 1 %). (b) A separate stage accepted
   at noise. (c) Keep OpenJPEG's sentinel write and restore.
3. **Table shared with the encoder.** (a, recommended) `kMqWords` for the decoder now; the encoder keeps its
   planned 4-byte packed table; converge after both are benched. (b) Move the encoder to state words now
   (not measured).
4. **Form of the decision.** (a, recommended) Inline functions in `src/mq.h` called from the macro passes
   (seam measured free). (b) Macros inside `t1.cpp` as today.
5. **SIMD lanes.** (a, recommended) Closed for AVX2; reopen only with a vectorised context-modelling design.
   (b) Keep the prototype in the bench.
6. **Unreachable over-long segments.** (a, recommended) Cut to the bytes present (same status). (b) Refuse the
   block (a status change on an input Tier-2 cannot produce).
7. **Rust timing.** (a, recommended) After the C++ stages land, with their vectors. (b) Now, in parallel.

## 11. Risks, and what was not verified

- GCC and Clang on Linux were not measured; clang-cl was (0.74-0.82 with both stages). A compiler that turns
  the selects into branches would lose most of stage 1; the gate measures it.
- The corpus is the encoder study's synthetic texture at six configurations plus the test corpus for
  exactness; the owner's private 212-file corpus (real Second Life textures) is not on this machine, so its
  mode-switch census and timings are not here.
- The end-to-end decode was not rebuilt with the candidates; its ratios are estimated from Tier-1's 93-99 %
  share. Stage 1 gates on the real library build.
- 256² lossy is a small set: its noise control ranged 0.947-1.012 in one run.
- No PMU counters; branch misses are derived from the outcome stream.
- The lanes comparison mixes this corpus (the select decoder) with the 212-file corpus (the lanes prototype).
- Not explained: the word-select decoder alone slower than the byte-select one alone.

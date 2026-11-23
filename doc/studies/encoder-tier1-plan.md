<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026, Anne Skydancer (neo-j2k) -->

# Encoder Tier-1: survey and staged plan (2026-11-22)

A study for the owner. Nothing here is implemented. It covers lever A (scheduling of the Tier-1
code-blocks) and lever B (the scalar MQ coder and its passes) of the Part 1 encoder, as the owner scoped
them. Out of scope: lever C (8-lane AVX2 MQ), lever D (SIMD helpers), mode switches, the decoder.
The rule for every change: keep it only if it is bit-exact (D9: OpenJPEG 2.5.4's bytes in C++; byte parity
between Rust and C++), not slower, and adds no complexity or fragility.

Neither repository has an `AGENTS.md`; the Rust repository has no `doc/stages.md` or `doc/bench.md`. The
rules used here are this repository's `doc/stages.md`, `doc/bench.md` and `doc/testing.md`.

## Summary

- **Lever B pays, at no memory cost.** Three changes together make one-thread encodes 8-16 % faster. They
  are: count-leading-zeros renormalisation, the MQ registers as scalar locals across a pass, and `byteout`
  out of line. This brings neo-j2k level with OpenJPEG, which is 14-18 % faster than neo-j2k today.
  Every output was byte-identical. The MQ coder is 53-63 % of one-thread Tier-1.
- **Lever A pays, and it is mostly about resilience.** Today Tier-1 runs 98 batches of 8 blocks at
  1,024², each followed by a join. When one CPU is late, the whole batch waits about one block's time.
  The fix is a dynamic pull over windows of up to 4 blocks per worker, with the commits kept in order on
  the caller. It cuts 8-thread encodes by 5-15 % on an idle machine and by 9-20 % with one CPU busy.
  Outputs were identical at every setting. The cost is 16 KiB of output room per worker (decision D1).
- **Costly-blocks-first is not worth it.** Codestream order already puts each component's costly
  low-resolution blocks first. Ordering by cost inside a window measured no gain.
- **Overlapping the component wavelets with Tier-1 gains nothing as specified.** The components'
  wavelets start and finish together, and a job may not wait for another (the executor contract). Once
  lever A has balanced Tier-1, the wavelet becomes the largest serial term on photographic content: 27 %
  of an 8-thread 1,024² lossy encode. That is decision D5.
- **Allocation order: no test depends on it.** No current test injects an allocation failure into the
  encoder, in either repository. The C++ encoder has no host allocator at all. The design keeps the
  commit allocations in code-block order, sized as serial, so the logical failure point is unchanged.
- **A test gap was found.** The encoder's threaded path is not run under ThreadSanitizer in CI. The TSan
  job selects tests named `threads`, and the encoder's thread test lives inside `encode_unit` (stage 0).

## How it was measured

- **Hardware and build:** a Ryzen 9 9950X3D with 8 logical CPUs visible to this Windows session, and
  MSVC (VS 18) RelWithDebInfo with AVX2, as `doc/bench.md`.
- **Builds measured:** instrumented copies of HEAD (bee41d4), built in the session's scratch directory,
  never in the repository. The variants were compile-time switches in copies of `encode.cpp` and
  `enc_t1.cpp`:
  - rdtsc phase and per-block probes;
  - a symbol counter;
  - an MQ stub that keeps the pass control flow but codes nothing;
  - the lever-B candidates;
  - a window scheduler selected by an environment variable.
- **Bench and inputs:** `nj2k_encode_bench` runs the viewer's upload encode, RGB, 6 resolutions,
  64 x 64 blocks, with the tests' pool as the executor. Two inputs were used:
  - "synth" is the bench's synthetic texture (gradients, edges and noise);
  - "photo" is a centred crop of `Bretagne2.ppm` (2,592 x 1,944) from OpenJPEG's test data, added
    because the owner's private bench corpus (`work/benchdata`) is no longer on disk.
- **Method:** runs were interleaved and rotated between variants, 2-3 rounds, using the median of each
  run's encodes. Load from other processes was 7-22 % as sampled. All timings are from this one machine.
  Other hardware is estimated only where stated.

## Current state, ranked by what matters

### 1. Tier-1 is the encode, and its MQ coder is most of Tier-1

One thread, 1,024² RGB (ms):

| Input | Whole tile | Tier-1 | Wavelet | Rate allocation and Tier-2 | DC shift and colour |
|---|---|---|---|---|---|
| synth lossy | 283.8 | 258.6 (91 %) | 16.2 | 8.0 | 1.0 |
| synth lossless | 294.8 | 286.6 (97 %) | 7.7 | 0.3 | 0.2 |
| photo lossy | 89.6 | 67.7 (76 %) | 17.1 | 3.8 | 1.0 |
| photo lossless | 131.9 | 124.0 (94 %) | 7.5 | 0.2 | 0.2 |

The MQ stub (passes, contexts and flags kept, no coding) removes 53-63 % of Tier-1. At 1,024² that
was 155 of 255 ms for synth lossy, 180 of 285 for synth lossless, 36 of 68 for photo lossy and 78 of
124 for photo lossless. There are 5.95-22.2 M symbols at 1,024². Per symbol, Tier-1 costs 11.7-13.4 ns, of which the MQ coder
is 6.0-8.1 ns.

Gathering and quantising a block's samples (`t1_job`, `encode.cpp:582-652`) is 0.3-2.5 % of Tier-1. It
is not a lever, and the 9/7 division by the step size must stay a division to be bit-exact.

The relevant code in `src/enc_t1.cpp`:

- **The MQ encoder** is a struct with member functions (`MqEnc`, 97-230):
  - `encode` (181-207) branches on `mps == d`;
  - `renorme` (169-179) is OpenJPEG's one-bit loop;
  - `byteout` (133-167) carries neo-j2k's bound check (OD-4) and is inlined into every symbol site.
- **The passes** call `mqc->encode` through `t1->mqc`: `enc_sigpass` (354-390), `enc_refpass` (409-453)
  and `enc_clnpass` (510-553).
- **Why the registers stay in memory:** MSVC does not use type-based aliasing, so every store to the
  `uint32_t` flags forces `a`, `c` and `ct` back to memory. OpenJPEG's own passes avoid this. They keep
  `a`, `c`, `ct` and `curctx` in locals (`DOWNLOAD_MQC_VARIABLES` / `UPLOAD_MQC_VARIABLES`,
  `oracle/openjp2/t1.c:495, 583, 829, 925`), and their byteout is out of line (`mqc.c:487`). That is why
  OpenJPEG's encoder is faster on one thread:

  | 1,024², one thread | OpenJPEG / neo-j2k |
  |---|---|
  | synth lossy | 0.845 |
  | synth lossless | 0.855 |
  | photo lossy | 0.831 |
  | photo lossless | 0.859 |

  `doc/bench.md` (stage 6a) recorded 1.11-1.17 the other way round under load, the same gap.

### 2. Tier-1 scheduling: join-barrier batches of `width` blocks

`encode.cpp:725-814` sets Tier-1 up and runs it. The sequence:

1. One job list of every block, in codestream order (component, resolution, band, block).
2. `width = min(count, executor.concurrency, 64)` scratches `EncT1` from the allocator (about 39 KiB
   each), plus `width` pointers and flags from the arena.
3. Then, for each batch of `width` consecutive blocks:
   - `parallel.run(batch)`, one block per job, then join;
   - commit in order on the caller (`enc_t1_commit`, `enc_t1.cpp:761-776`: arena copies of the passes
     and bytes);
   - stop at the first failure, `NJ2K_E_LIMIT` for an output overflow or `NJ2K_E_NO_MEMORY` for a commit.

At 1,024² there are 777 blocks: 98 batches at 8 threads. At 512² there are 210 blocks: 27 batches.

8 threads, 1,024², synth (ms of Tier-1 wall):

| | Tier-1 wall | sum of batch maxima | ideal (CPU / 8) | lost to imbalance | lost to dispatch and join |
|---|---|---|---|---|---|
| lossy | 41.9 | 38.3 | 32.1 | 6.2 | 3.7 |
| lossless | 80.7 (noisy run) | 50.1 | 38.7 | 11.4 | 30.6 |
| 512² lossy | 15.8 | 11.1 | 8.1 | 3.0 | 4.7 |

How the time is lost:

- **One CPU late stalls the batch.** The start skew inside a batch was often about one block's time
  (280-330 µs median in the loaded runs). One of the 8 CPUs was taken by another process. The pool
  correctly ran the late job on whichever thread finished first, but the batch still waited for it.
  That is the fragility.
- **Barriers:** 98 of them cost 15-21 µs each between batches, plus the wake-up of sleeping workers.
- **Block costs:** they are fairly uniform on synth (CV 0.17-0.30) and very uneven on photo lossy
  (CV 0.88, 13-411 µs). A photo's low-resolution blocks cost 4-5 times its highest-resolution ones, and
  codestream order already starts each component with them.

Callers and threads:

- **Who calls:** `nj2k_encode` (`encode.cpp:1116-1146`) is the only caller of `encode_tile`. The
  openjp2 module's `opj_encode` forwards to it (`modules/openjp2/opj_forward.inc:60`).
- **Executors:**
  - the host's (`nj2k_encoder_set_executor`, `encode.cpp:886-903`);
  - the openjp2 module's own pool (`opj_codec_set_threads`, `modules/openjp2/openjp2.cpp:81-160`);
  - the tests' pool (`tests/common/thread_pool.h`).
- **How the pools claim jobs:** both pools have a waiting caller that claims jobs itself through an
  atomic counter. The contract allows any executor: jobs run in any order and must never wait for one
  another or call the codec (`include/neoj2k/neoj2k.h:2174-2190`).
- **What jobs touch:** they read the encoder's parameters and the tile planes. Each writes its own
  `EncCblk`'s `numbps` and `totalpasses` and its own scratch. The arena and the allocator are touched
  only by the caller.
- **Floating point:** each job runs under `DefaultFpEnv` (`parallel.h:69-75`).

### 3. The serial stages around Tier-1

8 threads, 1,024² (ms):

| Input | Tile | Tier-1 | Wavelet (3 jobs) | Rate allocation and Tier-2 | DC shift and colour |
|---|---|---|---|---|---|
| synth lossy, batches | 57.5 | 42.6 | 5.84 | 7.96 | 1.04 |
| photo lossy, batches | 26.9 | 16.0 | 6.05 | 3.75 | 1.03 |
| photo lossy, windows (lever A prototype) | 22.4 | 11.5 | 6.05 | 3.80 | 1.03 |
| photo lossless, windows | 22.9 | 19.6 | 2.82 | 0.17 | 0.24 |

- **The wavelet** runs one job per component (`encode.cpp:708-723`), so 5 of 8 workers idle during it.
  The forward 9/7 takes about 5.7 ms per 1,024² component. The decoder's tuned inverse takes 0.47 ms
  (`doc/bench.md`, stage 3).
- **The rate allocation** (`enc_t2.cpp:533-644`, OpenJPEG's bisection with a Tier-2 simulation per step)
  is serial: 8 ms for synth lossy.
- Header and set-up are under 0.05 ms.

## Lever B: the MQ coder and its passes (measured, one thread)

Each variant against today's code, as the whole-encode time ratio (median of 3 interleaved rounds, both
images, 512² and 1,024², lossy and lossless):

| Variant | Ratio range | Note |
|---|---|---|
| clz renormalisation only | 0.919-0.959 | the same byteouts at the same shifts |
| branch-free symbol with clz (the Rust twin's form) | 0.884-0.969 | |
| locals as a struct copy | 0.984-1.04 | slower: the copy is not split into registers |
| scalar locals (`a`, `c`, `ct`, `curctx`), byteout inline | 0.966-1.001 | |
| `byteout` out of line only | 1.016-1.040 | slower alone |
| **scalar locals + clz + byteout out of line** | **0.836-0.918** | the winner |
| scalar locals + branch-free + byteout out of line | 0.922-0.984 | the branch-free form does not combine |
| OpenJPEG 2.5.4 (the oracle) | 0.822-0.859 | |

The winner measured 0.836-0.858 on the four synth configurations and on photo lossless, and 0.918 on
photo lossy. On photo lossy the rest of the gap to OpenJPEG lies outside Tier-1 (the wavelet). Every
variant gave the same checksums as today on all eight configurations.

Not measured:

- **Packed state words** (a `u8` index into a 4-byte state table, as in Rust). The state table
  (`t1.h:55`, `t1_tables.h:15`) is shared with the decoder, so packing would mean a second, encoder-only
  table.
- **Small uploads (64²-256²) on one thread.** The stage gate must include them.
- **Hoisting `t1->w`.** `enc_refpass` and `enc_clnpass` reload it in their loop conditions after every
  flag store; it comes with the locals change.

## Lever A design: pull windows with ordered commits

### What changes

All of it is in `encode.cpp:725-814`, plus the Tier-1 interface in `enc_tile.h:135-149` and
`enc_t1.cpp`.

**1. Split the scratch.**

- *Today:* `EncT1` holds the samples, flags, MQ state, passes and a worst-case output buffer, about
  39.9 KB.
- *Change:* split it into:
  - a core of samples, flags and MQ state, about 21.4 KB;
  - an output region per puller of `kBlockWorst + kSlack` bytes. `kBlockWorst` is the fake byte, the
    output (`kOutCapacity`, 16,677 bytes) and the passes (75 x 24 bytes): 18.5 KB. `kSlack` is 16 KiB
    (D1).
- `enc_t1_encode` writes a block's bytes and passes at the region's cursor: the fake zero byte, then the
  bytes, then the passes, 8-byte aligned.
- The MQ limit is the block's own worst case, so overflow detection is unchanged. A puller starts a
  block only when the room left is at least `kBlockWorst`. A region that held its first block always has
  room for that block.

**2. Pullers.**

- One `parallel.run(P)` per window, with `P = min(width, blocks in the window)`. Puller `p` owns core
  `p` and region `p`.
- Each puller loops:
  - if region `p` has less than `kBlockWorst` left, stop;
  - claim `i = next.fetch_add(1)`; if `i` is at or past the window's end or at or past `fail_at`, stop;
  - encode block `i` into region `p`, and leave its staged pointers in its `EncCblk` (`data` and
    `passes`);
  - on failure, lower `fail_at` to `i` (an atomic minimum) and stop.
- A window holds at most `4 x width` blocks. The k4 setting measured best overall, and k8 was slower at
  256² photo, where a tile has only 75 blocks.

**3. Commit on the caller, in order.**

- Every claimed block below the window's end was encoded, because claims are monotone and pullers check
  room before they claim. So the done blocks are `[first, min(next, window end))`.
- The caller commits them in order. Each commit copies the staged bytes and passes into the arena and
  repoints the block: the same allocations, the same sizes and the same order as serial.
- At `fail_at` it returns `NJ2K_E_LIMIT`. Every block below `fail_at` is complete, so this is the
  lowest failing index whatever the scheduling.
- Before the regions are freed or reused, it nulls the staged pointers of any block it did not commit.
- The next window starts at the end of this one. The cursors reset.

**4. Allocation order.**

- *Today:* the width-sized arrays (`EncT1*` pointers and ok flags) come from the arena, so the arena
  offsets during the commits depend on the width.
- *Change:* move them, the cores and the regions to the allocator, freed after Tier-1 as the scratches
  are today. The arena's sequence is then identical for every width, which makes "the same logical
  point" exact rather than nearly so.

**5. Serial, in the same code.**

- With no executor or below `kParallelEncodeFloor`, the same loop runs with one puller called directly
  and `kSlack = 0`. Its memory is today's exactly (core plus one worst case), and it commits after every
  block.
- This replaces the batch loop and `run_jobs` with one path.

**6. Threads.**

- One relaxed atomic claim counter and one relaxed atomic `fail_at`. The join gives the caller every
  job's writes (the executor contract).
- Each job writes only its own core, region and `EncCblk`. Jobs do not wait for one another, so the
  contract holds for any executor, including one that runs every job on the caller.

### Failure behaviour

- **Overflow** (a block's MQ output past its worst case, which OpenJPEG would write past):
  `NJ2K_E_LIMIT`, then "nj2k_encode: the tile could not be encoded". This is as today, at the same block.
- **Allocator refusal:** of the cores or regions before Tier-1, `NJ2K_E_NO_MEMORY` before any block
  runs; of a commit, `NJ2K_E_NO_MEMORY` at the same block as serial.
- **Messages:** none are emitted in jobs, so the order is unchanged.

### Alternatives considered

| Option | Why not |
|---|---|
| Keep batches, order each batch by cost | Does not address the late CPU or the 98 barriers. Costs are not known before coding; resolution is the only cheap proxy, and codestream order already follows it. |
| Fixed K worst-case output slots (the prototype) | 18.5 KB per slot whatever the content. The region design gets the same window length for the same memory and holds more small blocks. |
| One shared byte pool, copied after encoding | Same memory, the same window length per byte, one more copy and an atomic per block. Regions are simpler. |
| Cost-first order within a window (measured as k8c) | No gain beyond noise (0.79-0.97, against 0.80-0.93 for k8). Declined. |
| Commit in jobs under a lock | Calls the host allocator from workers. The brief rules it out, and an allocator that is not thread-safe or has thread affinity would break. |
| Overlap the component wavelets with Tier-1 | A component's blocks are ready only when its whole wavelet is done, and the three start and finish together. Pullers cannot wait, so idle workers have nothing to take: the gain is near zero. Only splitting the wavelet itself helps (D5). |

### Measured, 8 threads (prototype with K = m x width worst-case slots, base MQ)

Median encode time as a ratio to today's batches. Byte checksums were identical in every cell.

| Image | Mode | Idle k2 | Idle k4 | Idle k8 | One CPU busy k4 | One CPU busy k8 |
|---|---|---|---|---|---|---|
| synth 256² | lossy / lossless | 0.977 / 0.936 | 0.951 / 0.895 | 0.972 / 0.909 | | |
| photo 256² | lossy / lossless | 0.961 / 0.936 | 0.954 / 0.901 | 1.080 / 0.997 | | |
| synth 512² | lossy / lossless | 0.928 / 0.910 | 0.883 / 0.848 | 0.868 / 0.880 | | |
| photo 512² | lossy / lossless | 0.922 / 0.931 | 0.873 / 0.861 | 0.892 / 0.873 | | |
| synth 1,024² | lossy / lossless | 0.940 / 0.944 | 0.900 / 0.959 | 0.886 / 0.897 | 0.829 / 0.913 | 0.810 / 0.809 |
| photo 1,024² | lossy / lossless | 0.939 / 0.940 | 0.904 / 0.888 | 0.931 / 0.898 | 0.826 / 0.796 | 0.816 / 0.795 |
| synth 2,048² | lossy / lossless | 0.916 / 0.979 | 0.884 / 0.917 | 0.867 / 0.902 | | |

Absolute examples, k4 against batches, 8 threads (ms):

| Image | Idle, batches | Idle, k4 | One CPU busy, batches | One CPU busy, k4 |
|---|---|---|---|---|
| synth 1,024² lossy | 64.5 | 58.0 | 73.8 | 61.2 |
| photo 1,024² lossy | 28.3 | 25.6 | 31.8 | 26.3 |

The region design with `kSlack = 16 KiB` and a cap of 4 blocks per puller holds k4's window on this
content: synth blocks average 2.7-3.2 KB with their passes, photo blocks 0.4-1.3 KB. Its timing is
inferred from the k4 prototype and is not measured on its own.

## Costs

Memory per encode at width W, against today's 39.9 KB per worker:

| | Today | Recommended (`kSlack` 16 KiB) | Memory-neutral (`kSlack` 0) |
|---|---|---|---|
| per worker | 39.9 KB | 56.3 KB | 39.9 KB (no gain: one block per window) |
| W = 8 | 319 KB | 450 KB (+131 KB) | 319 KB |
| W = 64 | 2.55 MB | 3.60 MB | 2.55 MB |
| serial | 39.9 KB | 39.9 KB | 39.9 KB |

What the extra 131 KB at W = 8 means against the encode's peak (`doc/bench.md`, stage 6a):

| Image | Peak | Extra memory | Time saved at 8 threads (k4) |
|---|---|---|---|
| 256² | 2.4 MB | +5.5 % | 5-10 % |
| 512² | 9.5 MB | +1.4 % | 12-15 % |
| 1,024² | 37 MB | +0.35 % | 4-11 % idle, 9-20 % with one CPU busy |

Tier-2 allocates only tag trees, so the encoder's peak is reached during Tier-1, and the regions add to
it. Lever B changes no memory.

CPU:

- **Lever A:** one more copy per block (region to arena, as today's out buffer to arena), under 1 µs
  against 50-400 µs a block. One atomic per block. 13 parallel_for calls instead of 98 at 1,024².
- **Lever B:** 8-16 % less CPU per encode, on every thread count.

Expected 8-thread time, 1,024² photo lossy:

| Step | Tile (ms) | Basis |
|---|---|---|
| Today | 26.9 | measured |
| With A | 22.4 | measured, prototype |
| With A and B | about 21 | estimated: Tier-1 x 0.85-0.9 |
| With A3 as well | about 18 | estimated |

The synth equivalent is 57.5, then 55.8, then about 50 ms.

There is no performance budget in the project for encodes beyond "not slower" and the stage 6b numbers.

## Compatibility

- **Users see** the same bytes, status and messages at every thread count, and faster encodes.
- **The memory** of a threaded encode goes up by 16 KiB per worker (D1).
- **The public API** does not change, nor the executor contract. `doc/api.md`'s encoder text stays true.
- **Before and after evidence:** all prototype runs gave the same checksum as today's code in every
  configuration: 512² lossy `e044e820324895fb`, 1,024² lossy `28cee6066552cba0`, 1,024² lossless
  `f60bb6bb1f054bee`, and the photo crops.

## Staged plan (C++ first; the Rust twin follows)

| Stage | Scope | Gate (tests, performance) | Owner's check | Effort |
|---|---|---|---|---|
| 0: tests first | See below. On `main`'s code. | Passes on today's code and under TSan in CI. | That the matrix is the one wanted. | S, about half a day |
| B: scalar MQ | See below. Only `enc_t1.cpp` and `t1.h`. | See below. | The bench table, per stage 6a's format. | S, half to one day |
| A1: pull windows | See below. | See below. | D1's memory against the measured gain. | M, 1-2 days |
| A2 (optional): DC shift and colour transform in row bands | The two loops of `encode.cpp:672-696` cut into row bands through `parallel.run` when threaded. Element-wise and independent per sample, so bit-exact by construction. | Identity matrix; 8 threads not slower anywhere; about 0.8 ms saved at 1,024² (estimated: 1.0 ms becomes about 0.2). | Whether 3-4 % of an 8-thread photo encode is worth a dozen lines. | S |
| A3 (optional, D5): parallel forward wavelet | Each large level's vertical pass cut into column stripes and its horizontal pass into row bands, as the decoder's stage 5 does; small levels serial. | Identity matrix and TSan; 8 threads, 1,024² photo lossy: wavelet 6.05 to at most 3 ms (estimated); no size slower. | D5. | M |
| R: Rust follow-up | See below. | See below. | Rust's own bench numbers. | M, 1-2 days |

**Stage 0: tests first.**

- Add an `encode_threads` CTest entry (`nj2k_encode_unit --threads`), so that the TSan job's filter on
  `threads` runs it.
- Widen `test_threads` (`encode_unit.cpp:1102-1144`):
  - widths 2, 3, 4, 5, 7, 8, 16, 32 and 64;
  - sizes 256², 257 x 300, 512² and 1,023 x 1,024;
  - 1, 3, 4 and 8 components;
  - both wavelets, and 1 and 3 rate layers.
- Two test-only hooks, not exported, like `set_parallel_thresholds`:
  - force block i to fail, and check the same status, messages and stream bytes at every width;
  - an arena budget for the encoder (`Arena::set_budget` exists), with the same check.
- Add an executor that runs every job on the caller in reverse order, and one that delays one job.
- Add a stress loop: 500 encodes at random widths, 256²-512², under TSan.

**Stage B: scalar MQ.**

- The MQ registers as scalar locals across each pass, in a small register struct passed by reference
  into the inlined steps, written back around `byteout` and at the pass's end.
- clz renormalisation through the decoder's `mq_shifts`, moved from `t1.cpp:188` to `t1.h`. That gives
  lzcnt in the AVX2 build and bsr in the scalar one, whose object check refuses lzcnt.
- `byteout` out of line, through a new `NJ2K_NOINLINE` macro.
- `t1->w` and the stride hoisted into locals.
- Packed states only if measured: D4.

Gate for stage B:

- Bytes identical in `encode_unit` (OpenJPEG comparisons), the corpus re-encodes and the OpenJPEG
  checksums. The Rust differential is unchanged.
- One thread: at least 8 % faster at 512² and 1,024², synth and photo, lossy and lossless (measured
  8-16 %).
- No class slower, by the noise rule of two copies of one library, at 64², 128² and 256²: the uploads
  that stay serial.
- 8 threads not slower. The scalar build's object check passes, and ASan/UBSan are clean.

**Stage A1: pull windows.** The design above: cores and regions, puller loop, ordered commit, `fail_at`,
width-sized arrays out of the arena, serial as one puller. `enc_t1_encode` and `enc_t1_commit` get the
region interface.

Gate for stage A1:

- Stage 0's matrix at every width. TSan clean on the stress loop.
- 8 threads, k4-class gains:
  - idle: at least 8 % faster at 512² and 1,024² on both images;
  - one CPU busy: at least 10 % faster at 1,024²;
  - 256² and 2,048² not slower;
  - one thread within ±0.5 % (two copies' noise).
- Peak memory: the bench's peak goes up by at most `W x 16 KiB + 1 KiB`. The serial peak is unchanged.

**Stage R: Rust follow-up.** Rust has no arena and no commit: each job fills its own `Block`'s vectors
with fallible reserves.

- R1, lever B: Rust's `symbol` (`tier1/mq.rs:125-148`) is already branch-free with clz, and its slow
  path is cold and out of line. Measure it one thread against the new C++. If it is slower, port the
  C++ form (locals, branchy, clz); keep it only if faster.
- R2, lever A: pull windows of up to `4 x width` blocks over a fixed slot array, as `Jobs::run`'s 64
  `Mutex` slots (`executor.rs:41-71`) but claimed through an atomic counter by `width` pullers. Report
  the lowest-index error after each window, as `encoder.rs:434-439` does per batch now.

Gate for stage R:

- `encode_executor.rs` widened to stage 0's widths and sizes. `differential.rs` and the corpus
  re-encode byte for byte against the new C++.
- Miri if any `unsafe` is added (none planned).
- Rust's `large_encoder_executor_latency` (`small_image_gates.rs:523`) not slower at 256², and faster at
  512² and 1,024² with 8 workers.

Order: 0, then B, then A1, each one PR. B and A1 touch different functions, but A1 changes
`enc_t1_encode`'s interface, so B goes first. The coder, then reviewer and auditor, then checker on
each, per the pipeline.

## Decisions for the owner

**D1: the output room per worker for the windows.**

- **16 KiB (recommended):** about 4 blocks a worker, the k4 result. +131 KB at 8 threads; +5.5 % of the
  peak at 256², 0.35 % at 1,024².
- 8 KiB: about 2-3 blocks on synth (the k2 result, 0.91-0.98); +66 KB.
- 32 KiB: no measured gain over 16 KiB with the 4-block cap.
- 0: memory-neutral, but no gain: the windows fall back to today's batches.

**D2: costly blocks first.**

- **No (recommended):** measured no gain; codestream order already starts each component with its
  costly blocks.
- Order by resolution inside each window.

**D3: the form of the MQ symbol in C++.**

- **Scalar locals + clz + `byteout` out of line (recommended):** 0.836-0.918, level with OpenJPEG.
- Branch-free, the Rust form: 0.884-0.969 alone, and it does not combine with locals.
- clz only: 0.92-0.96.

**D4: packed state words (a `u8` index into a 4-byte state table).**

- **Skip unless measured (recommended):** not measured; the table is shared with the decoder, so it
  needs a second, encoder-only table.
- Try it in stage B and keep it only if at least 1 % faster.

**D5: the wavelet after lever A.** It is 27 % of an 8-thread 1,024² photo lossy encode.

- **A separate study of a row-wise forward 9/7 and 5/3 (recommended).** It is the one-thread term after
  Tier-1: 17 ms of 90 at 1,024² photo lossy, about 12 times the decoder's tuned inverse per sample. Row
  ordering is not SIMD, but the study may touch lever D.
- A3: the threaded wavelet now (about 3 ms at 8 threads, estimated).
- Leave it.

**D6: A2, the DC shift and colour transform in row bands.**

- **Yes, after A1 (recommended):** small and bit-exact by construction; about 0.8 ms at 8 threads,
  estimated.
- No.

**D7: the rate allocation, serial.** 8 ms at 1,024² synth lossy, 14 % of the 8-thread encode; 3.8 ms on
the photo.

- **A separate study, out of this scope (recommended):** it must stay OpenJPEG's bisection for D9, but
  the per-step walk over every pass may be made cheaper.
- Leave it.

**D8: stage 0's TSan entry.**

- **Land it first, on its own (recommended):** today's parallel encoder has no TSan coverage.
- Fold it into A1.

**D9: Rust lever A shape.**

- **Windows over a fixed slot array (recommended):** bounded stack memory and the C++ semantics.
- One slot per block for the whole tile: about 24 bytes a block, fallible allocation.

## Risks, and what was not verified

- **Hardware:** one machine (8 logical CPUs, MSVC, Windows), loaded 7-22 % by other processes.
  Linux/GCC/Clang, the scalar build and other CPU counts were not timed. The window's gain should grow
  with more workers (more barriers to lose) and with busier hosts. That is inferred, not measured.
- **Inputs:** the private bench corpus of Second Life textures was not available; a photo crop stood in
  for real content. Small uploads (64²-256², serial) were not timed for lever B; the stage B gate covers
  them.
- **Region design:** measured by proxy (K worst-case slots, k4), not as specified. Its window length
  depends on content; on incompressible content it shortens towards the k2 class (0.91-0.98), never
  below today's batches.
- **Locals:** the gain depends on MSVC keeping the register struct in registers; clang-cl and GCC were
  not measured. A layout-sensitive result could move by a few per cent between builds (`doc/bench.md`
  stage 4's experience); the gate is per build.
- **Not measured:** packed state words, stage A2, stage A3 and the region variant itself (all
  estimates). The Rust benches were not run; the Rust facts are from reading its code
  (`neo-j2k/src/encoder.rs:262-442`, `executor.rs`, `encoder/tier1/mq.rs:111-148`,
  `tests/encode_executor.rs:48-87`).
- **Allocation-failure semantics:** today they are not test-enforced in either repository, and the C++
  encoder has no host allocator (`encode.cpp:875-882`). Stage 0's hooks make them testable. Moving the
  width-sized arrays off the arena is what makes the logical point exact across widths.
- **Staged pointers:** a failed window leaves them in uncommitted blocks. The design nulls them before
  the regions are freed. The auditor should check this path, and that no reader of `EncCblk::data` runs
  after a Tier-1 failure (none does today: `encode_tile` returns at once).

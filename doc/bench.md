<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026, Anne Skydancer (neo-j2k) -->

# The bench

`nj2k_bench` times the viewer's request (the decode of `llimagej2coj.cpp`, and the upload encode
`JPEG2KEncode::encode`) with every decoder under test in one process, taking turns request by request.

```
nj2k_bench --a oracle --a "lib:C:/Program Files/Vulkanstorm-Release/openjp2.dll=shipped" \
           --a lib:build/<preset>/oracle/nj2k_oracle_counting.dll=counting \
           --files <list> --mode viewer --kinds full,budget,first --reduce 0-5 \
           --budget-min 15 --pin 7 --csv bench.csv --json bench.json
```

## Rules

- **Interleaved.** In every configuration (file, kind, reduce level) the decoders take turns request by
  request and the order rotates each round, so they share whatever load the machine has. What to gate on
  is the ratio to the first decoder, per file, then its median per class, plus the heap-call counts; load
  moves those far less than absolute times (plan audit C3).
- **Time-boxed, never waiting.** `--budget-min` (15 by default) caps the run; it never waits for a quiet
  machine. It records the load it ran under: total and other processes' CPU, and the compilers, linkers,
  benches and viewers it saw. The summary says when the box cut the run short.
- **Only the viewer's work is timed.** A timed round runs the request, its checks and its 8-bit copy, with
  message handlers that ignore the messages, as the viewer's do. The identity checksum (an FNV hash of every
  sample and of the 8-bit copy) and the heap counts come from untimed rounds before; hashing a 32² image
  costs about 10-17 µs, a third of the request.
- **One thread.** OpenJPEG starts a thread pool when `OPJ_NUM_THREADS` is set; the bench clears it at start
  and the summary says what it held.
- **Clock:** the TSC (nominal cycles), calibrated against `steady_clock` at start. Pin the thread with
  `--pin`. Every decoder needs its own name (`lib:<path>=<name>`): the rows are kept by name.
- **Heap calls:** the counting oracle module (`nj2k_oracle_counting`, OpenJPEG's allocation entry points
  counted) gives OpenJPEG's calls per request; neo-j2k's own path (`--a neoj2k-memory`: one codec kept
  across requests, decoding from memory into the viewer's buffer) counts its calls through the allocator
  hook, where the gate is zero after warm-up. `--a neoj2k` runs neo-j2k through the compatibility API, as
  the viewer's request is written today.
- Build the bench `Release` or `RelWithDebInfo`.

## Stage 0 baseline (2026-11-19)

The oracle at the 1a pin (static, MSVC 14.51 RelWithDebInfo), the shipped `openjp2.dll` (VS 2022, LTCG) and
the counting oracle, interleaved, on the owner's private bench corpus (212 files: 188 Second Life textures and
24 generated 5/3 controls), the viewer's request, pinned to core 7 of a Ryzen 9 9950X3D (8 logical CPUs
visible). **Loaded run:** other processes took 88 % of the CPUs on average (other agents' builds and benches),
so the absolute times are indicative only; the ratios and counts are what to compare. The timed rounds no
longer include the harness's identity hashes, which the first baseline did: they made 32² whole 48.7 µs and
1024² whole 56.8 ms. The earlier quiet bench measured 21.8 µs for the 1x1 floor and 29.7-35.4 µs for 32²
(`small-texture-stale` bench in the viewer's reports).

| Request (median per file, then over the class) | Files | Oracle µs | Shipped µs | Shipped / oracle | OpenJPEG heap calls (allocations / frees) |
|---|---|---|---|---|---|
| 32² whole | 28 | 36.4 | 36.0 | 0.99 | 549 / 575 |
| 32² at reduce 5 (1x1 floor) | 28 | 23.1 | 22.6 | 0.99 | 485 / 488 |
| 64² whole | 32 | 390 | 413 | 1.07 | 611 / 633 |
| 128² whole | 32 | 632 | 659 | 1.03 | 611 / 636 |
| 256² whole | 40 | 2,265 | 2,397 | 1.04 | 717 / 771 |
| 256² at reduce 3 | 40 | 160 | 175 | 1.09 | 645 / 663 |
| 512² whole | 40 | 11,811 | 12,303 | 1.05 | 1,028 / 1,148 |
| 1024² whole | 40 | 38,439 | 39,978 | 1.04 | 2,355 / 2,725 |
| 1024² at reduce 5 | 40 | 310 | 291 | 0.98 | 1,444 / 1,449 |
| 1024², first 600 bytes | 34 | 69.0 | 66.5 | 0.95 | 1,345 / 1,347 |

The heap calls are those of a whole viewer request (header pass and decode pass); 1a's exhaustion exit,
which stops the packet walk once a tile's data ends, takes about 100 off the cut 1024² request. The decoded
outputs equal the shipped DLL's in every whole-file class; the first-600-byte decodes differ in 23 of 34, as
the truncation fix keeps the delivered bytes. Upload encodes (`JPEG2KEncode::encode`, 23 textures of 32, 128
and 1024 px, lossy and lossless): the oracle's bytes equal the shipped DLL's in all 46 encodes; 32² lossy
86 µs, 128² 1.06 ms, 1024² 58 ms (loaded). The per-file data names private textures, so it stays off the
repository.

## Stage 1c (2026-11-19)

### Small images: the viewer's request

The oracle, neo-j2k through the compatibility API (`neoj2k`: the viewer's request as today, two codecs and a
stream per request) and neo-j2k's own path (`neoj2k-memory`: one codec kept across requests under the Second
Life limits, as the viewer will set it up, the header read from memory and parsed once per main header, the
decode into the viewer's 8-bit bottom-up buffer), interleaved, on the same private bench corpus (212 files),
pinned to core 7 of the Ryzen 9 9950X3D, MSVC 14.51 RelWithDebInfo; 2,050 configurations in 230 s (stage
1c's review round). Load: other processes 17 % mean, 40 % p90, 57 % max of 8 CPUs (other sessions' builds:
GCC, MSBuild, cl). Every decode is identical to the oracle's (the `same` column equals the file count in
every class), and no class of either neo-j2k path is slower than the oracle (the highest ratio is 0.96).

| Request (median per file, then over the class) | Files | Oracle µs | neo-j2k, compat. API µs | neo-j2k, memory µs | Memory / oracle | Memory path heap calls |
|---|---|---|---|---|---|---|
| 32² whole | 28 | 33.4 | 12.2 | **10.1** | 0.30 | 0 / 0 |
| 32² at reduce 3 | 28 | 24.6 | 6.40 | 4.12 | 0.17 | 0 / 0 |
| 32² at reduce 5 (1x1 floor) | 28 | 22.5 | 5.44 | 3.25 | 0.15 | 0 / 0 |
| 64² whole | 32 | 373 | 309 | 303 | 0.81 | 0 / 0 |
| 128² whole | 32 | 611 | 547 | 520 | 0.85 | 0 / 0 |
| 128² at reduce 5 | 32 | 26.6 | 8.95 | 6.60 | 0.25 | 0 / 0 |
| 256² whole | 40 | 2,310 | 2,179 | 2,048 | 0.92 | 0 / 0 |
| 256² at reduce 3 | 40 | 147 | 67.7 | 57.1 | 0.47 | 0 / 0 |
| 512² whole | 40 | 11,411 | 10,804 | 10,406 | 0.89 | 0 / 0 |
| 1024² whole | 40 | 37,730 | 35,345 | 34,831 | 0.93 | 1 / 1 |
| 1024² at reduce 5 | 40 | 296 | 201 | 172 | 0.64 | 0 / 0 |
| 1024², first 600 bytes | 34 | 66.0 | 27.1 | 23.8 | 0.36 | 0 / 0 |

The 32² whole request, stage 1c's target of 7-12 µs, takes 10.1 µs on neo-j2k's own path, with the Second
Life limits set (the same as the 10.1 µs the first stage 1c run measured without them: the working-memory
estimate is now computed once per header and reduce factor, where it had cost 1.8 µs a decode); through the
compatibility API it takes 12.2 µs, just above the target. Of what is left, a sampling profile of the memory decode gives Tier-1 about a third
(OpenJPEG's passes as they are), the 9/7 wavelet about a sixth, and the packet headers, tag trees and tile
layout about a sixth; the OpenJPEG-compatible request pays about 2 µs more for its two codecs and its stream.
Above 64² the time is Tier-1's, the same code as OpenJPEG's, so the gain narrows to 8-20 %; stages 2-5 (fused
output, row-wise wavelet, Tier-1 tuning) address it. A warmed-up codec makes no heap call up to 512²; a 1024²
decode needs more than the 8 MiB the codec keeps by default (`nj2k_decoder_set_retained_bytes`) and gives it
back each time.

### Large images: time and peak memory against OpenJPEG, one thread

Synthetic RGB textures written by the oracle's encoder (one layer, six resolutions, 64 x 64 code-blocks):
4,096² in one tile (9/7, 2.5 MB), 4,096² in 16 tiles of 1,024² (5/3 lossless, 33 MB), 8,192² in one tile (9/7,
10 MB) and 16,384² in 256 tiles of 1,024² (9/7, 40 MB). Each decode runs in a process of its own (the peak is
the process's peak commit, `PeakPagefileUsage`), the decoders in turn, three rounds (two for 16,384²), pinned
to core 7, `OPJ_NUM_THREADS` unset; medians. OpenJPEG and neo-j2k's compatibility API decode from a file
stream into the image they allocate; "from memory" holds the file's bytes in the process first (as the viewer
does), OpenJPEG reading them through a memory stream and neo-j2k through `nj2k_decode_memory` into int32
planes the caller allocated (the allocation outside the timing). Every decode is identical to OpenJPEG's.
Stage 1c's review round ran it again with its changes (the stream's bytes taken in one allocation when the
stream gives its length, the limits' estimate kept with the header), under a loaded machine (total CPU 36 %
mean, 72 % at most, sampled every 5 s; other sessions' builds), so the absolute times are a few per cent
noisier than the first run's.

| Image | OpenJPEG ms / MB | neo-j2k ms / MB | ratio | OpenJPEG from memory ms / MB | neo-j2k from memory ms / MB | ratio |
|---|---|---|---|---|---|---|
| 4,096², one tile, 9/7 | 401 / 202 | 348 / 200 | 0.87 | 409 / 205 | 351 / 200 | 0.86 |
| 4,096², 16 tiles, 5/3 | 3,325 / 209 | 3,293 / **238** | 0.99 | 3,407 / 248 | 3,342 / 245 | 0.98 |
| 8,192², one tile, 9/7 | 1,624 / 802 | 1,540 / 791 | 0.95 | 1,630 / 813 | 1,548 / 792 | 0.95 |
| 16,384², 256 tiles, 9/7 | 5,563 / 3,096 | 5,052 / **3,131** | 0.91 | 5,662 / 3,134 | 5,251 / 3,130 | 0.93 |

neo-j2k is faster everywhere and needs no more memory from memory. Through the stream API it holds the whole
codestream in memory before it decodes (the walk over the tile-parts reads the bytes in place), where
OpenJPEG holds one tile's data at a time: a multi-tile image then peaks higher by about the size of its
codestream (29 MB and 35 MB above; taking the stream's bytes in one allocation did not change the peak,
which the decode sets). Reading tile-parts from the stream as the walk reaches them, and dropping a tile's bytes
once it is decoded, would close that; it changes the stream input layer and is left for a later stage. Two
fixes came from these runs: the 9/7 Tier-1 store is vectorised (OpenJPEG's compiler vectorises its loop; 6 %
of the 8,192² decode), and clearing the caller's planes for a multi-tile image goes a row at a time (a memset
per sample had cost a second at 16,384²).

## Stages 2-4 (2026-11-20)

Every stage's build of the shared library is benched against the others in one interleaved run (the bench's
`neolib:` backend loads another build of `neoj2k.dll` and runs neo-j2k's memory path through it): `main` is
stage 1c's follow-up (218b0fa), `stage 2` the fused output pass (a4a3d5c), `stage 3` the row-wise wavelet
(c081da9), `stage 4` the MQ renormalisation with stage 2's follow-up (4cb2d40). MSVC 14.51 RelWithDebInfo,
pinned to core 7 of the Ryzen 9 9950X3D, the private bench corpus (212 files), the viewer's request on
neo-j2k's own path (one codec, the Second Life limits, the decode into the viewer's 8-bit bottom-up buffer).
The review round ran every gate again on the pull request's head after its fixes (`final`: one copy of the
colour transform kernels, comments, tests; no change to what the decoder computes), against `main` and the
oracle, on a quiet machine; the first run's per-stage figures follow each table.

### The kernel gates (`nj2k_kernels`, one plane or image, median of interleaved rounds)

Each round copies the input planes before it runs the kernel (the kernels work in place); the bench prints
that copy's time, timed alone with warm caches. The net times below leave it out; the bench's own ratio (gross)
includes it. The bench never fails on a time, so the gates are judged from these figures. The review round's
run (101 rounds at 1024², 21 at 4096²; the first run's net gain in brackets):

| Kernel | Size | Before (stage 1c), net | After, net | Gain net (first run) | Gain gross | Plan gate |
|---|---|---|---|---|---|---|
| Output tail: ICT, DC shift, 8-bit interleaved bottom-up copy (RGB 9/7) | 1024² | 1,171 µs | 442 µs | **2.6×** (2.9×) | 1.85× | ≥ 5× at 1024² |
| | 4096² | 24.2 ms | 7.9 ms | 3.1× (3.4×) | 2.12× | |
| Inverse 9/7, 5 levels | 1024² | 2,454 µs | 583 µs | **4.2×** (4.3×) | 3.60× | ≥ 5× at 1024² |
| | 4096² | 43.4 ms | 10.5 ms | 4.1× (4.2×) | 3.59× | |
| Inverse 5/3, 5 levels | 1024² | 994 µs | 409 µs | 2.4× (2.3×) | 2.07× | |
| | 4096² | 18.7 ms | 6.4 ms | 2.9× (2.6×) | 2.43× | |

The output tail takes the same time with the review fixes as without them (848-856 µs gross for both builds,
run in turn), so its lower net gain here comes from this run's copy time, not from the fixes. The gross ratio
understates the kernels; the net one, whose copy is timed with warm caches, slightly favours them.

Neither gate's factor is reached against stage 1c, which already had OpenJPEG's SIMD transforms and its own
AVX2 DC shift (the plan's factors were measured against OpenJPEG's scalar tail, 4.2-4.4 ms at 1024², which the
fused pass beats by about 11×). The 9/7 vertical pass alone went from OpenJPEG's 8-column strips to whole rows:
about 0.17 ns a sample against 1.2 (a profiled build's split); what is left of the transform is the horizontal pass, already on rows in the
first caches. Small levels keep OpenJPEG's structure, which is faster there (9/7 up to 64², 5/3 up to 256²,
measured with the same bench): the row-wise transform takes over from 128² (9/7) and 512² (5/3) samples a
level. The thresholds are by a level's area and were measured on square levels only, under load; thin levels
(a few rows of thousands of samples, or the reverse) were not measured on their own.

What else the plan lists for stage 3, and why it was not done: **int16 lanes for the 5/3** (twice the samples
an instruction "where the header's bounds allow") were not tried. The header bounds the image's precision, not
the coefficients a damaged or crafted code-block decodes to (up to 31 bit-planes), and D2 requires OpenJPEG's
int32 wrap-around on those too, so int16 lanes would need a range proof per tile-component from the decoded
bit-planes and a second set of kernels, on a transform already 2.4-2.9× faster than stage 1c's. Left for a
later stage, with that proof.

### Small images: the viewer's request

The review round: 1,437 configurations in 166 s (the 14-minute box not reached), load from other processes 4 %
mean, 7 % p90, 14 % max of 8 CPUs. Every decode of every decoder is identical to the oracle's in every class.
Medians per file, then over the class; `final / main` is the ratio of the class medians.

| Request | Files | Oracle µs | main µs | final µs | final / main | final / oracle |
|---|---|---|---|---|---|---|
| 32² whole | 28 | 34.1 | 9.8 | **8.1** | 0.83 | 0.24 |
| 32² at reduce 3 | 28 | 26.3 | 3.9 | 4.0 | 1.02 | 0.15 |
| 32² at reduce 5 (1x1 floor) | 28 | 22.9 | 3.05 | 3.10 | 1.02 | 0.14 |
| 64² whole | 32 | 373 | 345 | **316** | 0.92 | 0.85 |
| 64² at reduce 2 | 32 | 44.8 | 22.6 | 20.0 | 0.89 | 0.45 |
| 128² whole | 32 | 614 | 556 | **491** | 0.88 | 0.80 |
| 128² at reduce 3 | 32 | 39.5 | 16.7 | 16.4 | 0.98 | 0.41 |
| 128² at reduce 5 | 32 | 27.2 | 6.43 | 6.54 | 1.02 | 0.24 |
| 256² whole | 40 | 2,227 | 2,084 | **1,839** | 0.88 | 0.83 |
| 256² at reduce 3 | 40 | 151 | 120 | 116 | 0.96 | 0.77 |
| 256² at reduce 5 | 40 | 35.8 | 12.4 | 12.6 | 1.02 | 0.35 |
| 512² whole | 40 | 11,376 | 10,352 | **8,966** | 0.87 | 0.79 |
| 512² at reduce 2 | 40 | 1,626 | 1,528 | 1,438 | 0.94 | 0.88 |
| 1024² whole | 40 | 37,176 | 34,500 | **26,158** | 0.76 | 0.70 |
| 1024² at reduce 1 | 40 | 13,252 | 12,101 | 10,709 | 0.88 | 0.81 |
| 1024² at reduce 3 | 40 | 1,552 | 1,426 | 1,360 | 0.95 | 0.88 |
| 1024² at reduce 5 | 40 | 302 | 216 | 211 | 0.98 | 0.70 |
| 1024², first 600 bytes | 34 | 68.3 | 23.9 | 24.4 | 1.02 | 0.36 |
| 512², first 600 bytes | 38 | 38.9 | 12.8 | 12.9 | 1.00 | 0.33 |
| 256², first 600 bytes | 38 | 26.7 | 6.51 | 6.68 | 1.03 | 0.25 |
| 128², first 600 bytes | 24 | 25.0 | 4.70 | 4.83 | 1.03 | 0.19 |
| 64², first 600 bytes | 29 | 24.2 | 4.55 | 4.41 | 0.97 | 0.18 |

The whole decodes gain 8-24 % over stage 1c, most at 1024² (the wavelet: about 20 % of the request there;
Tier-1's renormalisation 5-7 % from 64² up). **The requests whose output is a few pixels** (the floor, reduce
4-5 of the small sizes, the first 600 bytes) **are 1-3 % slower than stage 1c**: 0.05-0.17 µs on requests of
3-25 µs, with the same sign in nearly every such class (the 64² ones are 2-4 % faster). That is a fixed cost,
not noise: in quiet interleaved runs two copies of one build agree within 0.4 % on these classes, and the sign
holds from run to run (+0.9 to +1.2 % in one, +1 to +3 % here; the first run's +1 to +6 % was under load).
Where it comes from was narrowed down but not found. With stage 1c's Tier-1 (the renormalisation taken out)
these classes are as slow, so it is not stage 4. With every tile finished in place and then copied, as stage
1c did (through the same write path), they are slower still (+1-4 %), so it is not the fused arithmetic. The
stage 2 build alone already shows it, and stage 3 adds nothing. What is left is the write path's fixed work
per tile (preparing up to four components, matching the planes, a row loop over a few short rows) or code
layout: a few hundred cycles. It is accepted as measured, against the 8-24 % gained on every decode with more
than a few pixels; a fast path to buy it back would need a profile first. (The profile came later: see "The
owner's three items" below, which also found Tier-1's part.)

Heap calls after warm-up, per request: none up to 512² but in one file, and one allocation and one free at
1024² whole (the 8 MiB the codec keeps, as in stage 1c). The one file is a 32² RGBA texture whose whole decode
makes one allocation and two frees (4,096 bytes) a request in `main` and in every stage build alike. It comes
from stage 1c, not from these stages, and lies outside the zero-heap test, which runs on the generated corpus;
it is reported for a fix of its own (fixed below, "Zero heap after warm-up").

The first run (under load: other processes 26 % mean, 53 % p90, 88 % max), each stage's build:

| Request | main µs | stage 2 µs | stage 3 µs | stage 4 µs |
|---|---|---|---|---|
| 32² whole | 9.9 | 8.2 | 8.2 | 8.2 |
| 64² whole | 366 | 363 | 368 | 336 |
| 128² whole | 573 | 567 | 547 | 507 |
| 256² whole | 2,101 | 2,033 | 1,949 | 1,823 |
| 512² whole | 10,588 | 10,722 | 9,653 | 9,017 |
| 1024² whole | 34,550 | 34,407 | 27,447 | 25,921 |
| 1024² at reduce 1 | 12,155 | 12,091 | 11,324 | 10,662 |

Only whole decodes are listed per stage. The stage 2 and stage 3 builds were made before stage 2's follow-up
(4cb2d40, in the stage 4 build only), which took a fixed cost out of the single tile's write path (its clip
rectangles, and a 4 KiB stack buffer under the stack probe). On the few-pixel requests those two columns carry
that cost and are not comparable (128² at reduce 3, for example: 28.1, 33.9, 36.1 and 27.1 µs).

### Large images: time and peak memory against OpenJPEG, one thread

The images and the method of stage 1c (a process a decode, the decoders in turn, three rounds, two for 16,384²;
the median time and the highest peak commit of the rounds). The review round, under a load of 15 % mean and 20 %
max (total CPU, sampled every 5 s). From memory (the viewer's way: OpenJPEG through a memory stream, neo-j2k
through `nj2k_decode_memory` into int32 planes):

| Image | OpenJPEG ms / MB | main ms / MB | final ms / MB | final / OpenJPEG |
|---|---|---|---|---|
| 4,096², one tile, 9/7 | 390 / 205 | 348 / 200 | **242 / 200** | 0.62 |
| 4,096², 16 tiles, 5/3 | 3,135 / 248 | 3,118 / 246 | **2,938 / 244** | 0.94 |
| 8,192², one tile, 9/7 | 1,618 / 813 | 1,532 / 792 | **996 / 792** | 0.62 |
| 16,384², 256 tiles, 9/7 | 5,700 / 3,134 | 5,107 / 3,131 | **3,496 / 3,130** | 0.61 |

Through the stream API (two rounds):

| Image | OpenJPEG ms / MB | main ms / MB | final ms / MB | final / OpenJPEG |
|---|---|---|---|---|
| 4,096², one tile, 9/7 | 389 / 202 | 343 / 200 | 238 / 200 | 0.61 |
| 4,096², 16 tiles, 5/3 | 3,145 / 209 | 3,123 / 238 | 2,942 / **238** | 0.94 |
| 8,192², one tile, 9/7 | 1,622 / 802 | 1,537 / 791 | 997 / 791 | 0.61 |
| 16,384², 256 tiles, 9/7 | 5,678 / 3,096 | 5,022 / 3,131 | 3,399 / **3,131** | 0.60 |

The first run, from memory, per stage (load 26 % mean, 96 % max), in ms for main, stage 2, stage 3 and stage
4: 4,096² in one tile 345, 341, 250, 240; in 16 tiles of 5/3 3,140, 3,147, 3,098, 2,939; 8,192² 1,539, 1,508,
1,006, 983; 16,384² 5,149, 5,144, 4,457, 3,525; the peaks as in the review round.

Every decode is identical to OpenJPEG's. The stages leave the memory unchanged: the wavelet's scratch is
stage 1c's (eight floats a sample of the longest side for the 9/7, sixteen columns for the 5/3), since the
levels below the row-wise thresholds keep OpenJPEG's structure and need it; the row-wise path needs less (a row
and one bit a row) and works inside it, and the fused pass needs a 2 KiB stack buffer. The stream API's higher peak on multi-tile images is stage 1c's (it holds the codestream; the
low-memory stream belongs to the j2k-tile module, owner 2026-11-20); the memory API meets the gate.

### Tier-1 (stage 4): what was tried

Each candidate against the one before it, interleaved, the 212 files whole (the Tier-1-bound classes):

| Candidate | 64² | 128² | 256² | 512² | 1024² | Kept |
|---|---|---|---|---|---|---|
| MQ renormalisation by count-leading-zeros, against stage 3 | 0.94 | 0.93 | 0.93 | 0.94 | 0.95 | yes: the same byte reads at the same shifts |
| Eight empty columns skipped at once (significance and refinement passes, AVX2), against the above | 1.015 | 1.004 | 1.011 | 1.021 | 1.012 | no: the per-column test is one load and a branch already |
| The LPS/MPS choice by selects instead of branches, against the renormalisation | 1.033 | 1.026 | 1.047 | 1.045 | 1.047 | no: the symbol is branched on right after, so the misprediction only moves |

The renormalisation takes 5-7 % off the request where Tier-1 is 65-86 % of it, so about 6-8 % of Tier-1: below
the plan's gate (kept only at 10 % or more of Tier-1, section 3.2), so by the plan it would be closed with these
numbers. It is kept under the brief's rule instead (a change stays when its bench shows it faster and
bit-exact), which this one is on every class from 64² up; the owner may close it by the plan's gate.

What else stage 4 lists, and what became of it:
- **State words with column skipping, the MQ decoder in locals**: already in OpenJPEG's Tier-1 (its PR #783
  and #945), which neo-j2k ported, so they were not candidates. The AVX2 skip of eight empty columns at once
  was tried on top (above) and dropped.
- **SIMD context formation**: not tried beyond that skip. Within a code-block every decision changes the
  flags the next sample's context is formed from (the stripe's neighbours), so the contexts of one block are
  formed one after another; across code-blocks it is the lane prototype below, which also leaves context
  modelling out and shows how little room there is for it.
- **Branch layout**: the select-based LPS/MPS choice above is the branch-layout candidate (3-5 % slower).
- **BMI2**: not tried; no hand-written BMI2 code. The AVX2 build's floor (x86-64-v3, checked by
  `nj2k_cpu_supported`) includes BMI2 and LZCNT, so it would need no new check; the renormalisation's lzcnt is
  the one such instruction used by hand.

**The lane-per-code-block prototype** (`bench/nj2k_mq_lanes.cpp`): a copy of the library built with Tier-1's
trace hook (`NJ2K_T1_TRACE`, compiled out of the library) decodes the 212 files and records every MQ segment
(27,093) and decision (218 million); the decisions are then decoded again from the bytes, with the contexts as
recorded, by the library's scalar MQ decoder and by eight AVX2 lanes, a code-block each, every lane taking the
next segment of any image when its own ends (cross-image batching):

| Segments | Decisions | Scalar ns a decision | 8 lanes ns a decision | Gain | Identical |
|---|---|---|---|---|---|
| all 212 files | 218,340,970 | 4.83 | 3.42 | 1.41× | yes |
| images up to 128² | 3,160,186 | 4.88 | 3.38 | 1.44× | yes |

Context modelling is left out of both: the lanes are branch-free where the scalar decoder mispredicts its
LPS/MPS branch, which is most of its 4.8 ns. In the decoder, a decision costs about 6.8 ns of Tier-1 (64²
lossless: 336 µs for about 38,000 decisions, 86 % Tier-1), so context modelling and pass control take about
2 ns of it. A lane Tier-1 would add to the lanes' 3.4 ns its context modelling with gathers of the flag words
and the context tables, the neighbours' flag updates written back a lane at a time (AVX2 has no scatter), and
lanes idling where their code-blocks' passes diverge (a sample that needs no decision in one block needs one in
another). To win it would have to fit all that under about 3.4 ns, 1.7 times the scalar modelling it replaces;
the literature reports no CPU coder that vectorises Part 1 context modelling, and GPU coders reach 23 %
occupancy for the same divergence. Not adopted; the prototype and its numbers stay as the record. The
1.41× is an upper bound for lanes: the prototype's scalar replay looks the MQ states up by index where the
library follows pointers, and neither side forms contexts.

## Stages 2-4: the owner's three items (2026-11-20)

Three items the owner set before the merge, each on the pull request's branch as commits of its own:
zero heap calls after warm-up for every file of both corpora, the kernel gates of the plan, and the few-pixel
requests no slower than `main`. Same machine and method as above (MSVC 14.51 RelWithDebInfo, pinned to core
7, interleaved, time-boxed, the load recorded).

### Zero heap after warm-up

One 32² RGBA texture of the bench corpus made one allocation and two frees (4,096 bytes) on every repeat of
its request, in `main` and in every stage. A diagnostic build that records the stack of each allocator call
put it in the tile-part header arena (`Decoder::tile_arena`: the tile-part headers' COD, COC, QCD, QCC, RGN,
POC and PPT markers). The arena starts with a 2 KiB block and adds blocks as a decode needs them;
`Arena::reset` merges them into one block of their total, so that a decode needing no more fits it, but it
ran when the next decode started. A request whose tile-part headers held more markers than any request
before it grew the arena, and its first repeat freed the two blocks and took the merged one. The arena's
contents die with the decode, so the merge now happens at the end of it (`Decoder::trim_after_decode`, next
to the work arena's), and the repeat of any request finds memory it fits in.

`decode_unit` checks it per request, as the bench counts it: the viewer's request through the bench's own
backend (one codec kept across every request, the Second Life limits), every file up to 512², at reduce 0-5,
whole, cut to the bytes the viewer asks for at that level, and the first 600 bytes; each request once, then
again: the repeat must make no allocator call and no `operator new`. Every file is gated, the DP-3 file
`edge_ppt_empty_tiles_97.j2k` included (the reference tests leave it out because the oracle refuses it; this
gate compares with no oracle), and the gate fails when a file's whole request is refused at any level, so it
cannot pass on requests that never decoded; the one exception is `sub_420.j2k`, whose components differ in
size and which the viewer refuses before the decode. On the generated corpus in CI (59 files, 527 requests,
513 decoded: the 6 of `sub_420.j2k` and 8 cut requests refused) and with `--heap-files <list>` on the bench
corpus locally (172 files, 1,623 requests, all decoded), both builds: no heap call. Without the fix it fails
on both corpora (`edge_ppt_empty_tiles_97.j2k` first, then `packed_ppt_tiles.j2k`, and a 128² RGBA texture:
which file shows it depends on what was decoded before).

512² decodes make none already. 1024² whole decodes make one allocation and one free a request (the
12-16 MiB of the tile's samples, above the 8 MiB a codec keeps by default). They can make none with
`nj2k_decoder_set_retained_bytes` at 17 MiB or more: measured on 12 textures of 1024² in a probe of its own
(two codecs, the default and 64 MiB retained, interleaved through the static library; 30.8 ms a decode in
that run, not the bench's 26.1 ms of the table below), that saves 1.3 ms a decode (4 %, the page faults of
fresh memory) and keeps 16.3 MiB per codec instead of the 8 MiB limit. Not
without cost, so the default stays; the viewer may raise it for its decode threads.

### The kernel gates

`nj2k_kernels` (101 rounds at 1024², 11 at 4096², pinned to core 7). Until the review round it timed each
kernel with a copy of its input and subtracted a copy timed apart (about 22 % of the 9/7's time and 62 % of
the output tail's), so a 10 % error in the copy moved the 9/7 ratio by about 0.15. It now restores the input
before every call untimed, at a 64-byte boundary as the decoder places each component's samples, and reports
net and gross (the restore added to both) directly. Six runs each:

| Kernel | Size | Stage 1c | Before the owner's items | Now | Net, median (range) | Gross | Plan gate |
|---|---|---|---|---|---|---|---|
| Output tail (RGB 9/7, 8-bit interleaved, bottom-up), quiet (load 1-9 %), unaligned planes | 1024² | 1,088-1,092 µs | 442-452 µs (2.5-2.6×, old method) | 215-224 µs | **4.98× (4.88-5.07×)** | 2.36-2.40× | ≥ 5× |
| same, planes 64-byte aligned (the committed bench), three runs, quiet | 1024² | 1,108-1,109 µs | | 217-220 µs | **5.12× (5.05-5.12×)** | 2.41-2.42× | |
| same, aligned, the owner's viewer running (load 9-27 %) | 1024² | 1,137-1,190 µs | | 232-267 µs | 4.70× (4.26-4.93×) | 2.27-2.40× | |
| | 4096² | 23.1 ms | | 4.8 ms | 4.84× | 2.50× | |
| Inverse 9/7, 5 levels, quiet, unaligned | 1024² | 2,413-2,536 µs | 583 µs (4.2×, old method) | 466-478 µs | **5.21× (5.18-5.35×)** | 4.21-4.33× | ≥ 5× |
| same, aligned, three runs, quiet | 1024² | 2,458-2,517 µs | | 462-467 µs | **5.32× (5.28-5.43×)** | 4.25-4.40× | |
| same, aligned, the owner's viewer running | 1024² | 2,390-2,522 µs | | 468-477 µs | **5.22× (5.07-5.34×)** | 4.14-4.34× | |
| | 4096² | 43.1 ms | | 8.1 ms | 5.32× | 4.34× | |
| Inverse 5/3, 5 levels, aligned, the owner's viewer running | 1024² | 811-828 µs | | 403-409 µs | 2.02× (1.98-2.05×) | 1.73-1.78× | |

The output tail's floor at 1024² (`nj2k_kernels --only floor`, same runs): its three int32 planes read and
nothing else, 203-206 µs quiet (220-225 under the viewer's load); read with the 3.1 MB of pixels' bytes
written by plain stores and one xor a sample, 207-209 µs quiet. The 5× budget is 218-222 µs. The pass now
runs 3-8 % above the floor of its own memory traffic, so 5× is met in quiet runs at the median and not in
every run, and it drops when another process shares the L3 and the memory bus. With the old method, on the
previous head, the 9/7 read 4.94-5.19× and the output tail 4.24-4.33×. A clang-cl build, one run: the output
tail 4.92×, the 9/7 4.18× (clang compiles stage 1c's reference faster); the gates are taken on MSVC as all
of this document's.

**Output tail.** The fused pass of stage 2 still went through a stack buffer: the transform into it, each
component's shift there, then the store of the low bytes, three trips through L1 a sample. For the viewer's
image (1 to 4 components interleaved in 8-bit pixels, each finished into [0, 255]) a sample now goes from its
source row to its byte in registers, sixteen pixels a step: the transform as before, the 9/7 rounding by
cvtps after min(2^30, v) (2^31 and above to the top, NaN to the bottom, as before), the shift in 16-bit lanes
(a saturating pack to int16 and a saturating add), then the bytes. 263 µs at the first review, then the
review round's changes, each bit-exact and each measured faster (unaligned planes, quiet):
- the samples stay in the pack's lane order, which the stores of three and four components take as they are
  (the per-component lane permute is gone; 263 -> 253 µs);
- the clamp to [0, 255] is the unsigned saturating pack to bytes (for 0 <= lo <= hi <= 255 and a shift below
  2^15, a saturating pack, a saturating add and that pack equal the 64-bit add and clamp); a component whose
  range is narrower (fewer than 8 bits) clamps the bytes after the pack, in instances of its own, so the
  viewer's 8-bit images skip the 16-bit max and min (253 -> 230 µs);
- one call per region, the rows looped inside, with the row pointers in locals (the byte stores could alias
  the caller's pointer array, which made every step reload it; 230 -> 217 µs).

Measured and dropped: the same pass in 32-bit lanes (387 µs); non-temporal stores of the pixels (the floor's
read and write went from 222 to 249-250 µs with them); software prefetch of the planes 128, 256 or 512 samples
ahead (233-237 µs, slower); joining each lane's 12 bytes with a cross-lane permute to store 24 at a time (no
change). Not tried: fusing the output pass into the last wavelet level, finishing each row while it is in
L1 or L2. The kernel gate is the isolated pass, which that cannot change; in a decode it could save at most
the pass's own traffic from the L3, about 200 µs of a 1024² RGB decode's 25.4 ms (under 1 %), and it needs
the three components' last levels run row by row together, which the decode does one component at a time
(a change to `Tile::decode_samples` and the wavelet's interface, not a kernel's). In whole decodes the
review round's output changes are inside the noise: 1024² at reduce 0, 25.37 ms against 25.45 for the
previous round's final library (40 files, one interleaved run under the viewer's load).

**9/7 wavelet.** Its parts at 1024², each alone after a copy: the horizontal pass 390 µs, the vertical
lifting 260, the pass that puts the rows in order 156 (a full read and write of the level from the L3); 630
fused. Two changes, each sample keeping OpenJPEG's operations in its order:
- where the vertical schedule gives each step exactly one interior target, the scaling of the two new rows
  and the four steps are one sweep over the columns with every value in registers (6 or 7 loads and 5 or 6
  stores for 8 columns of the two rows, against 14 loads and 6 stores in six passes); the schedule runs on
  after it for the edge rows;
- the horizontal lifting leaves each row's halves in place, and the row pass interleaves the columns as it
  copies every row (the vertical lifting treats each column alike), so one interleave and one memcpy a row
  fewer.

Measured and dropped (each bit-exact): the horizontal steps in registers with lane shifts (eight shuffles per
16 samples cost what the passes did), the scalings folded into the steps (more multiplies, slower), the step
loops unrolled by two (slower), software prefetch of the next rows (no change), row-wise thresholds from 32²
to 256² (no change). What is left of the top level is two passes over 4 MB of L3 (the lifting in place and
the row permutation, which a buffer the size of the level would avoid, and memory may not grow) and the
horizontal steps' arithmetic. With the input restored untimed the gate is met in every one of fifteen runs
(5.07-5.43×, median 5.2×, quiet and loaded); by the old subtracting method it was 4.94-5.19×.

### Few-pixel requests

Requests whose output is a few pixels were 1-3 % slower than `main`. A static build of each with rdtsc probes
around the tile's set-up, the packets, the samples and the hand-over (the minimum per file over thousands of
repeats, which load does not move) showed where:

- **The write path** (stage 2): about 260 cycles a 32² file at reduce 5 (1x1x4) against `main`'s 140 for the
  same work, and 480 against 430 at 128² reduce 5. About 160 cycles of bookkeeping before any sample moved
  (`hand_over_whole`'s check, then `write_tile`'s grouping through `Prepared` copies and the pairing checks),
  then a call per kernel and row into AVX2 kernels that only ran their scalar tails; and MSVC built the
  per-sample float bit casts (a memcpy of an int32 held in a register) out of byte stores on the stack, which
  the float load after them waited on. Now the viewer's single tile goes straight to one `write_group` pass,
  regions of up to four pixels go a pixel at a time with the per-sample operations inline (`mct.h`, the one
  copy the scalar kernels loop over, by `std::bit_cast`), and a region of up to 128 samples whose rows follow
  one another is finished as one run: 196 cycles at 1x1x4 (`main` 141), 419 at 4x4x4 (`main` 425).
- **Tier-1** (stage 4): about 600 cycles more than `main` at 256² reduce 5. With `main`'s `t1.cpp` in this
  build every few-pixel class is faster than `main`, but whole decodes lose stage 4's 5-7 %; and the same
  Tier-1 source moves by up to 1,000 cycles between builds (code layout). Keeping the counting
  renormalisation for whole decodes only was tried four ways and dropped, each slower in both regimes: the
  passes compiled twice and chosen by code-block size, the loop on the MPS path only, one counted step with
  the loop as fallback, the count in the 64x64 passes only.
- **The tile's set-up**: `Tile::init` called the C runtime's `ldexp` once per band for the step sizes (64
  calls for a 32² RGBA tile); the same double built from its exponent bits takes 300-500 cycles off every
  request.

The gate: no slower than `main` within the noise of two copies of one library (0.5 %), on a quiet machine.
The shared libraries of `main` (218b0fa) and of the final head, two copies of each, interleaved request by
request over the bench corpus (`--kinds full,first --reduce 3-5`, 801 configurations, about 56 s a run, pinned
to core 7): run 1 under 4 % mean load (14 % p90), run 2 in the opposite load order under 5 % (14 % p90), run 3
under 31 % (the owner's viewer running). Each ratio is the median over the class's files of the two copies'
sum against `main`'s two copies' sum; the copy column is `main`'s second copy against its first.

| Request | Files | main µs | final / main, run 1 | run 2 | run 3 (loaded) | main copy / main, run 1 | run 2 | run 3 |
|---|---|---|---|---|---|---|---|---|
| 32² at reduce 3 | 28 | 3.90 | 0.977 | 0.968 | 0.974 | 1.002 | 1.003 | 1.003 |
| 32² at reduce 4 | 28 | 3.45 | 0.983 | 0.982 | 0.983 | 1.003 | 1.000 | 1.006 |
| 32² at reduce 5 (1x1 floor) | 28 | 3.08 | 0.978 | 0.979 | 0.979 | 1.000 | 0.999 | 1.003 |
| 64² at reduce 5 | 32 | 5.55 | 0.972 | 0.965 | 0.974 | 0.992 | 0.999 | 0.996 |
| 128² at reduce 4 | 32 | 9.37 | 0.975 | 0.991 | 0.974 | 0.999 | 1.000 | 1.006 |
| 128² at reduce 5 | 32 | 6.48 | 0.991 | 0.991 | 0.993 | 0.997 | 1.002 | 1.004 |
| 256² at reduce 5 | 40 | 12.43 | 0.993 | 1.011 | 0.998 | 1.001 | 1.004 | 1.006 |
| 512² at reduce 5 | 40 | 63.06 | 0.968 | 0.978 | 0.970 | 0.997 | 0.996 | 0.996 |
| 32², first 600 bytes | 2 | 4.27 | 0.979 | 0.988 | 0.981 | 1.001 | 0.998 | 1.000 |
| 64², first 600 bytes | 29 | 4.55 | 0.966 | 0.955 | 0.970 | 0.995 | 1.002 | 0.996 |
| 128², first 600 bytes | 24 | 4.71 | 0.985 | 0.983 | 0.988 | 1.000 | 1.005 | 1.000 |
| 256², first 600 bytes | 38 | 6.51 | 0.991 | 0.999 | 0.994 | 1.005 | 1.006 | 1.006 |
| 512², first 600 bytes | 38 | 12.95 | 0.957 | 0.987 | 0.948 | 1.006 | 0.999 | 1.008 |
| 1024², first 600 bytes | 34 | 23.52 | 0.961 | 0.964 | 0.966 | 1.004 | 1.001 | 1.002 |

Twelve classes are 0.7-5.2 % faster than `main` in every run. 256², first 600 bytes, is 0.1-0.9 % faster
(within the copies' spread in run 2). 256² at reduce 5 is the exception: 0.7 % faster in run 1, 1.1 % slower in run 2 (where the two
copies of `main` differ by 0.4 %), 0.2 % faster under load in run 3. Four more runs of the 256² files alone, at
reduce 5 in alternating orders (16-31 % load, the viewer running), gave 0.992-0.995 for whole requests and
0.989-0.994 for the first 600 bytes, with `main`'s copies 0.2-1.1 % apart. Over the seven runs its median is
0.995; one run of seven is outside the gate's 0.5 %, and the copies of `main` themselves differ by up to 1.1 %
in this class, so the gate holds at the median and not in every run. What is known of the class's cost is
above (Tier-1's renormalisation, about 600 cycles of code layout); no further change was found that removes
it. Every decode is identical to `main`'s, and no request makes a heap call after warm-up but the 1024² whole
ones (above).

### Whole decodes and large images

The interleaved bench of the final head against `main` and the oracle (both libraries twice; 1,437
configurations in 275 s under a load of 37 % mean and 84 % p90, so read the ratios, not the times):

| Request | oracle µs | main µs | final µs | final / main |
|---|---|---|---|---|
| 32² whole | 36.0 | 10.3 | **7.9** | 0.83 |
| 64² whole | 403 | 371 | **338** | 0.91 |
| 128² whole | 643 | 580 | **502** | 0.87 |
| 256² whole | 2,279 | 2,120 | **1,811** | 0.83 |
| 512² whole | 13,521 | 12,193 | **10,452** | 0.83 |
| 1024² whole | 38,390 | 35,171 | **26,093** | 0.77 |

The earlier quiet run of these commits (before the last one, which does not touch whole decodes beyond its
300-500 cycles: load 10 % mean) gave 0.885, 0.915, 0.870, 0.825, 0.846 and 0.760 for the same classes.

Large images, a process a decode (OpenJPEG, `main`, final; three rounds, two for 16,384², medians and the
highest peak commit; load 33 % mean, 100 % max, sampled every 4 s); every decode identical to OpenJPEG's:

| Image | OpenJPEG ms / MB | main ms / MB | final ms / MB | final / OpenJPEG |
|---|---|---|---|---|
| 4,096², one tile, 9/7, from memory | 438 / 205 | 403 / 200 | **271 / 200** | 0.62 |
| 4,096², 16 tiles, 5/3, from memory | 3,291 / 248 | 3,257 / 246 | **3,146 / 244** | 0.96 |
| 8,192², one tile, 9/7, from memory | 1,776 / 813 | 1,634 / 792 | **1,078 / 792** | 0.61 |
| 16,384², 256 tiles, 9/7, from memory | 6,406 / 3,134 | 5,370 / 3,131 | **4,465 / 3,130** | 0.70 |
| the same through the stream API | 406, 3,225, 2,387, 6,362 ms | 354, 3,205, 1,673, 5,974 | **237, 2,992, 1,016, 3,710** | 0.58, 0.93, 0.43, 0.58 |

Peak memory is unchanged by these commits (the wavelet's scratch and the stack buffer as before); the stream
API's higher peak on multi-tile images is stage 1c's (238 and 3,131 MB against 209 and 3,096: the
low-memory stream belongs to the j2k-tile module).

## Stage 5: threads in one image (2026-11-20)

The host's executor (`nj2k_decoder_set_executor`, `doc/api.md`): here the tests' pool
(`tests/common/thread_pool.h`), N - 1 workers started before the decode and the calling thread the N-th.
OpenJPEG gets `opj_codec_set_threads(N)` (its pool starts inside the timed call, as a host that uses it
pays: a head start for neo-j2k of well under 1 % at these times). The Ryzen 9 9950X3D, with 8 logical CPUs
visible to this Windows session, so N = 8 is every core; MSVC 14.51, RelWithDebInfo. The numbers below are
the review round's (after the fixes of the stage's review).

### How to reproduce

`bench/nj2k_large.cpp` (built with the bench, `nj2k_large`) and its driver `bench/run_large.py`:

```
run_large.py --exe build/<preset>/bench/nj2k_large --files <dir> --gen          # the images, once
run_large.py --exe ... --files <dir> --rounds 3 --log mem.log                    # from memory
run_large.py --exe ... --files <dir> --decoders oracle,neo --rounds 2            # the stream API
run_large.py --exe ... --files <dir> --images g4096_97,o4095_97,g8192_97,o8191_97 --threads 1,4,8
run_large.py --exe ... --files <dir> --sat s1024_97 --hosts 8 --decodes 120     # the saturated host
nj2k_large split <file> <N> 60                                                   # where threads start
nj2k_large clear                                                                 # the plane clear
```

Each decode of the gate runs in a process of its own (its peak is the process's peak commit on Windows,
its peak resident set on Linux); the driver time-boxes the run (15 minutes by default), never waits for a
quiet machine, samples the total CPU load every 5 s and prints it with the table, and checks that every
decode of an image gave one hash. The small-image classes come from `nj2k_bench` (`--a
neolib:<library>=<name>`, one library a process, see below). `s1024_97` and `s2048_97` (one tile, 9/7)
come from `nj2k_large gen <out> <side> <side> 0 1`.

### Large images: time and peak memory against OpenJPEG at N threads (the gate)

Three rounds, two for 16,384², the thread counts and decoders taking turns in each round, the order
reversed every other round; medians, and the highest peak. One thread is pinned to core 7; at 4 and 8
threads nothing is pinned. Total CPU load 26 % mean, 55 % max, the decoders' own threads included. Every
decode at every count gave OpenJPEG's one-thread hash.

From memory (the viewer's way: OpenJPEG through a memory stream, neo-j2k through `nj2k_decode_memory` into
int32 planes):

| Image | N | OpenJPEG ms / MB | neo-j2k ms / MB | neo-j2k / OpenJPEG |
|---|---|---|---|---|
| 4,096², one tile, 9/7 | 1 | 392 / 205 | 234 / 200 | 0.60 |
| | 4 | 181 / 206 | 74 / 201 | 0.41 |
| | 8 | 136 / 207 | **46 / 201** | 0.34 |
| 4,096², 16 tiles, 5/3 | 1 | 3,203 / 248 | 2,948 / 244 | 0.92 |
| | 4 | 865 / 248 | 772 / 245 | 0.89 |
| | 8 | 488 / 249 | **418 / 245** | 0.86 |
| 8,192², one tile, 9/7 | 1 | 1,584 / 813 | 946 / 792 | 0.60 |
| | 4 | 780 / 815 | 337 / 793 | 0.43 |
| | 8 | 567 / 817 | **238 / 793** | 0.42 |
| 16,384², 256 tiles, 9/7 | 1 | 5,731 / 3,134 | 3,615 / 3,131 | 0.63 |
| | 4 | 2,768 / 3,135 | 1,152 / 3,131 | 0.42 |
| | 8 | 2,002 / 3,135 | **747 / 3,131** | 0.37 |

neo-j2k is faster at every count, and needs no more memory: the threaded tile's own memory (a Tier-1 scratch
a run, each with room for the largest code-block of its run, a wavelet line a part, 8 bytes a code-block) is
about a megabyte here. At 8 threads it decodes 4.0-7.1x faster than on one (OpenJPEG 2.8-6.6x). What stays
on the calling thread: the walk over the tile-parts and the packets (Tier-2, serial in OpenJPEG too), each
tile's layout and the wavelet levels below 2^18 samples; tiles are not decoded in parallel
(`doc/stages.md`).

Through the stream API (two rounds; load 26 % mean, 54 % max), neo-j2k / OpenJPEG at 1, 4 and 8 threads:
4,096² one tile 0.61, 0.42, 0.35 (48 ms at 8); 16 tiles 0.92, 0.90, 0.87; 8,192² 0.60, 0.43, 0.46; 16,384²
0.60, 0.40, 0.33 (703 ms against 2,152). The peaks are as from memory but for the multi-tile images: 238 and
3,131 MB against OpenJPEG's 210 and 3,097, at every count. That is stage 1c's stream input, which holds the
codestream (the low-memory stream belongs to the j2k-tile module, owner 2026-11-20), unchanged by threads:
**a known exception to the gate's "no higher peak" for the stream API's multi-tile images, accepted by the
owner (2026-11-20) for j2k-core**: the j2k-tile module's low-memory stream closes it, and the memory-buffer
API (the viewer's way, from memory), where the gate holds, is not affected.

Widths that are not a multiple of 16 (the wavelet's column stripes then share a cache line with their
neighbour where each row meets the next stripe), same method, three rounds:

| Image | neo-j2k ms at 1 / 4 / 8 | speed-up at 8 | neo-j2k / OpenJPEG at 8 |
|---|---|---|---|
| 4,096², one tile | 234 / 75 / 47 | 4.98x | 0.35 |
| 4,095 x 4,097, one tile | 237 / 74 / 49 | 4.84x | 0.41 |
| 8,192², one tile | 954 / 338 / 238 | 4.01x | 0.42 |
| 8,191 x 8,193, one tile | 967 / 342 / 234 | 4.13x | 0.48 |

The odd sizes scale as the even ones do (OpenJPEG's own times differ between them, hence the last
column): the shared lines cost nothing measurable, so the plan's 64-byte row padding stays undone.

With the first round's image guard (2^22 samples a component, since removed: see the saturated host; these images
are all above it, so the figures stand), the gate again at N = 1, 4 and 8 (`run_large.py --images g4096_97,g4096t_53,g8192_97 --threads 1,4,8
--rounds 2`; load 28 % mean, 51 % max; ms / MB), and one round for 16,384² (load 33 % mean, 58 % max), neo-j2k
against OpenJPEG:

| Image | N | OpenJPEG | neo-j2k | neo-j2k / OpenJPEG |
|---|---|---|---|---|
| 4,096², 1 tile, 9/7 | 1 | 399 / 205 | 233 / 200 | 0.58 |
| | 4 | 186 / 206 | 75 / 201 | 0.40 |
| | 8 | 136 / 207 | 47 / 201 | 0.34 |
| 4,096², 16 tiles, 5/3 | 1 | 3,204 / 248 | 2,944 / 244 | 0.92 |
| | 4 | 876 / 248 | 767 / 245 | 0.88 |
| | 8 | 489 / 249 | 425 / 245 | 0.87 |
| 8,192², 1 tile, 9/7 | 1 | 1,672 / 813 | 957 / 792 | 0.57 |
| | 4 | 794 / 815 | 348 / 793 | 0.44 |
| | 8 | 600 / 816 | 240 / 793 | 0.40 |
| 16,384², 256 tiles, 9/7 | 1 | 5,828 / 3,134 | 3,434 / 3,130 | 0.59 |
| | 4 | 2,848 / 3,135 | 1,206 / 3,131 | 0.42 |
| | 8 | 2,272 / 3,135 | 956 / 3,131 | 0.42 |

neo-j2k is as before: faster at every count with no higher peak, and every decode gave one hash. With an
executor lent the tile decides: the 16 tiles of 1,024^2 (1 M samples each) are threaded, and the tile floor
keeps many small tiles away from the executor.

### The saturated host

The viewer's case: many decodes at once. Eight host threads (every core) each decode the image again and
again through `nj2k_decode_memory`, each with a codec of its own, first each on its own thread and then all
lent one pool of 8; five rounds, taking turns, each run about 3 s; every decode gave the one-thread hash.

**With the first thresholds (tiles of 2^18 decoded samples over their components), the plan's "no throughput
loss with the pool saturated" was not met at 1,024^2** (the review round's numbers):

| Image | decodes a second, one thread each | the same, one shared pool | CPU ms a decode, one thread each / pool |
|---|---|---|---|
| 1,024² RGB 9/7, one tile | 322 | 300 (0.93) | 23.5 / 24.7 |
| 2,048² RGB 9/7, one tile | 73.2 | 71.7 (0.98) | 105 / 106 |
| 4,096² RGB 9/7, one tile | 15.2 | 16.3 (1.07) | 490 / 449 |

Splitting a 1,024^2 decode cost about 5 % more CPU (Tier-1's planning walk and each run's walk to its first
block, the threaded wavelet's extra passes, jobs running on other cores' caches) and 7 % of the throughput.

**The owner's call (2026-11-20), first round: an image guard of 2^22 samples a component** (a first-round
measurement, not reproduced in the second round below) (above
2,048 x 2,048), so that the viewer's textures up to 2,048^2 stay one thread each on a saturated host. With
that build the 1,024^2 and 2,048^2 rows never called the executor (the first two rows' ratios are
run-to-run spread; control: `nj2k_large sat <file> 8 100 1` twice, no executor, gave 259.6 against 252.1,
236.2 against 230.3 and 247.0 against 198.5 decodes a second), and 4,096^2 gained 5-6 %:

| Image | decodes a second, one thread each | the same, one shared pool | CPU ms a decode, one thread each / pool |
|---|---|---|---|
| 1,024² RGB 9/7, one tile | 307, 324, 269 | 299, 321, 257 (0.98, 0.99, 0.95) | 23 / 23 |
| 2,048² RGB 9/7, one tile | 71.2, 69.0 | 71.7, 69.7 (1.01, 1.01) | 105 / 105 |
| 4,096² RGB 9/7, one tile | 15.3, 13.4 | 16.2, 14.0 (1.06, 1.05) | 485 / 452 |

**The owner's call (2026-11-20), second and final: busy or idle is the host's decision, and the image guard
is removed.** "If busy, one thread; if idle, threads." The host lends the executor
(`nj2k_decoder_set_executor`) only when it has idle cores, for example when its decode queue holds fewer
jobs than it has workers, and lends nothing when the queue is backed up. The library cannot know what else
the host runs, so it keeps no guard of its own: with an executor lent, a tile whose largest component has
2^18 samples or more (`kParallelTileFloor`) is threaded, a 1,024^2 RGB texture included; with none, nothing
is. That is why the saturated-host measurements are kept: they are the cost the host rule avoids, and a host
that lends the executor to every decode on a saturated pool pays it (the first round's 7 % of throughput did
not reproduce; the 3 % of CPU below did). The 4 MiB plane clear is unchanged.

The saturated host again with the executor lent at 1,024^2 RGB 9/7 (`nj2k_large sat <file> 8 40 <1|8>`,
eight host threads, five interleaved pairs, MSVC RelWithDebInfo, other jobs sharing the machine):

| Run | decodes a second, no executor | the same, executor lent (pool of 8) |
|---|---|---|
| 1-5 | 280.7, 232.5, 259.6, 269.1, 261.1 | 279.7, 261.8, 266.0, 285.0, 254.8 (1.00, 1.13, 1.02, 1.06, 0.98) |

Throughput is within noise (median 261.1 against 266.0, ratio 1.02; the first round's 7 % loss did not
reproduce). The CPU cost did: 7,500 / 7,610 / 7,656 / 7,313 / 7,828 ms of CPU for 320 decodes without the
executor against 7,953 / 7,828 / 8,125 / 7,688 / 7,656 ms with it, +2.9 % on the medians (23.8 against
24.5 ms a decode; per pair -2 % to +6 %; the planning walk and the threaded wavelet's extra passes).
A saturated host has no idle core to spend it on, so the rule stands: do not lend the executor there.

### The idle host: one decode at a time

One host thread decoding the same RGB 9/7 one-tile image again and again (`nj2k_large sat <file> 1 <decodes> <N>`:
N = 1 lends no executor, N > 1 lends a pool of N - 1 workers and the calling thread, at the library's own
thresholds), three rounds with the two sides taking turns, other jobs sharing the machine. Every cell is
kept (decodes a second). The no-executor side ran in all of the N = 1, 4 and 8 sets (a median of 12 cells),
the lent side three times at each count (a median of 3):

| Image | no executor, all 12 runs | N = 4 lent, 3 runs | N = 8 lent, 3 runs |
|---|---|---|---|
| 512² RGB | 123.9, 173.4, 174.3, 174.5, 174.9, 175.7, 174.2, 142.9, 173.4, 174.3, 170.2, 174.1 | 248.1, 256.8, 254.6 | 277.8, 278.5, 279.7 |
| 1,024² RGB | 44.2, 44.3, 44.1, 39.6, 43.4, 43.7, 38.3, 42.5, 43.7, 13.8, 44.3, 44.3 | 65.4, 51.9, 47.7 | 68.6, 79.5, 74.8 |
| 2,048² RGB | 10.3, 8.8, 9.8, 9.8, 9.4, 9.9, 9.9, 8.4, 9.6, 9.7, 7.0, 7.1 | 15.5, 15.4, 16.8 | 19.0, 19.0, 17.4 |

| Image | no executor (median) | N = 4 lent | N = 8 lent | speed-up at 4 / 8 |
|---|---|---|---|---|
| 512² RGB | 174.2 | 254.6 | 278.5 | 1.46x / 1.60x |
| 1,024² RGB | 43.7 | 51.9 | 74.8 | not claimed / 1.71x |
| 2,048² RGB | 9.7 | 15.5 | 19.0 | 1.60x / 1.96x |

The slow cells of the no-executor side (123.9 and 142.9 at 512², 13.8 at 1,024², 7.0 and 7.1 at 2,048²) are
runs that another job disturbed; they are in the medians, which they barely move. 512² RGB has 2^18 samples
a component, the floor, so it is threaded. The 1,024² cell at 4 threads (47.7 to 65.4 in its three runs) is
too noisy to support a claim at 4 threads; the 8-thread cells are consistent. An idle host decodes a 1,024²
texture 1.7 times as fast with eight threads, and a 2,048² one twice as fast.

### Identity (the gate)

`decode_threads` in CI (both builds, every platform; `doc/testing.md`), and locally with `--files` on the
private bench corpus (212 files: 16,960 threaded decodes, at the library's thresholds at reduce 0-2 and with
every stage forced at reduce 0, into int32 planes with guard bytes and the viewer's 8-bit image, each codec
decoding twice) and the four large images (140 decodes), in the AVX2 and the scalar build: every decode at
2, 4, 8, 16 and 64 threads gives one thread's samples, status and messages, in order.

### Where threads start

These probes are from the first thresholds (a tile split from 2^18 decoded samples over its components: a
512 x 512 grey or a 296 x 296 RGB tile); the tile threshold is now 2^18 samples in the largest component,
with no image guard (the host decides; see the saturated host, above).

One codec decoding the same texture from memory again and again, one thread against N with every
stage forced (`nj2k_large split`, median of 60 each, taking turns, quiet machine; first run of the stage,
before the review fixes, which did not touch these paths): the time at 1 thread, then the time at N
threads as a fraction of it.

| Texture | 1 thread (µs) | 2 threads (time vs 1) | 4 (time vs 1) | 8 (time vs 1) | CPU efficiency at 8 |
|---|---|---|---|---|---|
| 128² RGB | 567 | 0.61 | 0.41 | 0.50 | 25 % |
| 256² RGB | 2,334 | 0.55 | 0.37 | 0.29 | 43 % |
| 512² RGB | 9,009 | 0.54 | 0.30 | 0.20 | 63 % |
| 1024² RGB | 36,116 | 0.52 | 0.28 | 0.15 | 83 % |

Threads shorten even a 128² decode, but at a high price in CPU time: below the threshold the cores do more
for a host that decodes many images at once (the viewer's case) as one image a thread, owner decision D8.

The plane clear (a multi-tile image's planes are cleared when its first tile does not cover them):
`nj2k_large clear`, a fresh plane each time (first touch, as a decode's new plane), one thread against 8
bands, medians:

| Plane | 64 KiB | 256 KiB | 1 MiB | 4 MiB | 16 MiB | 64 MiB |
|---|---|---|---|---|---|---|
| 1 thread (µs) | 0.3 | 1.5 | 91 | 361 | 1,268 | 4,246 |
| 8 bands (time vs 1) | 1.00 | 3.80 | 1.12 | 0.51 | 0.46 | 0.50 |

So a plane is cleared in bands from 4 MiB on (`kParallelClearBytes`). The clear stays at 4 MiB whatever the
tile threshold: it is memory bandwidth only, a few milliseconds that 8 bands halve from 4 MiB (a 16 MiB
plane, the one of a 2,048^2 image in several tiles, clears in 1.3 ms against 0.6), with none of the walks
and passes that splitting a decode adds. Only a multi-tile image has a plane to clear this way, and the
viewer's textures are one tile. The wavelet's level threshold (2^18
samples) and the output's (2^16 samples a component) are estimates, not probed on their own: they apply only
inside a tile already split.

### Small images: no executor, as the viewer decodes

The viewer's request on neo-j2k's own path through `main`'s and this stage's shared library (`main` is the tip
of the base branch, 6b6cc16, stage 2-4 merged). An interleaved bench with libraries of the same code in one
process cannot resolve these classes (a library's time in the reduced classes moved by up to 15 % from one
copy to another), so each library runs in a process of its own (`nj2k_bench --a neolib:<library>=<name>
--mode viewer --kinds full,budget,first --reduce 0-5 --pin 7`, the 32-256 px files then the 512 and 1024 px
ones, about 20 s each), the libraries in turn, three rounds (the corpus: the 188 real textures of the owner's bench corpus and the 5
generated `sl_*` files of the repository). Three
libraries besides stage 5, each built the same way (shared library only, `-DNJ2K_BUILD_STATIC=OFF`):

- **ctl**: `main` with only the walker as stage 5 first had it (`decode_blocks` over `for_each_cblk`, with the
  run counters and the refusal flag), no threaded code at all: does that work cost the reduced decodes?
- **pad**: `main` with one unused exported function of 14 instructions added in `codec.cpp`. Its machine code
  is `main`'s function by function (compared from the link map and the disassembly, addresses of data and
  jumps normalised; the ones that differ are `nj2k_status_string` and the CRT's start-up code); only the
  places of the functions after it move.
- stage 5: the one-thread Tier-1 walker is now `decode_blocks` with no counters (the same `for_each_cblk`
  walk, `decode_store` for each block), and the threaded runs use `decode_run`, a second function that has the
  counters. The Tier-1 kernel (`t1_decode_cblk`, `t1_store`, the passes) and the wavelet are `main`'s
  instructions with only the addresses of their tables changed; the output pass's `write_group` and
  `finish_in_place_mct` and the walker are code of stage 5 (the walker is a function of its own where `main`
  inlines the loop into `decode_samples`).

Ratios to `main` (median per file over the three rounds, then the range of the classes' medians):

| Request | ctl / main | pad / main | stage 5 / main |
|---|---|---|---|
| 32² whole and at reduce 1-5 | 1.005-1.009 | 0.986-0.991 | 0.991-1.001 |
| 64² at reduce 1, 2, 3 | 0.998-1.008 | 0.982-0.990 | 1.000-1.008 |
| 128² at reduce 2, 3, 4 | 0.987-0.990 | 0.986-0.992 | 0.995-0.998 |
| 64-512² whole | 0.996-1.011 | 0.993-1.005 | 0.998-1.010 |
| 1024² whole at reduce 0, 1, 2 | 1.021, 1.016, 1.016 | 1.006, 1.004, 1.003 | 1.010, 1.006, 1.007 |
| the viewer's budgets, 64-1024² | 0.985-1.024 | 0.982-1.005 | 0.991-1.021 |
| the viewer's budgets, 64 and 128² | 0.985-1.009 | 0.982-0.991 | 0.991-1.007 |
| first 600 bytes, 32-1024² | 0.987-1.013 | 0.983-0.996 | 0.989-1.007 |

What it shows:
- **Decision of the owner (2026-11-20): the one-thread walker loses its counters.** Reduced 64² and 128²
  decodes are 0.995-1.008 of `main`'s, inside what the `pad` control does to the same classes (0.982-0.992) and
  what the libraries' own spread is (about 1 %). The earlier round (against a stale `main`, the pre-stage-2
  code) is discarded.
- This round did not reproduce the first round's ~2 % for `ctl` on these classes (0.987-1.008): the counters'
  cost was a few tenths of a microsecond a request, near the layout noise, and it moves with the build.
  Removing them takes the cost out where it was measured; the control is no longer needed to show it.
- Whole 1,024² decodes are 1.003-1.010 of `main`'s for stage 5 and 1.003-1.006 for `pad`: code placement.
  The separate `decode_run` does not cost them what Tier-1 reached from two loops cost in the first round
  (1.0-1.5 %, an arrangement where the kernel was compiled for each caller): the kernel stays `main`'s.
- Gate 3 as written ("no slower than `main` within the noise of two copies of one DLL, 0.5 %") is met
  within the noise of this measurement for reduced 64 and 128 px decodes, and for the others within the
  placement effect `pad` shows (up to 1 % on 1,024² whole decodes here).

### Not measured

- The encoder (stage 6b uses the same executor).
- A Linux build's large-image times (CI runs the identity gate and ThreadSanitizer there; `nj2k_large`
  builds there too).

## Stage 6a: the upload encode (2026-11-20)

Correctness first: the encoder is OpenJPEG's operations on one thread, scalar, and stage 6b does its speed. This
is the baseline 6b starts from. `nj2k_encode_bench` (`bench/encode_bench.py` runs it, a fresh process for each
encoder and size so the peak is each one's own) times the viewer's upload encode, `JPEG2KEncode::encode`, of a
synthetic texture of three components (gradients, a few edges and noise): the oracle (OpenJPEG at the pinned
fork commit, MSVC 14.51 RelWithDebInfo, the same flags as neo-j2k) against neo-j2k through the compatibility
API, each timed over a first encode and then at least three, up to 1.5 to 2.5 seconds of them. One thread each;
a Ryzen 9 9950X3D, **not quiet**: other processes took 28 % of the CPUs on average while the sizes up to 512²
ran (the 1024² and 2048² runs under the same kind of load), so read the ratios, not the milliseconds. Every
codestream is identical to the oracle's (the checksum of the bytes is compared in the table).

| Size | Mode | Bytes | Oracle ms | neo-j2k ms | neo-j2k / oracle | Oracle peak MB | neo-j2k peak MB |
|---|---|---|---|---|---|---|---|
| 64² x 3 | lossy | 1,515 | 1.2 | 1.2 | 0.97 | 0.6 | 0.3 |
| 64² x 3 | lossless | 8,521 | 1.0 | 1.1 | 1.08 | 0.7 | 0.3 |
| 128² x 3 | lossy | 5,756 | 3.7 | 4.1 | 1.11 | 1.1 | 0.7 |
| 128² x 3 | lossless | 32,798 | 3.6 | 4.1 | 1.14 | 1.1 | 0.7 |
| 256² x 3 | lossy | 24,385 | 13.2 | 15.1 | 1.14 | 2.7 | 2.4 |
| 256² x 3 | lossless | 129,529 | 14.0 | 16.1 | 1.15 | 3.0 | 2.4 |
| 512² x 3 | lossy | 98,295 | 60.6 | 69.4 | 1.15 | 9.9 | 9.4 |
| 512² x 3 | lossless | 595,252 | 63.9 | 73.0 | 1.14 | 9.9 | 9.5 |
| 1,024² x 3 | lossy | 393,225 | 250.0 | 284.5 | 1.14 | 38.9 | 37.2 |
| 1,024² x 3 | lossless | 2,398,977 | 262.8 | 295.9 | 1.13 | 38.8 | 37.5 |
| 2,048² x 3 | lossy | 1,572,812 | 1,007.1 | 1,176.3 | 1.17 | 153.9 | 148.0 |
| 2,048² x 3 | lossless | 9,493,774 | 1,301.4 | 1,267.5 | 0.97 | 153.9 | 149.2 |
| 512² x 1 | lossy | 32,769 | 21.6 | 24.7 | 1.14 | 4.4 | 4.2 |
| 1,024² x 1 | lossless | 731,742 | 83.0 | 95.5 | 1.15 | 17.0 | 16.5 |

What the numbers say: neo-j2k takes 0.97 to 1.17 of OpenJPEG's time on one thread (most rows 1.11 to 1.17; 64² lossy 0.97, 64² lossless
1.08, and 2,048² lossless 0.97, a noisy median whose best times, 1,098 and 1,192 ms, give 1.09), as expected of the same algorithm written
straight, without 6b's work. Peak memory is the viewer's own buffers (for 2,048² x 3: the raw image 12.6 MB, the image of int32 planes
50 MB and the output's size guess 12.6 MB, 75 MB, which both encoders share) plus the encoder's: OpenJPEG's
is about 80 MB there (each code-block's output buffer of four bytes a sample, 50 MB, and the 100 passes and
100 layers it allocates for every one of them, 25 MB, the transform being done in the image's own planes);
neo-j2k's is about 73 MB (its own copy of the samples, 50 MB, which the transform works on, the code-blocks'
bytes as they come out, and the codestream buffer), so its peak is 0.3 to 6 MB lower, the copy being the price
of leaving the caller's planes alone (DP-19). Stage 6b can drop the copy for a caller that gives its planes up.
Stage 6b's levers, in the order the profile will say: Tier-1 (OpenJPEG's own runs the
code-blocks on its thread pool when it has one; here they are jobs for the host's executor), the vertical
wavelet pass, the rate allocation's Tier-2 simulations (each bisection step walks every packet header), and
the 9/7 quantization.

## Stage 6b: adaptive parallel encode (2026-11-22)

The encoder now borrows the host executor for its component wavelets and Tier-1 code-blocks. Tier-1 uses
one fixed scratch per concurrent job in batches of at most 64, then commits each batch to the arena in the
original code-block order. This preserves byte identity, deterministic allocation failures and support for
allocators that are not thread-safe, while bounding added memory by concurrency rather than code-block count.
Encoding has substantially more Tier-1 work per sample than decoding, so its measured crossover is lower:
2^16 samples, or 256 x 256. Uploads below that stay serial.

`nj2k_encode_bench --threads N` measured the synthetic three-component viewer upload on the Ryzen 9
9950X3D, Release, with other processes present. Medians in milliseconds; every thread count produced the
same byte count and checksum as one thread.

| Size | Mode | 1 thread | 2 threads | 4 threads | 8 threads | 8-thread speedup |
|---|---|---:|---:|---:|---:|---:|
| 128² | lossy | 4.10 | 4.11 | 4.10 | 4.10 | serial by threshold |
| 128² | lossless | 4.09 | 4.10 | 4.15 | 4.12 | serial by threshold |
| 256² | lossy | 15.41 | 9.21 | 5.80 | 4.12 | 3.74x |
| 256² | lossless | 16.16 | 9.28 | 5.59 | 3.97 | 4.07x |
| 512² | lossy | 70.74 | 39.85 | 24.82 | 16.64 | 4.25x |
| 512² | lossless | 72.90 | 40.43 | 23.40 | 14.54 | 5.01x |
| 1,024² | lossy | 285.82 | 162.37 | 92.51 | 62.43 | 4.58x |
| 1,024² | lossless | 295.45 | 164.21 | 88.92 | 52.57 | 5.62x |

At 1,024², 16 to 64 requested threads improved only modestly beyond eight (lossy 58.0 to 55.7 ms,
lossless 50.2 to 47.7 ms): the workload is already near the available-core and serial-stage ceiling.
At 256², 16 threads reached 3.93 ms lossy and 3.49 ms lossless. At 128² the executor was not called and
all requested counts stayed at about 4.1 ms, avoiding the known small-image multithreading loss entirely.

The SCALAR and AVX2 builds were also compared at 1,024². Their medians were within noise: 287.5 versus
285.3 ms for one-thread lossy, 61.0 versus 61.2 ms for eight-thread lossy, 297.7 versus 294.7 ms for
one-thread lossless, and 53.7 versus 55.2 ms for eight-thread lossless. Tier-1's MQ state is serial inside
each code-block and dominates the encode; the independent blocks, not wider instructions inside the MQ
state machine, are the profitable lanes. No encoder-only AVX2 path is kept without a measured gain.

## Stage 6c: the encoder's MQ coder (2026-11-22)

The encoder's MQ coder (`src/enc_mq.h`) gives OpenJPEG's output in a different formulation, in two steps after
the tests (each its own commit, followed by a review-fix commit): renormalisation by count-leading-zeros (`mq_shifts`, shared with the decoder through `t1.h`), then a
context's state as a byte index into a 376-byte compile-time table (`kMqEncStates`, `enc_luts.h`) with the
decision (interval, conditional exchange, state update) as selects. A decision has no data-dependent branch
left; the byte-out loop's compare and the byte-out itself are the only ones. The per-block context state
shrinks from 152 to 19 bytes (`EncT1` by 133 bytes); the one cost is 376 bytes of read-only data (the decoder
keeps its own 2,256-byte table). The gate of every step was exact (byte for byte), faster beyond the noise
(about 3 %) and leaner (fewer branches, less state and memory traffic, not fewer lines).

**Exactness.** `nj2k_enc_t1_unit` compares the coder with a frozen copy of the earlier operations
(`tests/unit/enc_mq_frozen.h`, `enc_t1_frozen.cpp`): random decision streams with pass ends at full and short
capacities (the output clamp at every byte position), one decision over every interval, state, bit, bit
counter, low word, previous byte and output limit (a pseudo-random combination of the last three in ctest, all
60 of them nightly: `enc_t1_exhaustive`, 4.4 billion decisions), and `enc_t1_encode` on 6,288 generated
code-blocks (every size 1..64, every orientation, 5/3 and 9/7: passes, distortion bit for bit, bytes). None of
the 6,288 overflows the driver's buffer (the test counts them), so the output clamp is tested through the
coder alone, not through the driver. `encode_unit` (against the oracle, every codestream byte for byte) and
the fuzz replays pass, and the codestream hashes of every measured configuration equal the oracle's. Run on
MSVC AVX2 (all tests), the MSVC SCALAR build and MSVC AddressSanitizer; `nj2k_enc_t1_unit` alone also on
MinGW GCC 13.2 and clang, with and without `-march=x86-64-v3`, and its full 4.4 billion decisions on MSVC and GCC.

**Method.** Ryzen 9 9950X3D, Windows 11, MSVC 14.51 (the library's flags), one thread pinned to CPU 7, TSC
cycles, interleaved runs, medians. The machine was not quiet. "Kernel" is `enc_t1_encode` over the code-blocks
of the viewer's upload encode (the synthetic texture of `nj2k_encode_bench`): the library's against the earlier
source compiled in its own object (which timed within 1 % of the earlier library). "Encode" is the whole
one-thread upload encode, the new library against the one at bee41d4 (5 rotations of 11 to 15 runs). The
harness is the study's `bench/t1_study`, on the `studies` branch (see `bench/STUDIES.md` there); the Linux
kernel bench, `bench/nj2k_enc_t1_ratio.cpp`, is in the repository.

AVX2 build, Mcycles and change against today's (minus is faster):

| Size | Mode | Today | Stage 2 | Encode, stage 2 | Kernel, stage 2 | Passes gate | Encode, stage 1 | Kernel, stage 1 |
|---|---|---:|---:|---:|---:|---|---:|---:|
| 256² | lossy | 64.5 | 42.3 | -34.4 % | -39.7 % | Yes | -13.5 % | -14.6 % |
| 256² | lossless | 68.5 | 41.7 | -39.1 % | -41.6 % | Yes | -14.4 % | -15.1 % |
| 512² | lossy | 293.5 | 189.6 | -35.4 % | -39.7 % | Yes | -13.4 % | -14.5 % |
| 512² | lossless | 306.9 | 187.5 | -38.9 % | -41.1 % | Yes | -14.5 % | -14.5 % |
| 1,024² | lossy | 1,201.4 | 774.0 | -35.6 % | -39.5 % | Yes | -14.0 % | -14.3 % |
| 1,024² | lossless | 1,246.6 | 760.7 | -39.0 % | -41.0 % | Yes | -14.3 % | -14.6 % |

SCALAR build (baseline x86-64, `bsr` for the count): at stage 2 the encode changes by -38.2 % to -32.9 % and the kernel
by -42.1 % to -40.0 % of today's (stage 1: encode -13.6 % to -11.5 %, kernel -15.5 % to -15.0 %). The study's targets, kernel
at most -38.0 % and encode at most -32.0 % against today's, hold on every configuration of both builds (nearest: SCALAR
256² lossy, encode -32.9 %). Stage 1 sits at its kernel gate of -15.0 % (-15.5 % to -14.3 %, within the 1 to 3 % that
code layout moves a kernel). The coder and the pass steps are force-inlined (`NJ2K_FORCE_INLINE`): without it
MSVC left the renormalisation and the decision as calls and stage 1 reached only -8.0 %.

Against OpenJPEG 2.5.4 (`nj2k_encode_bench`, 1,024² RGB, one thread, the same bytes): lossy 183.7 ms against
240.4 (-24.0 %), lossless 178.8 against 253.9 (-30.0 %). Before the redesign neo-j2k took 284.5 and 295.9 ms and was
13.0 % to 14.0 % slower.

### GCC and clang

The selects only pay when the compiler keeps them as `cmov`. MSVC and clang do; GCC turned them back into
branches (the select form: -5.0 % of today's kernel on a local MinGW GCC 13.2), so for GCC `encode` computes the
same function with arithmetic masks, and reads the next state as a byte of the table entry (`enc_luts.h`
asserts the layout) in place of a mask.

**Real Linux, GitHub Actions: the table of record.** `bench/nj2k_enc_t1_ratio.cpp` links the real library and
times the new `enc_t1_encode` against the frozen earlier one in one process, interleaved rounds, 256 synthetic
64 x 64 blocks a set, TSC cycles. A cell is the change in time, new against old (minus is faster, target -38.0 %), the median of the
rounds' ratios and then the median of 3 runs of 41 rounds, lossless / lossy, kernel only, on shared runners.
"Final code" is the confirmation run (45032049386) on the code of this branch; "before the byte load" is the
first run (the masks, with the next state by mask) on the same harness:

| Job | Before the byte load | Final code | Gain | Passes gate (final code) |
|---|---|---|---:|---|
| gcc-14, AVX2 | -41.2 % / -38.0 % | **-43.5 % / -40.1 %** | 3.9 % / 3.4 % | Yes |
| gcc-14, SCALAR | -30.7 % / -27.9 % | **-33.1 % / -30.6 %** | 3.5 % / 3.7 % | No |
| gcc-15, AVX2 | -32.3 % / -29.7 % | **-34.4 % / -31.5 %** | 3.1 % / 2.6 % | No (accepted) |
| gcc-15, SCALAR | -22.6 % / -20.3 % | **-24.7 % / -22.5 %** | 2.7 % / 2.8 % | No |
| clang, AVX2 | not run | **-38.9 % / -36.0 %** | the selects, unchanged | No |
| clang, SCALAR | not run | **-40.5 % / -38.0 %** | the selects, unchanged | Yes |

The byte load is 2.7 to 3.9 % lower than the masks on all four GCC jobs. Where the build ends up:

- GCC 14 AVX2 meets the -38.0 % target (-43.5 % / -40.1 %); clang is at it (-40.5 % to -36.0 %).
- **GCC 15, the compiler of the `linux-gcc` preset, AVX2 ends at -34.4 % to -31.5 %**, above the -38.0 % target; the
  owner accepted about -30.0 % or better.
- **GCC 15 SCALAR ends at -24.7 % to -22.5 %**, above the accepted -30.0 %, and GCC 14 SCALAR at -33.1 % to -30.6 %. No
  candidate gained there (below). The same masks cost 8 to 10 percentage points more on GCC 15 than on GCC 14.

**The candidates (relative ranking only).** The second CI run built 14 candidate formulations of the GCC path,
each its own program in one job per compiler and build, run 3 times each (run 45029986714). Those programs
compile their own copy of `enc_t1.cpp` and of the frozen reference, and their absolute SCALAR ratios came out
about 10 percentage points more negative than the single-bench runs above for the same code (the shipped form: gcc-14 SCALAR
-41.4 % / -38.4 %, gcc-15 SCALAR -35.9 % / -32.9 %, against -33.1 % / -30.6 % and -24.7 % / -22.5 % in the final run; the AVX2
jobs agreed to within 1 percentage point). The likely cause is that the variant programs are built with different
effective conditions from the library's (not proven), so **their absolute numbers are not comparable with the
table above and are kept only for the ranking inside that run**. In it the next-state byte load (variant 2) was
3.1 to 3.9 % lower than the masks (variant 0) on all four jobs; a single 4-byte table load unpacked by shifts was
level with it and was not kept; a direct `bsr` shift count for the SCALAR build added to the byte load gained
nothing over the byte load alone. Not better than the masks, or within the noise: the exchange decided from the
borrow of `a' - q`, a do-while byte-out loop with `__builtin_expect`, the byte-out loop out of line, the coder
in a local copy for the length of a pass, a forced-inline `byteout`, and the combinations of those.

On the local Windows machine (MinGW GCC 13.2, the Visual Studio clang with the MSVC ABI, 64 synthetic blocks,
kernel only, pinned, 31 interleaved rounds): GCC -5.0 % with selects, -31.5 % to -28.5 % with masks; clang -38.0 % to
-36.0 % with selects. GCC 13's new coder runs in about the same cycles as clang's (84 against 83 Mcycles), but
its earlier coder is 8 % faster than clang's, which is the ratio's denominator.

### Not measured, and the gaps

- **The Linux numbers are the kernel only**, from a shared GitHub runner, on synthetic blocks in a small
  harness. No GCC or clang whole-encode ratio exists. The sanitizer and fuzz presets run in the repository's CI,
  not in these benches; `nj2k_enc_t1_unit` ran locally on MSVC, MinGW GCC 13.2 and clang (the full 4.4 billion
  decisions on MSVC and GCC 13.2), and the Linux CI runs it on the `linux-*` presets.
- One local machine (Zen 5), one synthetic texture (gradients, edges, noise): real uploads and the corpus
  images were not timed (their bytes are pinned by `encode_unit`).
- No hardware counters: the branch and memory counts are derived from the algorithm.
- The MSVC kernel ratios are against the earlier source compiled in its own object, the encode ratios against
  the earlier library; the two agree to about 2 %.

## Stage 6d: the decoder's MQ decoder, MQ-NG (2026-11-22)

The decoder's Tier-1 (`src/t1.cpp`, `src/t1_mq.h`) decodes to OpenJPEG's coefficients with a different MQ decision, in
two steps after the tests (each its own commit). Stage 1: a context's state is one 32-bit word in its slot (the
interval's `qe`, the MPS, the index of the next state on an MPS and on an LPS), the 94 words built at compile time
from `kMqcStates` (376 bytes of read-only data; `MqContexts` is 76 bytes, where 19 state pointers were 152); the LPS
interval, the conditional exchange, the state change, `a` and `c` are selects; the renormalisation is always run (no
shift when bit 15 of `a` is set, the shifts at once when `c` has the bits, OpenJPEG's loop only when a byte must come
in: 0.07 to 0.11 a decision). The decision stays a macro in the passes, as OpenJPEG has it. Stage 2: the cleanup pass's
run of AGG decisions that are an MPS 0 without renormalisation is taken at once over the columns of an empty stripe
(`a -= n * qe`, `c -= (n * qe) << 16`, no division). This reverses stage 4's finding that select-based decisions and
empty-column skipping were slower: with the state word in the slot and the loop-free renormalisation the selects
win. OpenJPEG's 0xFF 0xFF sentinel write and restore stay (the owner's decision: a bounds-checked read-only input
measured within 0 to 2 %, so noise). The one addition is a length check: a segment longer than the bytes that are left
is cut to them, so the sentinel write cannot land past the buffer whatever Tier-1 is given (Tier-2 never gives such a
segment). The dead `end_of_byte_stream_counter` is gone. The `T1` scratch is 192 bytes, padded to whole cache lines as
`codec.cpp` asserts. The gate of every step was exact (coefficients), faster beyond the noise (about 3 %) and leaner.

**Exactness.** `nj2k_t1_unit` compares the library's Tier-1 with a frozen copy of the decoder before the redesign
(`tests/unit/t1_frozen.cpp`; coefficients and the refusal of a code-block) on: the 3,927 committed code-blocks of
`tests/vectors/t1` (3,777 of the generated corpus and 150 of the 256² study images, dumped from real decodes), 21,164
generated code-blocks (every size 1..64, the shapes 1024 x 4, 4 x 1024 and similar shapes, every style 0 to 63, bit-planes 0 to
31, ROI shifts, split chunks, all-0xFF, markers inside a segment, zero-length segments, 109 one-byte segments), 1,500
with over-long segments (against the frozen decode of the cut lengths), and, with the full dump set
(`NJ2K_T1_VECTORS`, test `t1_unit_more`), 37,424 code-blocks of 115 files, not committed, about 18 MB (the 35,390 mode-switch conformance blocks, which
contains the 3,777 generated ones, plus the 2,034 of the six study images; code-block styles 00, 01, 04, 05, 0a, 10,
12, 19, 20, 28, 2f, 34 and 3f). Single decisions are compared with a model of ISO 15444-1 C.3.2 written in the test,
in the frozen decoder and in the new one: 6.55 million in ctest (every 17th interval and, per state, the intervals
around the exchange and renormalisation edges), 887 million nightly (`t1_exhaustive`: every interval and state, `c_hi`
at `qe - 1`, `qe` and `qe + 1`, bit counters 0 to 8, next bytes from 00, 7F, 8F, 90, FE, FF). Each block is also
decoded on a buffer of exactly its bytes and two (the library's rounds up), so a write past the sentinel is the
sanitizers' to see, and `t1_unit_masks` runs the whole test with the mask form of the decision (GCC's) compiled in.
The state table is checked against Table C.2 written out in the test. The AGG run is checked against the decisions one
at a time (2.7 million cases, runs stopped by a column with flags). The new fuzz target `fuzz_t1` runs the library and
the frozen copy on any bytes; its seeds are real code-blocks. All of it passes on MSVC AVX2 (the whole ctest, 127
tests), MSVC SCALAR and clang-cl (`t1_unit` and the fuzz replay), and on MSVC AddressSanitizer (`t1_unit` including
the 37,424 code-blocks, the replay and 4,000 mutated fuzz inputs); `nj2k_t1_unit` also on MinGW GCC 13.2.

**Method.** Ryzen 9 9950X3D, Windows 11, one thread pinned to CPU 7, TSC cycles, medians of interleaved rounds. "Tier-1"
is `nj2k_t1_dec` (`bench/nj2k_t1_dec.cpp`): `t1_decode_cblk` of the library against the frozen copy built with the
same flags, over the dumped code-blocks of the author's synthetic texture (256², 512², 1,024², lossy and lossless; the viewer's
upload parameters; the two objects hold the same text before stage 1 and time -3.0 % to 0.0 % against each other), 9 to 11
rounds. "Whole decode" is the author's `mqng_profile` (7 decodes a run, 5 alternating runs of each library; the image
hashes are equal): the new library against the one at 41c4048. The harness around them (the profile and the
code-block dumper) is in the author's study notes, not in the repository.

Change against the earlier decoder, stage 1 / stage 2 (minus is faster). Tier-1 is against the frozen decoder, the whole decode against the library at 41c4048; the output is the same in both. Passes gate: stage 1 against the targets below, every size.

| Build, metric | 256² lossy | 256² lossless | 512² lossy | 512² lossless | 1,024² lossy | 1,024² lossless | Passes gate |
|---|---|---|---|---|---|---|---|
| MSVC AVX2, Tier-1 | -12.7 % / -15.4 % | -27.2 % / -27.9 % | -16.0 % / -21.6 % | -27.8 % / -28.6 % | -13.5 % / -20.7 % | -27.7 % / -29.0 % | Yes |
| MSVC AVX2, whole decode | -9.8 % / -12.5 % | -27.1 % / -27.2 % | -15.3 % / -20.2 % | -27.8 % / -28.5 % | -13.3 % / -19.9 % | -27.5 % / -28.7 % | Yes |
| MSVC SCALAR, Tier-1 | -13.2 % / -15.9 % | -27.1 % / -28.0 % | -15.9 % / -21.6 % | -28.0 % / -28.9 % | -14.4 % / -21.3 % | -28.1 % / -28.8 % | Yes |
| MSVC SCALAR, whole decode | -9.3 % / -11.7 % | -25.0 % / -25.6 % | -13.1 % / -18.2 % | -26.4 % / -27.5 % | -11.7 % / -17.2 % | -26.7 % / -27.9 % | Yes |
| clang-cl AVX2, Tier-1 | -13.1 % / -16.5 % | -22.4 % / -23.2 % | -12.2 % / -18.9 % | -21.9 % / -23.2 % | -10.2 % / -18.3 % | -21.4 % / -23.0 % | No (within noise) |
| clang-cl, whole decode | -10.8 % / -14.4 % | -22.7 % / -23.4 % | -12.5 % / -19.4 % | -22.9 % / -24.2 % | -11.3 % / -19.2 % | -22.5 % / -24.3 % | Yes |
| MinGW GCC 13.2, AVX2 flags, Tier-1 | -6.1 % / -11.9 % | -23.3 % / -24.3 % | -9.9 % / -19.1 % | -21.9 % / -23.2 % | -6.6 % / -18.3 % | -21.2 % / -22.6 % | No |
| MinGW GCC 13.2, no AVX2 flag, Tier-1 (stage 2 only) | -9.4 % | not run | -17.0 % | -21.4 % | -15.6 % | -21.3 % | n/a |

The targets (Tier-1 at most -22.0 % lossless and -9.0 % lossy, whole decode at most -21.0 % and -8.0 %) hold after stage 1 on
MSVC AVX2 and SCALAR; clang-cl's whole decode holds, and its Tier-1 lossless sits at -22.4 % to -21.4 % (up to 0.6
percentage points over -22.0 %, inside the 3 % noise band) until stage 2 takes it to -23.2 % to -23.0 %. Stage 2's gate (at least 3 % on lossy
at 512² and above in every build, at most 1 % slower elsewhere): lossy at 512² and above 7 to 12 % faster than
stage 1 (MSVC AVX2 6.7 and 8.3 %, SCALAR 6.8 and 8.0 %, clang-cl 7.6 and 9.0 %, GCC 10 and 12.5 %), 256² lossy 3 to 6 %,
lossless equal or 1 to 2 % faster. It stays.

### GCC and clang

GCC turns the select form back into branches (stage 1 with it: +2.0 % to +5.0 % against today's Tier-1 on lossy, -8.0 % to -7.0 %
on lossless), so for GCC the decision computes the same function with masks and a shift (the next-state index is
`(w >> (17 + 7 * flip)) & 0x7f`, the state kept or replaced by a mask): -9.9 % to -6.1 % lossy and -23.3 % to -21.2 % lossless at
stage 1, -19.1 % to -11.9 % and -24.3 % to -22.6 % at stage 2. clang takes the select form (the masks on clang-cl are worse: -21.0 %
lossless and -9.0 % lossy at 1,024²). Stage 1 alone is -9.9 % at 512² and -6.6 % at 1,024² on lossy GCC, the latter above the -9.0 % target; stage 2 brings
them to -19.1 % and -18.3 %.

### Not measured, and the gaps

- **Not a real Linux build.** There is no Linux or WSL here: MinGW GCC 13.2 and the Visual Studio clang were measured
  on Windows, Tier-1 only, in a small harness, not the whole decode (no GCC whole-decode ratio exists). The
  `linux-gcc`, `linux-clang`, `linux-clang-asan-ubsan` (so UBSan), TSan and libFuzzer presets were not run; UBSan did
  not link with this clang-cl. The exactness of the GCC build rests on `nj2k_t1_unit` compiled and run with it.
- No new shared state: the tables are `constexpr`, one `T1` a job; TSan was not run on the threaded decode.
- The corpus is the author's synthetic texture at six configurations (gradients, edges, noise); the owner's
  212-file corpus of real textures is not on this machine. The mode-switch coverage is the test files' 35,390
  code-blocks, of which the committed vectors hold the generated corpus's.
- The full dump set (37,424 blocks: the 35,390 mode-switch conformance blocks and the 2,034 of the six study images) is
  not committed (about 18 MB): `NJ2K_T1_VECTORS` runs it where it exists. The fuzz seeds are 28 code-blocks of the committed vectors.
- Observation, fixed in its own commit: an arithmetic-coded pass started on a segment that was initialised for raw
  (BYPASS) decoding (a cleanup pass in a segment the style types raw) ran the coder on registers nobody initialised, and
  OpenJPEG's text, the frozen copy and the first version here could read past the buffer's end. Tier-2 cannot build such
  a segment (all segments but the last are full and their types follow the pass counts), so the guard (a raw segment's
  passes end at its cleanup pass) changes results only for streams Tier-2 cannot produce.
- No hardware counters: the branch counts of the author's study (a last-outcome predictor over the recorded decision stream:
  23 to 25 % misses a decision on the LPS-interval branch lossy, 40 to 42 % lossless) are derived, not read (the author's study notes, not in the repository).

## Stage 6e (E1): the encoder's branch-free preprocessing (2026-11-22)

Step E1 of the EBCOT-NG plan: the sign-magnitude conversion and the block maximum at the head of
`enc_t1_encode` (OpenJPEG's per-sample branch on the sign) as scalar code without a data-dependent branch, in
`src/enc_pre.h` (`enc_preprocess`): `neg = 0 - (x >> 31)`, `a = (x ^ neg) - neg`, `a -= a >> 31` (INT32_MIN
negates to itself, and the subtraction turns 0x80000000 into 0x7FFFFFFF, the earlier `INT32_MIN + 1` negated;
no other `a` has bit 31), `out = a | (x & 0x80000000)`, `max = max(max, a)`. No intrinsics. It is stacked on the
encoder branch (`mq-encoder-ng`, stage 6c) and measured against it.

**Exactness.** `nj2k_enc_t1_unit` checks `enc_preprocess` against the earlier per-sample code (kept in the test)
over every output word and the block maximum, in blocks of 4,096 samples: a trimmed pass in ctest (the
neighbourhoods of 0, INT32_MIN and INT32_MAX, a window of 4,096 around every power of two and its negative, a
stride of 61 from 0 to about 0xF4000000, 200,000 random blocks with INT32_MIN or INT32_MAX in them) and all 2^32 inputs
in the nightly `enc_t1_exhaustive` (`--exhaustive-full`, about 2 minutes with the MQ decisions; the 2^32 pass
alone takes a few seconds). `enc_t1_encode` against the frozen reference (the earlier text of `enc_t1.cpp`, left as
it was) on 1,680 more generated code-blocks: every largest-magnitude class from 0 to 25 bit-planes (1 to 25: the
largest sample, negative or positive, above every other; 0: all samples under 2^6, where `numbps` wraps as an
unsigned in OpenJPEG, the frozen reference and the new code alike and no pass runs), each in all 8 sizes x 4
orientations x 2 wavelets (asserted), INT32_MIN alone, among others and with INT32_MAX, and all-zero blocks:
passes (rate, length, termination, distortion bit for bit), bytes and `numbps` equal.
The one-thread upload encode's codestream (size and hash) is the earlier one's at 256², 512² and 1,024², lossy
and lossless, both builds. Mutations the test must catch, each tried: without the `a -= a >> 31` step, without `| (x & 0x80000000)`, and with
the maximum taken signed over the stored words (sign bit included) each makes `enc_t1_unit` fail (the trimmed
pass and the earlier block comparison; the new bit-plane blocks were not reached after the first failure). The
maximum taken signed over the magnitudes alone is an equivalent mutant (a magnitude never has bit 31), so no test
can catch it.
Every ctest passes on MSVC AVX2 (126, the two class 1 conformance tests skipped for lack of data), the MSVC SCALAR
build (130, `global_state` included), clang-cl (130) and MSVC with AddressSanitizer (125, which has no bench
tests), the fuzz replays among them; the nightly `enc_t1_exhaustive` (all 2^32 preprocessing inputs and the 4.4
billion MQ decisions) passed on MSVC.

**Method.** As stage 6c: Ryzen 9 9950X3D, MSVC 14.51 (the library's flags, `/O2 /arch:AVX2` or baseline
x86-64), one thread pinned to CPU 7, TSC cycles, the two builds interleaved in alternating order, nine rounds
(21 repeats a round, 7 at 1,024²), the median of the rounds' ratios. "Kernel" is `enc_t1_encode` over the
code-blocks of the upload encode's texture (the study's dump, `bench/t1_study`), the earlier library against this
one, in separate processes; "encode" is the whole one-thread upload encode (5 rounds, 9 repeats, 3 at 1,024²).
The harness is the study's (`bench/t1_study` and `bench/ebcotng_study`, on the `studies` branch, see
`bench/STUDIES.md`), not in this branch. The gate: at most -3.0 % against the base kernel on every
configuration, beyond the 3 % noise band.

Kernel, Mcycles and change against the stage-6c base (minus is faster; the round-to-round range of the change was 0.2 to 1.0 percentage points):

| Size | Mode | AVX2 base | AVX2 E1 | AVX2 change | SCALAR base | SCALAR E1 | SCALAR change | Passes gate |
|---|---|---:|---:|---:|---:|---:|---:|---|
| 256² | lossy | 33.4 | 31.8 | -5.0 % | 33.0 | 31.1 | -5.8 % | Yes |
| 256² | lossless | 37.8 | 36.3 | -4.1 % | 37.2 | 35.4 | -4.8 % | Yes |
| 512² | lossy | 158.7 | 152.1 | -4.2 % | 156.4 | 149.1 | -4.8 % | Yes |
| 512² | lossless | 174.1 | 167.8 | -3.6 % | 171.2 | 164.3 | -4.1 % | Yes |
| 1,024² | lossy | 646.6 | 619.8 | -4.1 % | 637.8 | 608.3 | -4.7 % | Yes |
| 1,024² | lossless | 706.5 | 680.8 | -3.6 % | 695.3 | 667.0 | -4.1 % | Yes |

The whole one-thread encode follows the kernel (the preprocessing is in it, the rest of the encode unchanged):
AVX2 -4.1 % and -4.1 % (256²), -3.3 % and -3.7 % (512²), -3.3 % and -3.5 % (1,024², lossy and lossless); SCALAR -4.4 %
and -4.0 %, -3.7 % and -3.6 %, -4.0 % and -3.6 %. The gate holds on all twelve MSVC kernel configurations. The study's
prototype measured -4.8 % to -3.4 % (AVX2) and -7.7 % to -5.5 % (SCALAR); the scalar build gains less here than there
(-5.8 % to -4.1 %): the stage-6c base moved, and the loop is kept scalar (below).

**Leaner.** Only in this sense: the loop has no data-dependent branch (the earlier one branched on the sign of
every sample and again on INT32_MIN in the negative path). It does one more store per positive sample (it writes
every sample back; the earlier loop wrote the negative ones only) and the same loads. No hardware counters were
used: instruction, branch and miss counts are not measured.

**MSVC's vectoriser.** The first form of the loop was vectorised by MSVC: with AVX2 by `vpmaxud`, in the
baseline build by SSE4.1 `pmaxud` behind its run-time CPU check (`__isa_available`), which the `global_state`
test refuses in a scalar build (a `.bss` word and an instruction above baseline x86-64). Vectorised, the kernel
was -6.4 % to -4.5 % (AVX2) and -6.2 % to -4.1 % (SCALAR). The step is specified as scalar code, so the loop has
`#pragma loop(no_vector)` under MSVC (as `jp2.cpp`'s palette loop has), which costs about one point and keeps
`global_state` green in both builds.

### GCC and clang

MinGW GCC 13.2 (`-O2`, kernel only, 64 x 64 code-blocks of the same texture, the library's `enc_t1.cpp` and
`enc_dwt.cpp` against the base's, three rounds at 512² and 1,024², seven at 256²): baseline x86-64 -5.0 % and
-4.1 % (256²), -3.6 % and -3.2 % (512²), -3.7 % and -3.2 % (1,024²); `-march=x86-64-v3` -5.8 % and -5.1 %, -4.9 % and
-4.4 %, -4.8 % and -3.9 %. GCC's nearest cell is -3.2 %, with one round at -2.9 %, inside the noise of a three-round
median; the gate is met on the medians, with no margin to speak of.

**clang-cl misses the gate.** The Visual Studio clang (`-T ClangCL`, the same flags as MSVC's), AVX2 build: -0.9 %
(256² lossy), -0.6 % (256² lossless), -0.6 % (512² lossy); the other configurations and its SCALAR build were not
measured (the run was stopped at that result): 3 of the 12 configurations, -0.9 % to -0.6 %, no gain. Its compiler already compiles the earlier loop's main part without a
sign branch (selects and `vpmaxsd`, vectorised), so there is no branch for E1 to remove, and the new loop gives about 1 %.
The output is identical; nothing in the code is left for a Clang-specific form to win.

### Not measured, and the gaps

- No Linux or WSL here: the `linux-*` presets, UBSan, TSan and libFuzzer were not run locally (the repository's CI
  runs them). The E1 numbers for GCC and clang come from Windows (MinGW GCC 13.2, clang-cl), kernel only; the Linux CI
  bench of stage 6c measures the stage-2 MQ coder, not E1. No GCC or clang whole-encode figure exists, and
  `nj2k_enc_t1_unit` was not run locally with them.
- The comparison with OpenJPEG 2.5.4 (`nj2k_encode_bench`) was not repeated: the README's and stage 6c's encode rows
  are before E1; with MSVC E1 takes about 3 to 4 % off the one-thread encode time (-4.4 % to -3.3 % of it). No GCC or clang
  whole-encode figure exists.
- One machine (Zen 5), one synthetic texture; the machine was not quiet. The harness is on the `studies` branch, not in this branch.

## Stage 6f (E2): the cleanup pass from a mask fixed at the column's start (2026-11-22)

Step E2 of the EBCOT-NG plan, on `main` after E1 (#23): the cleanup pass (`enc_clnpass`, `src/enc_t1.cpp`) takes a
column outside run mode (a full stripe, a non-zero flag word) from one mask computed at the column's start,
`cand = ~((f >> 4) | (f >> 21)) & 0x249` (row r at bit 3r: significance at bit 4 + 3r, visited at 21 + 3r). The
rows that are neither are coded. A row becomes significant only by being coded itself (coding row r sets that row's
own sigma and its neighbours' neighbour bits, never another row's sigma or pi), so the set is the same at the
column's start and when each row is reached: the pass codes the same decisions in the same order. One pi clear per
column replaces four flag reloads, tests and clears, and the "all significant and visited" case is `cand = 0`.
Run-mode columns and the partial stripe are as before. The four rows are unrolled (`enc_clnpass_row<ci>`, a template
on the row), so each row's shifts and the `ci` branches of `update_flags` and of the sign-context index are
constants; both are `NJ2K_FORCE_INLINE` for that (the significance pass also uses them, with literal rows already).
No intrinsics.

**The first form missed, the second passes.** The first form (the study's prototype: a `tzcnt` loop over the set bits
of `cand`, one iteration a coded row) gave -3.1 % to -2.3 % against the base kernel on MSVC AVX2 (the gate is -3.0 % beyond the
noise; the study had -4.5 % to -3.8 % on another base): the loop's exit branch and the runtime row index (variable
shifts, the `ci == 0` and `ci == 3` branches of `update_flags`, the sign index) cost what the mask saved. The unrolled
form is the one committed.

**Exactness.** Output byte-identical: the codestream and each pass's rate, length, termination and distortion (bit for
bit) are the earlier code's. `nj2k_enc_t1_unit` compares `enc_t1_encode` with the frozen reference
(`tests/unit/enc_t1_frozen.cpp`, unchanged) on 8,292 more blocks built for the cleanup pass (`test_encode_cleanup`):
(a) 2,304 blocks, every pair of 4-bit row masks (rows significant early, rows at the middle plane, the others small;
rotated by the column so that the neighbours differ), four sign patterns (positive, negative, checkerboard, random), on
nine sizes (whole stripes and partial ones), all orientations and both wavelets over the pairs: columns with 0, 1, 2,
3 and 4 candidates, every reachable candidate set (11 of the 16 4-bit sets; 5, 9, 10, 11 and 13 cannot occur, by the
audit's reasoning and with zero hits over about 55 million columns); (b) 5,888 blocks, every size 1..64 x 1..64 (the edge sizes in all 8 orientation
and wavelet combinations), each sample with a leading bit-plane of its own, the block at 0 to 25 planes (each count
has at least 100 blocks, asserted); (c) 100 dense 64 x 64 blocks of one magnitude (the all-significant, all-visited
columns), 25 plane counts x 4 sign patterns. Mutations tried, each makes `enc_t1_unit` fail: the mask without the
visited bits, without the significance bits, without the visited-bit clear at the end, and a row left out of the mask
(the older generated blocks of `test_encode` fail first). The reviewer confirmed that `test_encode_cleanup` run alone
catches seven mutations: the mask without the visited bits, without the significance bits, the masks 0x49 and 0x241, the
shift 5 instead of 4, the shift 20 instead of 21, and no visited-bit clear. The
nightly `enc_t1_exhaustive` (`--exhaustive-full`: all 2^32 preprocessing inputs and the 4.4 billion MQ decisions)
passes. The one-thread upload encode's codestream (size and hash) equals main's at 256², 512² and 1,024², lossy and
lossless, in both builds, and the kernel's passes and bytes (hashed over every dumped block) are equal in every build
measured. Every ctest passes on MSVC AVX2 (133 tests registered with the openjpeg-data checkout, none skipped; 129
without it, where the two class 1 conformance tests skip), on MSVC SCALAR and on clang-cl; MSVC with AddressSanitizer:
see the gaps.

**Method.** As stage 6e: Ryzen 9 9950X3D, MSVC 14.51 (the library's flags, `/O2 /arch:AVX2` or baseline x86-64), one
thread pinned to CPU 7, TSC cycles, main (`ca64528`) and E2 in separate processes run alternately (the order swapped
every round), the median of the rounds' ratios. "Kernel" is `enc_t1_encode` alone over the code-blocks of the upload
encode's texture (the study's dump; `bench/t1_study` and the study harnesses are on the `studies` branch, see
`bench/STUDIES.md`: 75, 210 and 777 blocks at 256², 512², 1,024²; 21 repeats a
process, 7 at 1,024²; 9 rounds); "encode" is the whole one-thread upload encode (5 rounds, 9 repeats, 3 at 1,024²). The
harness is a throwaway one (a small timer over the dump and the library's sources, built once from `main` and once from
this branch), not in the repository. The gate: at most -3.0 % against the base kernel on every configuration, beyond the 3 %
noise band.

Kernel, Mcycles and change against main (minus is faster; the round-to-round range of the change was 0.2 to 1.2 percentage points):

| Size | Mode | AVX2 main | AVX2 E2 | AVX2 change | SCALAR main | SCALAR E2 | SCALAR change | Passes gate |
|---|---|---:|---:|---:|---:|---:|---:|---|
| 256² | lossy | 31.1 | 29.5 | -4.9 % | 31.3 | 29.2 | -6.7 % | Yes |
| 256² | lossless | 35.5 | 33.7 | -5.1 % | 35.8 | 33.3 | -7.0 % | Yes |
| 512² | lossy | 149.1 | 141.8 | -4.9 % | 149.9 | 140.1 | -6.6 % | Yes |
| 512² | lossless | 164.3 | 155.8 | -5.2 % | 165.7 | 154.0 | -7.0 % | Yes |
| 1,024² | lossy | 607.6 | 576.9 | -5.0 % | 611.0 | 571.8 | -6.4 % | Yes |
| 1,024² | lossless | 667.6 | 632.2 | -5.3 % | 671.9 | 625.8 | -6.9 % | Yes |

The gate holds on all twelve MSVC configurations (-5.3 % to -4.9 % AVX2, -7.0 % to -6.4 % SCALAR). The whole one-thread
encode follows (the kernel is most of it): AVX2 -4.1 % and -4.9 % (256², lossy and lossless), -4.2 % and -5.3 % (512²),
-4.2 % and -5.0 % (1,024²); SCALAR -4.6 % and -6.2 %, -5.0 % and -6.2 %, -5.0 % and -6.2 %.

**Leaner.** In this sense: for a column outside run mode the new code builds the mask from one load of the flag word,
tests one bit a row in a register where the old code reloaded the word and tested two bits of it, and clears the visited
bits once per column instead of once per row; the shifts and the branches on `ci` in `update_flags` and the sign index
are constants. Fewer instructions, flag reloads and branches are expected from that, not measured: no hardware counters
were used. The row's own branch (is it a candidate) remains, as in the old code. The step that became unreachable
(`enc_clnpass_step`'s all-significant-and-visited shortcut: it runs only from run-mode columns and the partial stripe)
was removed in the fix round.

**Code size.** The unrolling costs text. `enc_t1.o`: MinGW GCC `-O2` 21,528 to 25,948 bytes (+20 %), clang 15,781 to
19,845 (+26 %), MSVC `.text` 19,133 to 27,651 (AVX2, +45 %) and 18,985 to 27,260 (SCALAR). With plain `inline` instead of
`NJ2K_FORCE_INLINE` on `update_flags` and the sign index, GCC's and clang's sizes are the same (clang's code did not
change at all with the force-inline change), so the unrolling accounts for the growth there. On MSVC the force-inline of
the two helpers in the significance pass and in `enc_clnpass_step` was measured as a variant (A: forced only in the
unrolled rows, via compile-time-row copies of the helpers; B: forced everywhere, as committed): A is 358 bytes (1.3 %)
smaller (AVX2; 178 bytes SCALAR) and its kernel is -0.4 % to -0.2 % against B (AVX2) and -1.3 % to -0.7 % (SCALAR), so within the
noise band; not clearly smaller, so B stays and the helpers are not duplicated. The gain over main is not split between
the unrolling and the force-inline change.

### GCC and clang (E2)

Kernel only, the same dumps (7 rounds), the library's `enc_t1.cpp` (with `enc_dwt.cpp`, `arena.cpp`, `memory.cpp`) of main
against this branch's:

| Build | 256² lossy | 256² lossless | 512² lossy | 512² lossless | 1,024² lossy | 1,024² lossless | Passes gate |
|---|---:|---:|---:|---:|---:|---:|---|
| clang-cl AVX2 | -5.1 % | -5.2 % | -5.2 % | -5.4 % | -5.3 % | -5.4 % | Yes |
| clang-cl SCALAR | -5.1 % | -5.6 % | -5.0 % | -5.6 % | -4.9 % | -5.5 % | Yes |
| MinGW GCC 13.2 `-O2` (baseline) | -4.8 % | -4.9 % | -4.8 % | -4.9 % | -4.8 % | -4.9 % | Yes |
| MinGW GCC 13.2 `-O2 -march=x86-64-v3` | -4.0 % | -4.3 % | -4.1 % | -4.7 % | -4.0 % | -4.6 % | Yes |

None misses (the worst cell is -4.0 %); unlike E1, clang-cl gains as much as MSVC.

### Linux, GCC 14, GCC 15 and Clang (E2)

Workflow `bench-kernel-compare.yml`, run 45056793413 (head = `enc-e2-cleanup` at `0459480`, base = main at `a60e153`):
each of the six jobs builds `nj2k_enc_t1_ratio` for both refs and runs the two binaries alternately on the same machine
(head, base, head, base, head, base; 3 runs of 41 rounds, 256 blocks of 64 x 64). Each number is the change in the
encoder's kernel against the frozen pre-redesign reference in one process (minus is faster); E2 / main is the E2 change over main's,
run by run. These are same-machine ratios, one CPU per job, and are the table of record for Linux; the absolute ratios differ
from job to job with the CPU. The output is identical (the kernel hash of the lossy and lossless sets is the same for both refs).

E2 / main, the three runs' median (change; all three runs in brackets), lossless and lossy:

| Job | CPU | Lossless | Lossy | Passes gate |
|---|---|---|---|---|
| gcc-14, AVX2 | Intel Xeon Platinum 8573C | -4.6 % (-4.6 %, -4.6 %, -4.8 %) | -4.6 % (-4.6 %, -4.5 %, -4.7 %) | Yes |
| gcc-14, SCALAR | Intel Xeon 6975P-C | -6.3 % (-6.3 %, -6.3 %, -6.3 %) | -5.5 % (-5.5 %, -5.5 %, -5.5 %) | Yes |
| gcc-15, AVX2 | AMD EPYC 7763 | -6.0 % (-5.6 %, -6.0 %, -6.0 %) | -5.7 % (-5.4 %, -5.8 %, -5.7 %) | Yes |
| gcc-15, SCALAR | AMD EPYC 7763 | -6.0 % (-6.0 %, -6.0 %, -6.0 %) | -5.6 % (-5.8 %, -5.6 %, -5.6 %) | Yes |
| clang, AVX2 | AMD EPYC 7763 | -6.4 % (-6.0 %, -6.5 %, -6.4 %) | -5.5 % (-5.1 %, -5.5 %, -5.5 %) | Yes |
| clang, SCALAR | Intel Xeon Platinum 8573C | -5.8 % (-6.2 %, -5.8 %, -5.8 %) | -4.8 % (-4.9 %, -4.8 %, -4.8 %) | Yes |

The changes against the frozen reference behind them, E2 against main (the median of three runs, lossless / lossy):

| Job | E2 | main | Passes gate |
|---|---|---|---|
| gcc-14, AVX2 | -49.1 % / -46.6 % | -46.6 % / -44.0 % | n/a |
| gcc-14, SCALAR | -46.6 % / -43.8 % | -43.0 % / -40.5 % | n/a |
| gcc-15, AVX2 | -41.2 % / -39.7 % | -37.5 % / -36.1 % | n/a |
| gcc-15, SCALAR | -31.4 % / -29.8 % | -27.0 % / -25.6 % | n/a |
| clang, AVX2 | -45.3 % / -42.7 % | -41.6 % / -39.3 % | n/a |
| clang, SCALAR | -46.9 % / -44.4 % | -43.6 % / -41.5 % | n/a |

E2 gains about 5 to 6 % on all six Linux builds (-6.4 % to -4.6 %; the three runs of a job agree within 0.5 percentage points), clang
included, unlike E1, which had nothing to gain with clang. gcc-15 SCALAR on that EPYC reads -27.0 % to -25.6 % on main and -31.4 % to
-29.8 % with E2 (the level accepted in Stage 6c is about -30.0 %).

### Not measured, and the gaps (E2)

- No Linux or WSL here: the `linux-*` presets run in the repository's CI; UBSan, TSan and libFuzzer were not run
  locally. `nj2k_enc_t1_unit` was not run locally with GCC or clang (the ctest on clang-cl was). The Linux kernel numbers
  above are from the bench workflow only (kernel, no whole-encode figure on Linux).
- MSVC ASan: 124 ran and passed; 4 were excluded and not run (`decode_unit`, `decode_threads` and two
  `fuzz_replay.decode*`: the decoder is not touched, and those tests ran for over an hour under AddressSanitizer here
  without finishing); every encoder test, `enc_t1_unit` among them, ran. The ASan preset registers 128 tests with the
  openjpeg-data checkout (124 without it) against the MSVC preset's 133 (129 without it), because the five library checks (`exports`, `openjp2_exports`, `openjp2_forwarders`, `global_state`,
  `oracle_known_resources`) are not registered when `NJ2K_SANITIZE` is set (`tests/CMakeLists.txt`).
- The comparison with OpenJPEG 2.5.4 (`nj2k_encode_bench`) was not repeated: the README's rows are before E1 and E2.
- One machine (Zen 5), one synthetic texture; the machine was not quiet. The partial stripe (height not a multiple of
  4) and the run-mode columns are not changed; the share of columns outside run mode on real images is not measured.

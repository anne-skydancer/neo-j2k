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
pays). The Ryzen 9 9950X3D, with 8 logical CPUs visible to this Windows session, so N = 8 is every core;
MSVC 14.51, RelWithDebInfo.

### Large images: time and peak memory against OpenJPEG at N threads (the gate)

The images and method of stage 1c (a process a decode, the peak its peak commit; three rounds, two for
16,384², the thread counts and decoders taking turns in each round, the order rotating; medians, and the
highest peak). One thread is pinned to core 7 as before; at 4 and 8 threads nothing is pinned. The total CPU
load, sampled every 5 s, includes the decoders' own threads (23 % mean, 53 % max). Every decode at every
count is identical to OpenJPEG's one-thread decode (the hash column of the run).

From memory (the viewer's way: OpenJPEG through a memory stream, neo-j2k through `nj2k_decode_memory` into
int32 planes):

| Image | N | OpenJPEG ms / MB | neo-j2k ms / MB | neo-j2k / OpenJPEG |
|---|---|---|---|---|
| 4,096², one tile, 9/7 | 1 | 378 / 205 | 233 / 200 | 0.62 |
| | 4 | 180 / 206 | 74 / 201 | 0.41 |
| | 8 | 135 / 207 | **46 / 201** | 0.34 |
| 4,096², 16 tiles, 5/3 | 1 | 3,147 / 248 | 2,934 / 244 | 0.93 |
| | 4 | 858 / 248 | 766 / 245 | 0.89 |
| | 8 | 474 / 249 | **416 / 245** | 0.88 |
| 8,192², one tile, 9/7 | 1 | 1,579 / 813 | 961 / 792 | 0.61 |
| | 4 | 776 / 815 | 346 / 793 | 0.45 |
| | 8 | 596 / 816 | **238 / 793** | 0.40 |
| 16,384², 256 tiles, 9/7 | 1 | 5,463 / 3,134 | 3,392 / 3,131 | 0.62 |
| | 4 | 2,729 / 3,135 | 1,154 / 3,131 | 0.42 |
| | 8 | 2,000 / 3,135 | **754 / 3,131** | 0.38 |

neo-j2k is faster at every count, and needs no more memory: the threaded tile's own memory (a Tier-1 scratch
and a wavelet line per part, 8 bytes a code-block) is about a megabyte. At 8 threads it decodes 4.0-7.1x
faster than on one (OpenJPEG 2.6-6.6x). What stays on the calling thread: the walk over the tile-parts and
the packets (Tier-2, serial in OpenJPEG too), each tile's layout and the wavelet levels below 2^18 samples;
tiles are not decoded in parallel (`doc/stages.md`).

Through the stream API (two rounds): 4,096² one tile 133 / 47 ms at 8 threads (OpenJPEG / neo-j2k), 16
tiles 479 / 439, 8,192² 582 / 240, 16,384² 2,023 / 694; the peaks as from memory but for the multi-tile
images, 238 and 3,131 MB against OpenJPEG's 210 and 3,097 at every count: stage 1c's stream input, which
holds the codestream (the low-memory stream belongs to the j2k-tile module, owner 2026-11-20), unchanged by
threads.

### Identity (the gate)

`decode_threads` in CI (both builds, every platform; `doc/testing.md`), and locally with `--files` on the
private bench corpus (212 files: 13,568 threaded decodes, at the library's thresholds at reduce 0-2 and with
every stage forced at reduce 0, into int32 planes with guard bytes and the viewer's 8-bit image, each codec
decoding twice) and the four large images (112 decodes), in the AVX2 and the scalar build: every decode at
2, 4, 8 and 16 threads gives one thread's samples, status and messages, in order.

### Where threads start

A tile is split from 2^18 decoded samples over its components on (a 512 x 512 grey or a 296 x 296 RGB
tile). One codec decoding the same texture from memory again and again, one thread against N with every
stage forced (median of 60 each, taking turns, quiet machine):

| Texture | 1 thread | 2 | 4 | 8 | CPU efficiency at 8 |
|---|---|---|---|---|---|
| 128² RGB | 567 µs | 0.61 | 0.41 | 0.50 | 25 % |
| 256² RGB | 2,334 µs | 0.55 | 0.37 | 0.29 | 43 % |
| 512² RGB | 9,009 µs | 0.54 | 0.30 | 0.20 | 63 % |
| 1024² RGB | 36,116 µs | 0.52 | 0.28 | 0.15 | 83 % |

Threads shorten even a 128² decode, but at a high price in CPU time: below the threshold the cores do more
for a host that decodes many images at once (the viewer's case) as one image a thread, owner decision D8.

### Small images: no executor, as the viewer decodes

The viewer's request on neo-j2k's own path through `main`'s and this stage's shared library. The
interleaved bench with both libraries twice in one process cannot resolve these classes: with four similar
libraries running in turn, a library's time in the reduced classes moved by up to 15 % from one copy to
another (the same bytes under another file name), and two copies of `main` agreed with each other while
both differed from a third. So each library ran in a process of its own, `main` and stage 5 in turn, three
times each (20 s a run for the 32-256 px files, 34 s for 512 and 1024 px; load from other processes under
6 %). The spread of a library's own three runs is the noise (at most 1.5 % in a class, mostly under
0.6 %). Every decode is identical in both.

| Request (median per file, then over the class) | Files | main µs | stage 5 µs | stage 5 / main |
|---|---|---|---|---|
| 32² whole | 28 | 7.37 | 7.35 | 1.00 |
| 32² at reduce 1-5 | 28 | 2.91-5.32 | 2.94-5.39 | 1.009-1.016 |
| 64² whole | 32 | 264 | 266 | 1.008 |
| 64² at reduce 1, 2, 3 | 32 | 34.9, 17.6, 10.7 | 35.9, 18.1, 10.9 | 1.029, 1.032, 1.021 |
| 128² whole | 32 | 416 | 419 | 1.008 |
| 128² at reduce 2, 3, 4 | 32 | 31.5, 15.0, 8.83 | 32.2, 15.5, 9.03 | 1.025, 1.032, 1.023 |
| 256² whole | 40 | 1,717 | 1,725 | 1.004 |
| 256² at reduce 1-5 | 40 | | | 1.002-1.012 |
| 512² whole | 40 | 8,657 | 8,706 | 1.006 |
| 512² at reduce 1-5 | 40 | | | 1.001-1.009 |
| 1024² whole | 40 | 24,930 | 25,020 | 1.004 |
| 1024² at reduce 1-5 | 40 | | | 0.995-1.005 |
| the viewer's budgets, 64-1024² | 14-37 | | | 0.971-1.041 (64² at reduce 2: 1.041, 128² at reduce 3: 1.035) |
| first 600 bytes, 32-1024² | 2-38 | | | 1.000-1.012 |

**The gate is not met everywhere:** whole decodes are within 0.4-0.8 % of `main`, but reduced decodes of 64
and 128 px textures, where Tier-1 handles many small code-blocks, are 2-4 % slower (0.3-1 µs a request), above
the 1.5 % noise. What it comes from was narrowed down to code generation, not to work the decode does: the
shared library is built with link-time code generation, which compiles Tier-1's and the wavelet's functions by
where they are called from, and the threaded path calls them from new places. These arrangements were measured
(the interleaved bench, so their small-class figures are the uncertain ones):
- the parts calling Tier-1's entry points from a second place: whole 1024² decodes 1.0-1.5 % slower (stable in
  every run), small reduced decodes no slower;
- Tier-1 called from one function for one thread and the parts (`decode_blocks`, the committed arrangement):
  whole decodes as `main`'s, reduced small ones as above;
- the small wavelet levels of a threaded tile run through the threaded path's own loop: the small levels'
  transform compiled differently for one thread too (machine code compared function by function against
  `main`'s), so they now go through one thread's own call;
- Tier-1's entry points kept out of line, or compiled twice from one source for the two callers: whole 1024²
  decodes 1.4 % slower again.
Neither the source of the one-thread path's kernels nor their operations changed; their machine code did.

### Not measured

- The encoder (stage 6b uses the same executor).
- A Linux build's large-image times (CI runs the identity gate and ThreadSanitizer there).

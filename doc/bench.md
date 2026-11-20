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

### The kernel gates (`nj2k_kernels`, one plane or image, median of interleaved rounds)

The times leave out the plane copies the bench makes before each round (it prints them).

| Kernel | Size | Before (stage 1c) | After | Gain | Plan gate |
|---|---|---|---|---|---|
| Output tail: ICT, DC shift, 8-bit interleaved bottom-up copy (RGB 9/7) | 1024² | 1,177 µs | 401 µs | **2.9×** | ≥ 5× at 1024² |
| | 4096² | 24.2 ms | 7.2 ms | 3.4× | |
| Inverse 9/7, 5 levels | 1024² | 2,545 µs | 587 µs | **4.3×** | ≥ 5× at 1024² |
| | 4096² | 44.0 ms | 10.6 ms | 4.2× | |
| Inverse 5/3, 5 levels | 1024² | 950 µs | 405 µs | 2.3× | |
| | 4096² | 18.0 ms | 6.8 ms | 2.6× | |

Neither gate's factor is reached against stage 1c, which already had OpenJPEG's SIMD transforms and its own
AVX2 DC shift (the plan's factors were measured against OpenJPEG's scalar tail, 4.2-4.4 ms at 1024², which the
fused pass beats by about 11×). The 9/7 vertical pass alone went from OpenJPEG's 8-column strips to whole rows:
about 0.17 ns a sample against 1.2 (a profiled build's split); what is left of the transform is the horizontal pass, already on rows in the
first caches. Small levels keep OpenJPEG's structure, which is faster there (9/7 up to 64², 5/3 up to 256²,
measured with the same bench): the row-wise transform takes over from 128² (9/7) and 512² (5/3) samples a
level.

### Small images: the viewer's request

1,437 configurations in 257 s (the 15-minute box not reached). Load: other processes 26 % mean, 53 % p90, 88 % max
of 8 CPUs (other sessions' builds: GCC, MSBuild, link, cl, cmake, and a viewer). Every decode of every decoder is
identical to the oracle's (`same` equals the files in every class). Medians per file, then over the class.

| Request | Files | Oracle µs | main µs | stage 2 µs | stage 3 µs | stage 4 µs | stage 4 / main | stage 4 / oracle |
|---|---|---|---|---|---|---|---|---|
| 32² whole | 28 | 34.9 | 9.9 | 8.2 | 8.2 | **8.2** | 0.83 | 0.24 |
| 32² at reduce 3 | 28 | 24.9 | 4.0 | 4.0 | 4.0 | 4.0 | 1.02 | 0.16 |
| 32² at reduce 5 (1x1 floor) | 28 | 22.5 | 3.1 | 3.2 | 3.1 | **3.1** | 1.01 | 0.14 |
| 64² whole | 32 | 400 | 366 | 363 | 368 | **336** | 0.92 | 0.84 |
| 64² at reduce 2 | 32 | 75.3 | 46.8 | 50.3 | 52.5 | 42.8 | 0.92 | 0.57 |
| 128² whole | 32 | 633 | 573 | 567 | 547 | **507** | 0.89 | 0.80 |
| 128² at reduce 3 | 32 | 56.5 | 28.1 | 33.9 | 36.1 | 27.1 | 0.97 | 0.48 |
| 128² at reduce 5 | 32 | 26.6 | 6.5 | 6.9 | 6.7 | 6.8 | 1.04 | 0.26 |
| 256² whole | 40 | 2,325 | 2,101 | 2,033 | 1,949 | **1,823** | 0.87 | 0.78 |
| 256² at reduce 3 | 40 | 174 | 140 | 144 | 146 | 133 | 0.95 | 0.76 |
| 256² at reduce 5 | 40 | 37.2 | 12.7 | 13.3 | 13.1 | 13.2 | 1.04 | 0.35 |
| 512² whole | 40 | 11,538 | 10,588 | 10,722 | 9,653 | **9,017** | 0.85 | 0.78 |
| 512² at reduce 2 | 40 | 1,669 | 1,557 | 1,574 | 1,554 | 1,450 | 0.93 | 0.87 |
| 1024² whole | 40 | 37,507 | 34,550 | 34,407 | 27,447 | **25,921** | 0.75 | 0.69 |
| 1024² at reduce 1 | 40 | 13,397 | 12,155 | 12,091 | 11,324 | 10,662 | 0.88 | 0.80 |
| 1024² at reduce 3 | 40 | 1,577 | 1,443 | 1,464 | 1,438 | 1,354 | 0.94 | 0.86 |
| 1024² at reduce 5 | 40 | 316 | 229 | 228 | 228 | 224 | 0.98 | 0.71 |
| 1024², first 600 bytes | 34 | 71.7 | 25.2 | 26.1 | 26.1 | 25.1 | 1.00 | 0.35 |
| 512², first 600 bytes | 38 | 42.3 | 14.7 | 16.5 | 16.6 | 14.9 | 1.02 | 0.35 |
| 256², first 600 bytes | 38 | 26.7 | 6.6 | 7.3 | 7.0 | 6.9 | 1.04 | 0.26 |

The whole decodes gain 8-25 % over stage 1c, most at 1024² (the wavelet: 20 % of the request there; Tier-1's
renormalisation 5-7 % from 64² up). The requests whose output is a few pixels (the floor, reduce 4-5, the first
600 bytes) have nothing these stages touch beyond a few hundred samples; they are within 1-6 % of stage 1c in
this run, at 3-15 µs under this load. A rerun of the 128-512 px classes at reduce 3-5 and their first 600 bytes
(three decoders, the same load) put them within 2 % of stage 1c either way (per-file ratios 0.985 to 1.019),
which is the bench's noise there: two copies of the same build differ by up to 0.5 % in a quiet run. That rerun also found and removed a fixed cost stage 2 had added (2-4 %
on those requests: the clip rectangles computed for the single tile, a 4 KiB stack buffer under the stack probe),
the follow-up commit. Heap calls: none after warm-up up to 512², one allocation and one free at 1024² whole (the
8 MiB the codec keeps), as in stage 1c.

### Large images: time and peak memory against OpenJPEG, one thread

The images and the method of stage 1c (a process a decode, the decoders in turn, three rounds, two for 16,384²;
medians; peak commit). From memory (the viewer's way: OpenJPEG through a memory stream, neo-j2k through
`nj2k_decode_memory` into int32 planes); load: total CPU 26 % mean, 96 % max (sampled every 5 s):

| Image | OpenJPEG ms / MB | main ms / MB | stage 2 | stage 3 | stage 4 ms / MB | stage 4 / OpenJPEG |
|---|---|---|---|---|---|---|
| 4,096², one tile, 9/7 | 382 / 205 | 345 / 200 | 341 / 200 | 250 / 200 | **240 / 200** | 0.63 |
| 4,096², 16 tiles, 5/3 | 3,160 / 248 | 3,140 / 245 | 3,147 / 244 | 3,098 / 245 | **2,939 / 245** | 0.93 |
| 8,192², one tile, 9/7 | 1,612 / 813 | 1,539 / 792 | 1,508 / 792 | 1,006 / 792 | **983 / 792** | 0.61 |
| 16,384², 256 tiles, 9/7 | 5,578 / 3,134 | 5,149 / 3,130 | 5,144 / 3,130 | 4,457 / 3,130 | **3,525 / 3,130** | 0.63 |

Through the stream API (two rounds; load 21 % mean, 35 % max):

| Image | OpenJPEG ms / MB | main ms / MB | stage 4 ms / MB |
|---|---|---|---|
| 4,096², one tile, 9/7 | 570 / 202 | 414 / 200 | 314 / 200 |
| 4,096², 16 tiles, 5/3 | 3,186 / 209 | 3,146 / 238 | 2,951 / 238 |
| 8,192², one tile, 9/7 | 1,622 / 802 | 1,583 / 791 | 995 / 791 |
| 16,384², 256 tiles, 9/7 | 5,531 / 3,096 | 5,017 / 3,131 | 3,417 / 3,131 |

Every decode is identical to OpenJPEG's. The stages add no memory: the row-wise wavelet needs a row and a bit
a row where OpenJPEG's structure needed eight floats a sample of the longest side, and the fused pass a 2 KiB
stack buffer. The stream API's higher peak on multi-tile images is stage 1c's (it holds the codestream; the
low-memory stream belongs to the j2k-tile module, owner 2026-11-20); the memory API meets the gate.

### Tier-1 (stage 4): what was tried

Each candidate against the one before it, interleaved, the 212 files whole (the Tier-1-bound classes):

| Candidate | 64² | 128² | 256² | 512² | 1024² | Kept |
|---|---|---|---|---|---|---|
| MQ renormalisation by count-leading-zeros, against stage 3 | 0.94 | 0.93 | 0.93 | 0.94 | 0.95 | yes: the same byte reads at the same shifts |
| Eight empty columns skipped at once (significance and refinement passes, AVX2), against the above | 1.015 | 1.004 | 1.011 | 1.021 | 1.012 | no: the per-column test is one load and a branch already |
| The LPS/MPS choice by selects instead of branches, against the renormalisation | 1.033 | 1.026 | 1.047 | 1.045 | 1.047 | no: the symbol is branched on right after, so the misprediction only moves |

The renormalisation takes 5-7 % off the request where Tier-1 is 65-86 % of it, so about 7-10 % of Tier-1, at the
plan's 10 % line. OpenJPEG's Tier-1 already has the state words and the MQ registers in locals that the
literature lists (its PR #783 and #945), so those were not candidates.

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
occupancy for the same divergence. Not adopted; the prototype and its numbers stay as the record.

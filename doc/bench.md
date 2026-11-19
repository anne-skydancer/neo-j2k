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
stream per request) and neo-j2k's own path (`neoj2k-memory`: one codec kept across requests, the header read
from memory and parsed once per main header, the decode into the viewer's 8-bit bottom-up buffer),
interleaved, on the same private bench corpus (212 files), pinned to core 7 of the Ryzen 9 9950X3D, MSVC 14.51
RelWithDebInfo; 2,050 configurations in 230 s. Load: other processes 8 % mean, 12 % p90, 20 % max of 8 CPUs.
Every decode is identical to the oracle's (the `same` column equals the file count in every class).

| Request (median per file, then over the class) | Files | Oracle µs | neo-j2k, compat. API µs | neo-j2k, memory µs | Memory / oracle | Memory path heap calls |
|---|---|---|---|---|---|---|
| 32² whole | 28 | 33.7 | 12.3 | **10.1** | 0.29 | 0 / 0 |
| 32² at reduce 3 | 28 | 24.6 | 6.39 | 4.17 | 0.17 | 0 / 0 |
| 32² at reduce 5 (1x1 floor) | 28 | 22.4 | 5.36 | 3.27 | 0.15 | 0 / 0 |
| 64² whole | 32 | 373 | 310 | 303 | 0.80 | 0 / 0 |
| 128² whole | 32 | 617 | 543 | 514 | 0.84 | 0 / 0 |
| 128² at reduce 5 | 32 | 26.7 | 9.03 | 6.69 | 0.25 | 0 / 0 |
| 256² whole | 40 | 2,267 | 2,146 | 2,045 | 0.90 | 0 / 0 |
| 256² at reduce 3 | 40 | 145 | 63.8 | 53.5 | 0.47 | 0 / 0 |
| 512² whole | 40 | 11,468 | 10,676 | 10,299 | 0.90 | 0 / 0 |
| 1024² whole | 40 | 37,669 | 34,397 | 34,137 | 0.92 | 1 / 1 |
| 1024² at reduce 5 | 40 | 296 | 206 | 177 | 0.65 | 0 / 0 |
| 1024², first 600 bytes | 34 | 67.1 | 26.9 | 23.9 | 0.36 | 0 / 0 |

The 32² whole request, stage 1c's target of 7-12 µs, takes 10.1 µs on neo-j2k's own path (9.4 µs in an earlier
run of the same day). Of what is left, a sampling profile of the memory decode gives Tier-1 about a third
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

| Image | OpenJPEG ms / MB | neo-j2k ms / MB | ratio | OpenJPEG from memory ms / MB | neo-j2k from memory ms / MB | ratio |
|---|---|---|---|---|---|---|
| 4,096², one tile, 9/7 | 384 / 202 | 346 / 200 | 0.90 | 384 / 205 | 344 / 200 | 0.90 |
| 4,096², 16 tiles, 5/3 | 3,213 / 209 | 3,135 / **239** | 0.98 | 3,208 / 248 | 3,132 / 245 | 0.98 |
| 8,192², one tile, 9/7 | 1,620 / 802 | 1,527 / 791 | 0.94 | 1,603 / 813 | 1,538 / 792 | 0.96 |
| 16,384², 256 tiles, 9/7 | 5,636 / 3,096 | 5,042 / **3,131** | 0.89 | 5,516 / 3,134 | 5,160 / 3,130 | 0.94 |

neo-j2k is faster everywhere and needs no more memory from memory. Through the stream API it holds the whole
codestream in memory before it decodes (the walk over the tile-parts reads the bytes in place), where
OpenJPEG holds one tile's data at a time: a multi-tile image then peaks higher by the size of its codestream
(30 MB, 35 MB above). Reading tile-parts from the stream as the walk reaches them, and dropping a tile's bytes
once it is decoded, would close that; it changes the stream input layer and is left for a later stage. Two
fixes came from these runs: the 9/7 Tier-1 store is vectorised (OpenJPEG's compiler vectorises its loop; 6 %
of the 8,192² decode), and clearing the caller's planes for a multi-tile image goes a row at a time (a memset
per sample had cost a second at 16,384²).

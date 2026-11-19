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
  counted) gives OpenJPEG's calls per request; neo-j2k's come from its allocator hook from stage 1c, where
  the gate is zero after warm-up.
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

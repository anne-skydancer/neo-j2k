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
- **Clock:** the TSC (nominal cycles), calibrated against `steady_clock` at start. Pin the thread with
  `--pin`.
- **Heap calls:** the counting oracle module (`nj2k_oracle_counting`, OpenJPEG's allocation entry points
  counted) gives OpenJPEG's calls per request; neo-j2k's come from its allocator hook from stage 1c, where
  the gate is zero after warm-up.
- Build the bench `Release` or `RelWithDebInfo`.

## Stage 0 baseline (2026-11-19)

The oracle (static, MSVC 14.51 RelWithDebInfo), the shipped `openjp2.dll` (VS 2022, LTCG) and the counting
oracle, interleaved, on the owner's private bench corpus (212 files: 188 Second Life textures and 24
generated 5/3 controls), the viewer's request, pinned to core 7 of a Ryzen 9 9950X3D (8 logical CPUs
visible). **Loaded run:** other processes took 87 % of the CPUs on average (other agents' builds: cl, link,
MSBuild, GCC, ninja), so the absolute times are indicative only; the ratios and counts are what to compare.
The earlier quiet bench measured 21.8 µs for the 1x1 floor and 29.7-35.4 µs for 32² (`small-texture-stale`
bench in the viewer's reports).

| Request (median per file, then over the class) | Files | Oracle µs | Shipped µs | Shipped / oracle | OpenJPEG heap calls (allocations / frees) |
|---|---|---|---|---|---|
| 32² whole | 28 | 48.7 | 48.6 | 1.00 | 554 / 579 |
| 32² at reduce 5 (1x1 floor) | 28 | 23.1 | 22.2 | 1.00 | 490 / 492 |
| 64² whole | 32 | 423 | 448 | 1.06 | 616 / 638 |
| 128² whole | 32 | 827 | 852 | 1.03 | 616 / 640 |
| 256² whole | 40 | 3,267 | 3,355 | 1.02 | 722 / 775 |
| 256² at reduce 3 | 40 | 175 | 194 | 1.08 | 650 / 667 |
| 512² whole | 40 | 15,201 | 15,752 | 1.03 | 1,033 / 1,152 |
| 1024² whole | 40 | 56,817 | 57,105 | 1.02 | 2,360 / 2,729 |
| 1024² at reduce 5 | 40 | 324 | 303 | 0.98 | 1,450 / 1,454 |
| 1024², first 600 bytes | 34 | 82.8 | 78.5 | 0.91 | 1,450 / 1,452 |

The heap calls are those of a whole viewer request (header pass and decode pass). Upload encodes
(`JPEG2KEncode::encode`, 23 textures of 32, 128 and 1024 px, lossy and lossless): the oracle's bytes equal the
shipped DLL's in all 46 encodes; 32² lossy 82 µs, 128² 1.0 ms, 1024² 60 ms (loaded). The per-file data
names private textures, so it stays off the repository.

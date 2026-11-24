<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026, Anne Skydancer (neo-j2k) -->

# Local Linux benchmarks of neo-j2k C++ against OpenJPEG 2.5.4 (2026-11-24)

The benchmarks the README names, run on Linux on the owner's own machine, for the tables the README reports from a shared
Windows machine ("It was not quiet: other jobs shared it"). OpenJPEG is the pinned oracle (`doc/oracle.md`). Every decode
gave one hash for every run of both libraries, and every encode gave identical bytes. The change is neo-j2k C++'s time
against OpenJPEG's (minus is faster).

## Machine and method

| | |
|---|---|
| Commit | neo-j2k `main` 50f74d2 |
| CPU | AMD Ryzen 9 9950X3D, 8 logical CPUs given to the WSL2 virtual machine |
| System | Arch Linux in WSL2 (kernel 6.18); the Windows host carried the owner's own use: 3 % to 27 % CPU in the samples before the runs and 3 % to 15 % after, with one 54 % sample after the first pair of runs |
| Compiler | GCC 16.2.1, preset `linux-gcc` (AVX2, the README's configuration), the preset's own flags, benches on |
| Drivers | `bench/run_large.py` with `nj2k_large` (the gate, from memory, thread 1 pinned to CPU 7; the idle host through its `--sat` mode with one host thread); `bench/encode_bench.py` with `nj2k_encode_bench` (a fresh process for each library and size) |
| Load the harness saw | the large-image gate 21 % mean (33 % max) and the multi-tile rows 24 % mean (49 % max): the figure includes the benchmark's own busy threads; the idle-host runs 15 % to 17 % mean |

Raw output is in `~/bench-out/cpp-20261124-*` of the WSL. The scripts that ran this (`~/bin/nj2k-cpp-build.sh`,
`~/bin/nj2k-cpp-bench.sh`) are not in the repository.

## Large images, threads lent by the host (README stage 5)

Time and peak memory, from memory, medians of three rounds (the multi-tile images of 16,384² too; the harness caps 25 minutes).
The last column is the README's number for the same row (Windows, MSVC, AVX2, 26 % mean load), where it has one.

| Image | Threads | OpenJPEG ms / MB | neo-j2k ms / MB | Change | README (Windows) |
|---|---:|---|---|---:|---:|
| 4,096², one tile, 9/7 | 1 | 426 / 208 | 246 / 201 | -42.3 % | -40.0 % |
| 4,096², one tile, 9/7 | 4 | 297 / 208 | 81 / 201 | -72.7 % | - |
| 4,096², one tile, 9/7 | 8 | 188 / 209 | 55 / 201 | -70.7 % | -66.0 % |
| 4,095 × 4,097, one tile, 9/7 | 1 | 370 / 208 | 250 / 201 | -32.4 % | - |
| 4,095 × 4,097, one tile, 9/7 | 4 | 284 / 208 | 80 / 201 | -71.8 % | - |
| 4,095 × 4,097, one tile, 9/7 | 8 | 172 / 209 | 55 / 201 | -68.0 % | - |
| 8,192², one tile, 9/7 | 1 | 1,757 / 813 | 1,002 / 791 | -43.0 % | -40.0 % |
| 8,192², one tile, 9/7 | 4 | 1,268 / 814 | 361 / 792 | -71.5 % | - |
| 8,192², one tile, 9/7 | 8 | 750 / 815 | 268 / 792 | -64.3 % | -58.0 % |
| 8,191 × 8,193, one tile, 9/7 | 1 | 1,550 / 814 | 1,000 / 791 | -35.5 % | - |
| 8,191 × 8,193, one tile, 9/7 | 4 | 1,151 / 814 | 354 / 792 | -69.2 % | - |
| 8,191 × 8,193, one tile, 9/7 | 8 | 708 / 815 | 253 / 792 | -64.3 % | - |
| 4,096², 16 tiles, 5/3 | 1 | 3,386 / 245 | 2,162 / 240 | -36.1 % | -8.0 % |
| 4,096², 16 tiles, 5/3 | 4 | 1,018 / 249 | 580 / 240 | -43.0 % | - |
| 4,096², 16 tiles, 5/3 | 8 | 582 / 250 | 334 / 240 | -42.6 % | -14.0 % |
| 16,384², 256 tiles, 9/7 | 1 | 6,987 / 3,132 | 3,835 / 3,127 | -45.1 % | -37.0 % |
| 16,384², 256 tiles, 9/7 | 4 | 4,903 / 3,131 | 1,438 / 3,127 | -70.7 % | - |
| 16,384², 256 tiles, 9/7 | 8 | 4,516 / 3,132 | 1,371 / 3,127 | -69.6 % | -63.0 % |

neo-j2k is faster at every thread count on every image, and needs no more memory (201 MB against 208 at 4,096², 791 against 813 at
8,192², 3,127 against 3,132 at 16,384²). The multi-tile 5/3 rows gain much more on Linux than the README's Windows rows
(-36 % and -43 % against -8 % and -14 %); OpenJPEG's own time is about the same here as in the README (3,386 ms against 3,203 ms), so
the difference is on neo-j2k's side, and nothing here says why.

## The idle host: one decode at a time (the README's former "Busy or idle" table, stage 5b)

The README no longer carries this table (removed on 2026-11-24 as provisional); `doc/bench.md` keeps the Windows runs, and the Linux
rows below are kept here.

One host thread decoding the same RGB 9/7 one-tile image again and again: no executor, or a pool of 4 or 8 lent (the library's own
thresholds). Decodes a second; in brackets the change in time per decode (minus is faster). The "no executor" column is the mean of
the two runs of that side; the last column is the README's (no executor / 4 lent / 8 lent).

| Image | No executor | 4 lent | 8 lent | README (Windows) |
|---|---:|---:|---:|---|
| 512² RGB | 192.6 | 250.8 (-23.1 %) | 249.7 (-23.0 %) | 174.2 / 254.6 / 278.5 |
| 1,024² RGB | 48.3 | 70.8 (-31.6 %) | 75.3 (-36.1 %) | 43.7 / 51.9 / 74.8 |
| 2,048² RGB | 12.0 | 19.1 (-37.2 %) | 21.0 (-43.0 %) | 9.7 / 15.5 / 19.0 |

The former README cell "1,024² with 4 threads lent: too noisy to claim" is a clear gain here (70.8 against 48.4 decodes a second, one
run of three rounds). The no-executor side repeated within 1 % between the pool-of-4 and the pool-of-8 runs (48.4 and 48.2). The 512² gain is smaller than the README's
(-23 % against -31 % and -37 %).

## The upload encode against OpenJPEG, one thread (README "The upload encode, one thread")

The viewer's upload sequence (RLCP, 64 × 64 code-blocks, 5/3 lossless or 9/7 lossy), a synthetic three-component texture, a fresh
process for each library and size, three runs; the change is the median of the three runs' own changes (their lowest and highest in
brackets); times are the medians of the runs, in ms; the last column is the peak memory, OpenJPEG / neo-j2k, in MB.

| Size | Mode | Bytes | Identical | OpenJPEG ms | neo-j2k ms | Change | Peak MB |
|---|---|---:|---|---:|---:|---|---|
| 64² RGB | lossy | 1515 | yes | 1.0 | 0.6 | -40.0 % (-40.0 to -40.0) | 1.4 / 0.7 |
| 64² RGB | lossless | 8521 | yes | 0.9 | 0.5 | -44.4 % (-44.4 to -44.4) | 1.3 / 0.7 |
| 128² RGB | lossy | 5756 | yes | 3.4 | 2.2 | -35.3 % (-38.2 to -35.3) | 1.7 / 1.1 |
| 128² RGB | lossless | 32798 | yes | 3.5 | 2.1 | -40.0 % (-40.0 to -40.0) | 1.6 / 1.0 |
| 256² RGB | lossy | 24385 | yes | 13.1 | 8.2 | -37.4 % (-38.8 to -36.6) | 2.9 / 2.4 |
| 256² RGB | lossless | 129529 | yes | 14.1 | 8.5 | -39.3 % (-39.7 to -38.9) | 3.1 / 2.7 |
| 512² RGB | lossy | 98295 | yes | 61.2 | 38.4 | -37.6 % (-38.1 to -36.3) | 8.8 / 8.1 |
| 512² RGB | lossless | 595252 | yes | 65.0 | 39.5 | -39.5 % (-39.6 to -38.4) | 9.6 / 9.2 |
| 1,024² RGB | lossy | 393225 | yes | 247.7 | 158.7 | -36.5 % (-36.8 to -35.8) | 32.5 / 30.9 |
| 1,024² RGB | lossless | 2398977 | yes | 263.1 | 161.3 | -38.3 % (-39.0 to -38.2) | 36.3 / 35.0 |
| 2,048² RGB | lossy | 1572812 | yes | 1023.0 | 645.4 | -36.8 % (-37.4 to -36.7) | 113.2 / 121.9 |
| 2,048² RGB | lossless | 9493774 | yes | 1057.6 | 647.5 | -38.6 % (-39.2 to -38.4) | 124.1 / 138.2 |

neo-j2k is 36 % to 42 % faster on every row, with the same bytes, and the three runs agree within about 3 points. Its peak memory is
lower up to 1,024² and higher at 2,048² (122 MB against 113 lossy, 138 against 124 lossless). The README's 1,024² rows (Windows):
-32.8 % lossy and -38.2 % lossless.

## Not run, and limits

- The small-image tables (one thread, real textures) need the owner's private texture corpus, which is not on this machine. They were not run.
- The openjp2 module's API-level comparison (`bench/api_bench.py`) and the viewer-request bench were not run.
- Only the AVX2 configuration was run (the README's); the scalar preset is built but was not timed.
- One machine, one session, three rounds or runs per row. WSL2 is a virtual machine: CPU-bound times are close to native but not
  identical, and the hardware counters are not available.
- These are not the README's Windows numbers: other system, compiler (GCC 16.2 against MSVC 14.51) and load.

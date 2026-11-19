<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026, Anne Skydancer (neo-j2k) -->

# Fuzz regression inputs

An input a fuzzer found a defect with lands here once the code under test is fixed, named
`<target>-<finding>.bin` (for example `decode-ppm-eph-cut.bin`). CTest replays this folder through
every fuzz target on every platform (`fuzz_replay.*`, with the seeds), so a fixed defect stays fixed.

An input whose defect is still open, in the oracle or in neo-j2k, does not belong here when the
target would only skip it. It goes to `tests/known-defects` with a check that it still reproduces (the
check fails once the defect is gone) and an entry in `doc/oracle.md`. An input the target still runs,
checking less of it because of an open oracle defect, stays here, and its known-defect check replays
it with the guard off.

| Input | Finding |
|---|---|
| `roundtrip-guard-bit-overflow.bin` | ClusterFuzzLite on the stage-0 pull request: a lossless round trip of the oracle came back with other samples. The harness asked for losslessness where OpenJPEG's encoder writes a stream that cannot hold the image (OD-6, too few guard bits); the target now encodes and decodes such inputs without comparing them. `oracle_known_defect.od6` replays it with the guard off. |
| `header-jp2-pclr-values-before-size.bin` | Stage 1b, the header target: a JP2 palette box declaring 400 entries of 255 columns in 774 bytes; the reader allocated the 408 KB of values before checking the box holds them. |
| `header-jp2-image-box-before-ftyp.bin` | Stage 1b's review round, ClusterFuzzLite with UBSan (run 44526231005), the header target comparing with the oracle: a JP2 file with a bits-per-component box where the file type box belongs. OpenJPEG reads it (its skip of the misplaced box sets every state bit, DP-11), neo-j2k refuses it, as the standard has it; the target did not know the departure. It now does, and replays this input. |

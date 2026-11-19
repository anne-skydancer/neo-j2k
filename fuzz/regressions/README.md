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

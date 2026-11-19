<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026, Anne Skydancer (neo-j2k) -->

# Fuzz regression inputs

An input a fuzzer found a defect with lands here once the code under test is fixed, named
`<target>-<finding>.bin` (for example `decode-ppm-eph-cut.bin`). CTest replays this folder through
every fuzz target on every platform (`fuzz_replay.*`, with the seeds), so a fixed defect stays fixed.

An input whose defect is still open, in the oracle or in neo-j2k, does not belong here: it would only
replay as a skip. It goes to `tests/known-defects` with a check that it still reproduces (the check
fails once the defect is gone) and an entry in `doc/oracle.md`. The folder holds no input yet.

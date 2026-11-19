<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026, Anne Skydancer (neo-j2k) -->

# Fuzz regression inputs

Every input a fuzzer found a defect with lands here once the defect is fixed, named
`<target>-<finding>.bin` (for example `decode-o1-ppm-eph-cut.bin`). CTest replays this folder and
`corpus/generated` through every fuzz target on every platform (`fuzz_replay.*`), so a fixed defect
stays fixed. Inputs that need a fix in the oracle's pin go to `doc/oracle.md` instead.

<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026, Anne Skydancer (neo-j2k) -->

# Inputs of the oracle's open defects

Each input reproduces a defect of the oracle at its pin (`oracle/PIN`, `doc/oracle.md`), and a test
checks that it still does. A check fails once its defect is gone, so the list follows the pin: remove
the entry, and move a fuzz-found input to `fuzz/regressions` when neo-j2k is held to it.

| Input | Defect | Check |
|---|---|---|
| `od3-psot-1g.j2k` | OD-3: a tile-part's declared length (1 GiB) is allocated before it is read | `oracle_known_resources` (counting oracle module, builds without sanitizers): the viewer's request asks the heap for at least 1,000 MiB |
| `od5-fullres-cblk4-2048-rgba.j2k` | OD-5: code-block and precinct structures of every resolution for a reduced decode | `oracle_known_resources`: the viewer's request at reduce 5 asks the heap for at least 300 MiB |

`od3` and `od5` are made by `fuzz/make_inputs.py --known-defects`, from `corpus/generated/sl_32_rgb_97.j2k`
and from nothing. Both are synthetic resource bombs: nothing but these checks may decode them.

The encoder's defects OD-4 and OD-6 are fixed at the pin (the fork's `e64db843` and `841b2de4`). Their
inputs are round-trip regressions now, replayed with every sample compared:
`fuzz/regressions/roundtrip-mqc-termall-16bit.bin` (OD-4, the round-trip fuzzer's own find, moved from
here) and `fuzz/regressions/roundtrip-guard-bit-overflow.bin` (OD-6, an image the encoder now refuses).

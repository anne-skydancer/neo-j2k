<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026, Anne Skydancer (neo-j2k) -->

# Inputs of the oracle's open defects

Each input reproduces a defect of the oracle at its pin (`oracle/PIN`, `doc/oracle.md`), and a test
checks that it still does. A check fails once its defect is gone, so the list follows the pin: remove
the entry, and move a fuzz-found input to `fuzz/regressions` when neo-j2k is held to it.

| Input | Defect | Check |
|---|---|---|
| `od3-psot-1g.j2k` | OD-3: a tile-part's declared length (1 GiB) is allocated before it is read | `oracle_known_resources` (counting oracle module, builds without sanitizers): the viewer's request asks the heap for at least 1,000 MiB |
| `od4-roundtrip-mqc-termall-16bit.bin` | OD-4: the MQ encoder writes past the code-block buffer (TERMALL, 16-bit) | `oracle_known_defect.od4` (AddressSanitizer builds): the round-trip target with its guard off must report the heap-buffer-overflow |
| `od5-fullres-cblk4-2048-rgba.j2k` | OD-5: code-block and precinct structures of every resolution for a reduced decode | `oracle_known_resources`: the viewer's request at reduce 5 asks the heap for at least 300 MiB |

`od3` and `od5` are made by `fuzz/make_inputs.py --known-defects`, from `corpus/generated/sl_32_rgb_97.j2k`
and from nothing; `od4` is the round-trip fuzzer's own find. All are synthetic. They are resource bombs
and a heap overflow: nothing but these checks may decode or encode them.

OD-6's input is not here: the round-trip target runs it as a regression (without the comparison the
defect spoils), so it stays in `fuzz/regressions/roundtrip-guard-bit-overflow.bin`, and
`oracle_known_defect.od6` replays it with the guard off, on every compiler and platform: the samples
must still come back different.

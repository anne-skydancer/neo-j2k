# E3 on clang: the follow-up that did not pay (archive)

Status: closed, 2026-11-23. Archived at the tags `archive/bench-clang-linux` (variants harness and the Linux
matrix workflow) and `archive/enc-e3-clang` (the `encode_pairs` change, offered as PR 28).

## Question

E3 (refinement pass as a branch-free (context, bit) pair list, PR 27) gains 4 to 6 % on MSVC and GCC but only
about 2 % on clang lossless (same-machine Linux: clang AVX2 -2.0 %, clang SCALAR -2.1 %; gate 3 %). Can a change
reach 3 % on clang without regressing MSVC or GCC?

## Finding in the generated code (clang 22.1.3 -O2)

In the refinement MQ loop the coder's state went through memory on every decision: `a` loaded from the struct and
stored back, `c`, `ct`, `cx` stored each iteration, the `t1` pointer reloaded three times. Cause: aliasing, since
the byte stores (`ctxs[cx]`, `*bp`, the list) may alias the struct. A plain struct copy did not fix it (the
variable index into `ctxs[19]` kept clang from splitting the copy); plain scalar locals did. With `encode_pairs`
`a`, `c` and `ct` stay in registers on clang and GCC.

## Variants (bit mask `NJ2K_ENC_VARIANT`)

1 = `encode_pairs` (local MQ state); 2 = row-bit simplification; 4 = magnitude contexts 14-16 in registers;
8 = significance and cleanup passes on a local copy; 16 = `unlikely` on the byte-out loop. Tested: v1, v3 (=1+2),
v7 (=1+2+4), v11 (=1+2+8), v19 (=1+2+16), v0 = E3's own code as the noise control.

## Results, Linux same machine per job (run 45076814024)

Change in kernel time against E3, lossless / lossy, minus is faster; median of three runs of 41 rounds.

| Job (CPU) | v0 (noise) | v1 | v3 | v7 | v11 | v19 |
|---|---|---|---|---|---|---|
| clang AVX2 (EPYC 9V45) | +0.1 / +0.2 | +0.5 / +0.7 | +1.6 / +1.8 | +0.7 / +1.4 | +2.3 / +2.0 | +1.4 / +1.3 |
| clang SCALAR (EPYC 9V74) | -0.2 / -0.1 | +0.8 / +0.8 | +2.5 / +2.4 | +4.1 / +3.9 | +5.1 / +5.4 | +2.9 / +2.6 |
| gcc-14 AVX2 (Xeon 8370C) | +0.1 / -0.3 | -1.0 / -1.1 | -0.7 / -0.7 | +5.3 / +4.3 | +4.9 / +5.6 | -1.9 / -2.1 |
| gcc-14 SCALAR (EPYC 9V74) | +0.4 / +0.3 | -0.7 / -0.3 | -1.0 / -0.6 | +1.3 / +2.3 | +1.5 / +2.1 | -1.0 / -0.9 |
| gcc-15 AVX2 (EPYC 7763) | +2.1 / +1.0 | +1.2 / +0.8 | +1.1 / +0.8 | +0.9 / +2.1 | +1.8 / +1.3 | -0.5 / -0.3 |
| gcc-15 SCALAR (EPYC 9V45) | +1.9 / +2.2 | +1.2 / +1.2 | -0.2 / -0.2 | +4.4 / +5.7 | +4.6 / +4.3 | +2.8 / +2.4 |

Local (1,024², lossy / lossless, 5 rounds, pinned, interleaved), `encode_pairs` (v1) against E3: clang-cl AVX2
-0.3 / +0.1; clang-cl SCALAR -0.2 / -0.6; MSVC AVX2 +0.1 / +0.2; MSVC SCALAR -1.4 / -1.2; MinGW GCC 13.2 AVX2
-0.3 / -0.9; GCC SCALAR +0.6 / +0.5. Variant 7 on MSVC -1.4 / -2.4 but +1.6 / +2.9 on GCC; variant 16 +4 % to
+5 % on MSVC.

## Verdict

| Gate | Result |
|---|---|
| clang lossless and lossy at least 3.0 % faster than main | No (every variant slower than E3 on Linux clang) |
| Faster than E3 beyond noise on MSVC and GCC, no regression | No (no variant helps every compiler) |
| Byte-identical output | Yes |
| Leaner (`.text` not larger) | No: `enc_t1` text +0.8 % clang (+152 B), +2.1 % MSVC AVX2 (+534 B), +2.3 % MSVC SCALAR (+580 B) |

The aliasing store-forward is real but is not what limits the speed. The remaining chain is probably the context
state byte (load, table lookup, store, next decision on the same context), which only variant 7 touches, and that
variant is the one that costs most. Layout noise is about +-1 point locally (identical assembly timed 0.5 to 0.8
points apart; one build read +4 % until relinked) and up to 2 points on a Zen 3 runner.

## What was not tried

Attacking the context-state byte chain directly (the planner judged the prior weak); a 2-way interleave of the MQ
loop (the coder is serial in `a`, `c` and each context's state); other packed pair formats (off the dependency
chain); unrolling or `#pragma clang loop` (bound by the chain, not by loop overhead).

## Reproduce

Check out the tag `archive/bench-clang-linux` (a push to a branch of that name runs the matrix, or dispatch
`bench-clang-linux.yml` with `variants` such as `0 1 3 7 11 19`; `3p1` adds a layout perturbation). The
`encode_pairs` change on its own is PR 28 and the tag `archive/enc-e3-clang`.

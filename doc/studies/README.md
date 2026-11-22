<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026, Anne Skydancer (neo-j2k) -->

# Studies: the measured designs behind the Tier-1 and MQ work

These documents record the studies, with their measurements, that led to the encoder MQ coder, the decoder
MQ-NG, and the encoder steps E1 to E3 (`doc/bench.md`, Stages 6c to 6g). Each states what was measured, what was
read in the code and what was inferred. The owner's decisions are in the two decision documents.

| Document | Study |
|---|---|
| `enc_t1_study.md` | Encoder MQ coder and Tier-1: count-leading-zeros renormalisation, one-byte state index and selects |
| `mqng_study.md` | The MQ-NG decoder: state words, select decision, runs of the cleanup pass |
| `ebcotng_study.md` | EBCOT-NG Tier-1: bit-packed state against targeted steps; encoder steps E1 to E3, decoder step D1 |
| `e3_clang_study.md` | E3 on clang: variants of the refinement MQ loop (closed, no gain) |
| `mq-encoder-redesign-decisions.md` | The decisions taken for the encoder MQ redesign |
| `ngcodec-program-alternative.md` | The alternative program (the NG briefs) compared with the staged plan, and the decisions |

## The harnesses are archived, not in the tree

The studies were run with standalone benchmark projects (`bench/t1_study/`, `bench/mqng_study/`,
`bench/ebcotng_study/`). They build the library's sources from `../../src` together with drop-in copies of the
encoder and decoder files as they were, so they build only against the commit they were written for: `main` at
`bee41d4` (before the MQ redesign). They are not in this tree because `src/` has been rewritten since, and no CI
job builds them. They are kept at the tag `archive/studies`:

```
git worktree add ../neo-j2k-studies archive/studies
```

The write-ups name the harness files they refer to (for example `bench/t1_study/driver.cpp`); those paths are in
that tag, not in this tree. The E3-on-clang matrix (the variants and its Linux workflow) is at the tag
`archive/bench-clang-linux`, and the `encode_pairs` change is at `archive/enc-e3-clang`.

## Method

TSC cycles, one thread pinned, interleaved rounds against an unchanged copy of the old source, ratios as medians of
per-round ratios. The noise band is about 3 % (the same source in another object gives 0.99 to 1.00; code layout
alone can swing results 2 to 10 %). Since stage 6f the repository's dispatch-only workflow
`.github/workflows/bench-kernel-compare.yml` compares two refs on the same Linux machine in one job; only ratios
within one job count, because GitHub's runners differ between jobs.

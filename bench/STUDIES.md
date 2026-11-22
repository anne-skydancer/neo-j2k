# Tier-1 / MQ studies: the benchmark harnesses

Status: study tooling, not part of the build. This branch (`studies`) starts from `main` at
bee41d4, the library the studies measured as "today's". Each study is a standalone CMake
project that builds the library's sources from `../../src` and its own prototypes.

| Folder | Study | Document |
|---|---|---|
| `bench/t1_study/` | Encoder MQ coder and Tier-1 (stages 1-2: count-leading-zeros renormalisation, state index plus selects) | `doc/enc_t1_study.md` |
| `bench/mqng_study/` | MQ-NG decoder (state words, select decision, AGG run) | `doc/mqng_study.md` |
| `bench/ebcotng_study/` | EBCOT-NG Tier-1 (bit-packed state; encoder steps E1-E3, decoder step D1) | `doc/ebcotng_study.md` |

Decisions taken from them: `doc/mq-encoder-redesign-decisions.md` and
`doc/ngcodec-program-alternative.md`. The resulting work is in pull requests #21 (encoder MQ),
#22 (decoder MQ-NG) and #23 (encoder E1).

## Method

TSC cycles, one thread pinned, interleaved rounds against an unchanged copy of the old source,
ratios as medians of per-round ratios. The noise band is about 3 % (the same source in another
object gives 0.99-1.00; code layout alone can swing results 2-10 %). Each study document states
which numbers were measured, which were read in the code and which were inferred.

## Not in these folders

- The block dumps the replay tools read (generated from the repository's corpus and study
  images with the harnesses' own dumpers; the decoder's committed vectors are in
  `tests/vectors/t1` on the #22 branch).
- Results on other hardware: everything was measured on one Ryzen 9 9950X3D (MSVC, plus
  clang-cl and MinGW GCC for some rows). The Linux numbers for the encoder come from
  `bench-enc-linux` (throwaway branch, `bench/nj2k_enc_t1_ratio.cpp`).

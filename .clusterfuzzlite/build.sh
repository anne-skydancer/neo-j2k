#!/bin/bash -eu
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Anne Skydancer (neo-j2k)
#
# ClusterFuzzLite build: the sanitizer and coverage flags come in CFLAGS/CXXFLAGS and the engine in
# LIB_FUZZING_ENGINE (fuzz/CMakeLists.txt reads it). Seeds: the generated corpus; dictionary:
# fuzz/j2k.dict.
cmake -S "$SRC/neo-j2k" -B "$WORK/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DNJ2K_BUILD_FUZZERS=ON -DNJ2K_BUILD_TESTS=OFF -DNJ2K_BUILD_TOOLS=OFF -DNJ2K_BUILD_BENCH=OFF \
  -DNJ2K_BUILD_SHARED=OFF -DNJ2K_INSTALL=OFF
cmake --build "$WORK/build" --parallel
for t in header decode roundtrip; do
  cp "$WORK/build/fuzz/nj2k_fuzz_$t" "$OUT/"
  cp "$SRC/neo-j2k/fuzz/j2k.dict" "$OUT/nj2k_fuzz_$t.dict"
done
zip -j -q "$OUT/nj2k_fuzz_decode_seed_corpus.zip" "$SRC"/neo-j2k/corpus/generated/*.j2k
cp "$OUT/nj2k_fuzz_decode_seed_corpus.zip" "$OUT/nj2k_fuzz_header_seed_corpus.zip"

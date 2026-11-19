# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Anne Skydancer (neo-j2k)
#
# The corpus generator (writes corpus/generated with the oracle's encoder).
add_executable(nj2k_corpus_gen nj2k_corpus_gen.cpp)
target_link_libraries(nj2k_corpus_gen PRIVATE nj2k_test_common)
nj2k_fp_strict(nj2k_corpus_gen)
nj2k_program_exceptions(nj2k_corpus_gen)

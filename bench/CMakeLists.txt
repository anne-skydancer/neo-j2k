# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Anne Skydancer (neo-j2k)
#
# The bench harness. Build it Release (or RelWithDebInfo) for timing; the counting oracle module
# (nj2k_oracle_counting, built next to it) gives OpenJPEG's heap calls per request.
add_executable(nj2k_bench nj2k_bench.cpp)
target_link_libraries(nj2k_bench PRIVATE nj2k_test_common)
nj2k_fp_strict(nj2k_bench)
nj2k_program_exceptions(nj2k_bench)
if(TARGET nj2k_oracle_counting)
  add_dependencies(nj2k_bench nj2k_oracle_counting)
endif()

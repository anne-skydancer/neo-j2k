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

# The large-image bench (one decode a process against the oracle at N threads, and the saturated
# host; bench/run_large.py drives it)
add_executable(nj2k_large nj2k_large.cpp)
target_link_libraries(nj2k_large PRIVATE nj2k_test_common)
if(WIN32)
  target_link_libraries(nj2k_large PRIVATE psapi)
endif()
nj2k_fp_strict(nj2k_large)
nj2k_program_exceptions(nj2k_large)

# The stage gates' kernel timings (the library's internals, from the static library)
add_executable(nj2k_kernels nj2k_kernels.cpp)
target_link_libraries(nj2k_kernels PRIVATE nj2k_test_common)
nj2k_fp_strict(nj2k_kernels)
nj2k_simd(nj2k_kernels)
nj2k_program_exceptions(nj2k_kernels)

# The decoder's Tier-1 against the frozen copy of it before the MQ-NG redesign (the library's internals)
add_executable(nj2k_t1_dec nj2k_t1_dec.cpp ../tests/unit/t1_frozen.cpp)
target_link_libraries(nj2k_t1_dec PRIVATE nj2k_test_common)
nj2k_fp_strict(nj2k_t1_dec)
nj2k_simd(nj2k_t1_dec)
nj2k_program_exceptions(nj2k_t1_dec)

# The lane-per-code-block MQ prototype (stage 4): MQ segments traced from real decodes, decoded
# again by the scalar MQ decoder and by eight AVX2 lanes, one code-block a lane
if(NJ2K_SIMD STREQUAL "AVX2")
  add_executable(nj2k_mq_lanes nj2k_mq_lanes.cpp)
  target_link_libraries(nj2k_mq_lanes PRIVATE nj2k_t1_trace)
  target_include_directories(nj2k_mq_lanes PRIVATE "${PROJECT_SOURCE_DIR}/include")
  nj2k_fp_strict(nj2k_mq_lanes)
  nj2k_simd(nj2k_mq_lanes)
  nj2k_program_exceptions(nj2k_mq_lanes)
endif()

# Stage 6a: the upload encode's time and peak memory, one encoder a process (bench/encode_bench.py)
add_executable(nj2k_encode_bench nj2k_encode_bench.cpp)
target_link_libraries(nj2k_encode_bench PRIVATE nj2k_test_common)
nj2k_fp_strict(nj2k_encode_bench)
nj2k_program_exceptions(nj2k_encode_bench)

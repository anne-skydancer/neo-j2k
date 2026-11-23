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

# The encoder's Tier-1 kernel against its frozen earlier version, one process, interleaved rounds (the Linux
# CI bench, .github/workflows/bench-enc-linux.yml): the library's internals, from the static library
add_executable(nj2k_enc_t1_ratio nj2k_enc_t1_ratio.cpp ../tests/unit/enc_t1_frozen.cpp)
target_link_libraries(nj2k_enc_t1_ratio PRIVATE neoj2k_static)
nj2k_fp_strict(nj2k_enc_t1_ratio)
nj2k_simd(nj2k_enc_t1_ratio)
nj2k_program_exceptions(nj2k_enc_t1_ratio)

# The openjp2 module as an application sees it (bench/api_bench.py): the libraries it loads by path are built first
if(TARGET neoj2k_openjp2)
  add_dependencies(nj2k_encode_bench neoj2k_openjp2)
  add_dependencies(nj2k_large neoj2k_openjp2)
endif()
if(TARGET nj2k_oracle_openjp2)
  add_dependencies(nj2k_encode_bench nj2k_oracle_openjp2)
  add_dependencies(nj2k_large nj2k_oracle_openjp2)
endif()
# The harness's own test: the encode through each library loaded by path gives the oracle's codestream, 1 and 2 threads
# (and the two libraries' decodes give one set of samples when both are built)
find_package(Python3 COMPONENTS Interpreter)
if(NJ2K_BUILD_TESTS AND Python3_FOUND AND TARGET neoj2k_openjp2)
  set(_nj2k_api_libs --lib "$<TARGET_FILE:neoj2k_openjp2>")
  if(TARGET nj2k_oracle_openjp2)
    list(APPEND _nj2k_api_libs --lib "$<TARGET_FILE:nj2k_oracle_openjp2>")
  endif()
  add_test(NAME api_bench_harness
           COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/api_bench.py" --check
                   --encode-exe "$<TARGET_FILE:nj2k_encode_bench>" --large-exe "$<TARGET_FILE:nj2k_large>" ${_nj2k_api_libs})
endif()

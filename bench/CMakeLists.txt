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

# bench-enc-linux only: the candidate formulations of the GCC path (NJ2K_ENC_GCC_VARIANT, src/enc_mq.h), one
# program each. They compile enc_t1.cpp themselves (with the library's flags), so the variant is theirs and
# not the library's; the frozen reference is compiled once.
option(NJ2K_BENCH_ENC_VARIANTS "Build one nj2k_enc_t1_ratio_vN per candidate formulation" OFF)
if(NJ2K_BENCH_ENC_VARIANTS)
  function(nj2k_variant_flags target)
    nj2k_warnings(${target})
    nj2k_fp_strict(${target})
    nj2k_simd(${target})
    nj2k_no_exceptions(${target})
    nj2k_hardening(${target})
  endfunction()
  add_library(nj2k_enc_t1_frozen OBJECT ../tests/unit/enc_t1_frozen.cpp)
  target_link_libraries(nj2k_enc_t1_frozen PRIVATE neoj2k_static)
  nj2k_variant_flags(nj2k_enc_t1_frozen)
  foreach(v RANGE 0 13)
    add_executable(nj2k_enc_t1_ratio_v${v} nj2k_enc_t1_ratio.cpp ../src/enc_t1.cpp $<TARGET_OBJECTS:nj2k_enc_t1_frozen>)
    target_link_libraries(nj2k_enc_t1_ratio_v${v} PRIVATE neoj2k_static)
    target_compile_definitions(nj2k_enc_t1_ratio_v${v} PRIVATE NJ2K_ENC_GCC_VARIANT=${v})
    nj2k_variant_flags(nj2k_enc_t1_ratio_v${v})
    nj2k_program_exceptions(nj2k_enc_t1_ratio_v${v})
  endforeach()
endif()

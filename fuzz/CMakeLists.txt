# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Anne Skydancer (neo-j2k)
#
# Fuzz targets. With NJ2K_BUILD_FUZZERS they link libFuzzer: nj2k_fuzz_<target> (Clang on Linux,
# where CI fuzzes; MSVC with /fsanitize=fuzzer for local runs on Windows). With the tests they also
# build against a plain driver, nj2k_fuzz_<target>_replay, and CTest replays the seeds (the corpus,
# the option-prefixed decode seeds and the JP2 header seeds of make_inputs.py, the round-trip seeds)
# and fuzz/regressions through them on every compiler and platform. The guards' own test checks
# the bombs of fuzz/bombs against the guards the targets give the oracle. Stage 5: the decode
# target decodes each input again with every stage split, through an executor that runs the parts
# one after the other (so the sanitizers see the threaded path), and compares; and
# nj2k_fuzz_decode_threads_replay, the decode target built with NJ2K_FUZZ_THREADS, does the same
# through a pool of threads (the ThreadSanitizer job replays the seeds and regressions through it).
set(NJ2K_FUZZ_TARGETS header decode roundtrip)

if(NJ2K_BUILD_FUZZERS)
  if(NJ2K_CL)
    # instrumentation for every object comes from the top level (NJ2K_FUZZ_COVERAGE); the engine's
    # main() is linked into the fuzz programs only
    set(_nj2k_engine "/fsanitize=fuzzer")
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND NJ2K_GNU_LIKE)
    # ClusterFuzzLite and OSS-Fuzz pass the engine in LIB_FUZZING_ENGINE and the sanitizer and
    # coverage flags in CFLAGS/CXXFLAGS; a local build asks for libFuzzer itself.
    if(DEFINED ENV{LIB_FUZZING_ENGINE})
      set(_nj2k_engine "$ENV{LIB_FUZZING_ENGINE}")
    else()
      set(_nj2k_engine "-fsanitize=fuzzer")
    endif()
  else()
    message(FATAL_ERROR "NJ2K_BUILD_FUZZERS needs Clang (libFuzzer) or MSVC")
  endif()
  separate_arguments(_nj2k_engine)
  foreach(t IN LISTS NJ2K_FUZZ_TARGETS)
    add_executable(nj2k_fuzz_${t} fuzz_${t}.cpp)
    target_link_libraries(nj2k_fuzz_${t} PRIVATE nj2k_test_common)
    if(NJ2K_CL)
      target_compile_options(nj2k_fuzz_${t} PRIVATE /fsanitize=fuzzer)
    endif()
    target_link_options(nj2k_fuzz_${t} PRIVATE ${_nj2k_engine})
    nj2k_fp_strict(nj2k_fuzz_${t})
    nj2k_program_exceptions(nj2k_fuzz_${t})
  endforeach()
endif()

if(NJ2K_BUILD_TESTS)
  set(_nj2k_corpus "${PROJECT_SOURCE_DIR}/corpus/generated")
  set(_nj2k_regressions "${CMAKE_CURRENT_SOURCE_DIR}/regressions")
  foreach(t IN LISTS NJ2K_FUZZ_TARGETS)
    add_executable(nj2k_fuzz_${t}_replay fuzz_${t}.cpp standalone_main.cpp)
    target_link_libraries(nj2k_fuzz_${t}_replay PRIVATE nj2k_test_common)
    nj2k_fp_strict(nj2k_fuzz_${t}_replay)
    nj2k_program_exceptions(nj2k_fuzz_${t}_replay)
  endforeach()
  add_executable(nj2k_fuzz_decode_threads_replay fuzz_decode.cpp standalone_main.cpp)
  target_link_libraries(nj2k_fuzz_decode_threads_replay PRIVATE nj2k_test_common)
  target_compile_definitions(nj2k_fuzz_decode_threads_replay PRIVATE NJ2K_FUZZ_THREADS=1)
  nj2k_fp_strict(nj2k_fuzz_decode_threads_replay)
  nj2k_program_exceptions(nj2k_fuzz_decode_threads_replay)

  # The decode seeds with option bytes (viewer mode, reduce levels, cuts, layer limits) and the
  # header target's JP2 seeds are made from the corpus by make_inputs.py, as the fuzz jobs make them.
  find_package(Python3 COMPONENTS Interpreter)
  set(_nj2k_decode_seeds)
  set(_nj2k_header_seeds)
  if(Python3_FOUND)
    set(_nj2k_decode_seeds "${CMAKE_CURRENT_BINARY_DIR}/seeds-decode")
    add_test(NAME fuzz_seeds.decode
             COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/make_inputs.py"
                     --decode-seeds "${_nj2k_decode_seeds}" --corpus "${_nj2k_corpus}")
    set_tests_properties(fuzz_seeds.decode PROPERTIES FIXTURES_SETUP nj2k_decode_seeds)
    set(_nj2k_header_seeds "${CMAKE_CURRENT_BINARY_DIR}/seeds-header")
    add_test(NAME fuzz_seeds.header
             COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/make_inputs.py"
                     --header-seeds "${_nj2k_header_seeds}" --corpus "${_nj2k_corpus}")
    set_tests_properties(fuzz_seeds.header PROPERTIES FIXTURES_SETUP nj2k_header_seeds)
  endif()
  add_test(NAME fuzz_replay.header
           COMMAND nj2k_fuzz_header_replay "${_nj2k_corpus}" "${_nj2k_regressions}" ${_nj2k_header_seeds})
  if(_nj2k_header_seeds)
    set_tests_properties(fuzz_replay.header PROPERTIES FIXTURES_REQUIRED nj2k_header_seeds)
  endif()
  add_test(NAME fuzz_replay.decode
           COMMAND nj2k_fuzz_decode_replay "${_nj2k_corpus}" "${_nj2k_regressions}" ${_nj2k_decode_seeds})
  if(_nj2k_decode_seeds)
    set_tests_properties(fuzz_replay.decode PROPERTIES FIXTURES_REQUIRED nj2k_decode_seeds)
  endif()
  add_test(NAME fuzz_replay.decode_threads
           COMMAND nj2k_fuzz_decode_threads_replay "${_nj2k_corpus}" "${_nj2k_regressions}" ${_nj2k_decode_seeds})
  if(_nj2k_decode_seeds)
    set_tests_properties(fuzz_replay.decode_threads PROPERTIES FIXTURES_REQUIRED nj2k_decode_seeds)
  endif()
  # the round-trip target reads coding parameters, not codestreams: its own seeds
  add_test(NAME fuzz_replay.roundtrip
           COMMAND nj2k_fuzz_roundtrip_replay "${CMAKE_CURRENT_SOURCE_DIR}/seeds/roundtrip" "${_nj2k_regressions}")

  add_executable(nj2k_fuzz_filter_check filter_check.cpp)
  target_link_libraries(nj2k_fuzz_filter_check PRIVATE nj2k_test_common)
  nj2k_program_exceptions(nj2k_fuzz_filter_check)
  add_test(NAME fuzz_filters COMMAND nj2k_fuzz_filter_check "${CMAKE_CURRENT_SOURCE_DIR}/bombs" "${_nj2k_corpus}")
endif()

# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Anne Skydancer (neo-j2k)
cmake_minimum_required(VERSION 3.25)

# One version, in the public header (NJ2K_VERSION_*).
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/neoj2k/neoj2k.h" _nj2k_version_lines
     REGEX "^#define NJ2K_VERSION_(MAJOR|MINOR|PATCH) [0-9]+$")
foreach(_line IN LISTS _nj2k_version_lines)
  string(REGEX MATCH "NJ2K_VERSION_([A-Z]+) ([0-9]+)" _ "${_line}")
  set(_nj2k_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
endforeach()
project(neoj2k VERSION ${_nj2k_MAJOR}.${_nj2k_MINOR}.${_nj2k_PATCH}
        DESCRIPTION "JPEG 2000 Part 1 codec" LANGUAGES C CXX)

option(NJ2K_BUILD_STATIC "Build the static library" ON)
option(NJ2K_BUILD_SHARED "Build the shared library (neoj2k.dll, libneoj2k.so)" ON)
option(NJ2K_BUILD_TESTS "Build the oracle, the harness and the tests" ${PROJECT_IS_TOP_LEVEL})
option(NJ2K_BUILD_TOOLS "Build the corpus generator" ${PROJECT_IS_TOP_LEVEL})
option(NJ2K_BUILD_BENCH "Build the bench harness" ${PROJECT_IS_TOP_LEVEL})
option(NJ2K_BUILD_FUZZERS "Build libFuzzer targets (Clang, or MSVC with /fsanitize=fuzzer)" OFF)
option(NJ2K_WERROR "Treat warnings in neo-j2k's own code as errors" OFF)
option(NJ2K_MSVC_STATIC_RUNTIME "Link the static C runtime (/MT) instead of the DLL one (/MD)" OFF)
option(NJ2K_INSTALL "Generate install rules" ${PROJECT_IS_TOP_LEVEL})
option(NJ2K_PROFILE_OPENSIM "Build the OpenSim decoder limit profile (NJ2K_DECODER_LIMITS_OPENSIM_TEXTURE)" ON)
set(NJ2K_SIMD "AVX2" CACHE STRING "Instruction-set floor of neo-j2k's own code: AVX2 (x86-64-v3) or SCALAR (x86-64)")
set_property(CACHE NJ2K_SIMD PROPERTY STRINGS AVX2 SCALAR)
set(NJ2K_SANITIZE "" CACHE STRING "Sanitizers for every target: address;undefined;memory;thread")
if(NOT NJ2K_SIMD MATCHES "^(AVX2|SCALAR)$")
  message(FATAL_ERROR "NJ2K_SIMD must be AVX2 or SCALAR, not '${NJ2K_SIMD}'")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(NJ2K_MSVC_STATIC_RUNTIME)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(NJ2KFlags)

if(MSVC)
  # The library compiles without exceptions (/EHs-c-), so /EHsc leaves the global flags and the
  # test programs ask for it themselves; RelWithDebInfo inlines as Release does.
  string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  foreach(_lang C CXX)
    string(REPLACE "/Ob1" "/Ob2" CMAKE_${_lang}_FLAGS_RELWITHDEBINFO "${CMAKE_${_lang}_FLAGS_RELWITHDEBINFO}")
  endforeach()
endif()
# C++ programs that use exceptions (tests, tools, bench, fuzz drivers)
function(nj2k_program_exceptions target)
  if(MSVC)
    target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
  endif()
endfunction()

add_subdirectory(src)

if(NJ2K_BUILD_TESTS OR NJ2K_BUILD_TOOLS OR NJ2K_BUILD_BENCH OR NJ2K_BUILD_FUZZERS)
  add_subdirectory(oracle)
  add_subdirectory(tests/common)
endif()
if(NJ2K_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(NJ2K_BUILD_TOOLS)
  add_subdirectory(tools)
endif()
if(NJ2K_BUILD_BENCH)
  add_subdirectory(bench)
endif()
if(NJ2K_BUILD_TESTS OR NJ2K_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()

message(STATUS "neo-j2k ${PROJECT_VERSION}: SIMD ${NJ2K_SIMD}, static ${NJ2K_BUILD_STATIC}, shared ${NJ2K_BUILD_SHARED}, "
               "OpenSim profile ${NJ2K_PROFILE_OPENSIM}, tests ${NJ2K_BUILD_TESTS}, fuzzers ${NJ2K_BUILD_FUZZERS}, "
               "sanitizers '${NJ2K_SANITIZE}'")

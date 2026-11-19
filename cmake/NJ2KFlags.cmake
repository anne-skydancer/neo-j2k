# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Anne Skydancer (neo-j2k)
#
# Compiler settings shared by the library, the oracle and the tests.
include_guard(GLOBAL)
include(CheckCXXSourceCompiles)

# Which compiler front end: cl.exe, clang-cl, or a GCC-style driver (GCC, Clang).
set(NJ2K_CL OFF)
set(NJ2K_CLANG_CL OFF)
set(NJ2K_GNU_LIKE OFF)
if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  set(NJ2K_CL ON)
elseif(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set(NJ2K_CLANG_CL ON)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set(NJ2K_GNU_LIKE ON)
else()
  message(FATAL_ERROR "neo-j2k: unsupported compiler ${CMAKE_CXX_COMPILER_ID}")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64|x64)$")
  message(FATAL_ERROR "neo-j2k builds for x86-64 only (found ${CMAKE_SYSTEM_PROCESSOR}, ${CMAKE_SIZEOF_VOID_P}-byte pointers)")
endif()

# No floating-point contraction anywhere (owner decision D2: decodes bit-identical to OpenJPEG
# 2.5.4). cl.exe does not contract under /fp:precise; clang-cl does (the literature audit, A4), so
# it gets -ffp-contract=off, on its default precise model (/fp:precise on top would only be
# overridden, which clang-cl reports); GCC and Clang get it explicitly. Never fast-math.
function(nj2k_fp_strict target)
  if(NJ2K_CL)
    target_compile_options(${target} PRIVATE /fp:precise)
  elseif(NJ2K_CLANG_CL)
    target_compile_options(${target} PRIVATE /clang:-ffp-contract=off)
  else()
    target_compile_options(${target} PRIVATE -ffp-contract=off -fno-fast-math)
  endif()
endfunction()

# The instruction-set floor of neo-j2k's own code: x86-64-v3 (AVX2) chosen at compile time, or
# baseline x86-64 for the scalar build (owner decision D8).
function(nj2k_simd target)
  if(NJ2K_SIMD STREQUAL "AVX2")
    if(NJ2K_CL OR NJ2K_CLANG_CL)
      target_compile_options(${target} PRIVATE /arch:AVX2)
    else()
      target_compile_options(${target} PRIVATE -march=x86-64-v3)
    endif()
  endif()
endfunction()

# Warnings for neo-j2k's own code.
function(nj2k_warnings target)
  if(NJ2K_CL)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8)
    if(NJ2K_WERROR)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  elseif(NJ2K_CLANG_CL)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    if(NJ2K_WERROR)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    if(NJ2K_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# The library: no exceptions and no RTTI, so the compiler enforces the status-code API.
function(nj2k_no_exceptions target)
  if(NJ2K_CL OR NJ2K_CLANG_CL)
    target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/EHs-c- /GR->)
    target_compile_definitions(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:_HAS_EXCEPTIONS=0>)
  else()
    # without RTTI the vptr check cannot work; sanitizer flag sets that include it (OSS-Fuzz's
    # UBSan) would otherwise refuse to compile the library
    target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti -fno-sanitize=vptr>)
  endif()
endfunction()

# _FORTIFY_SOURCE=2 when optimising, unless the toolchain already sets it (Ubuntu's GCC does).
if(NJ2K_GNU_LIKE AND NOT DEFINED NJ2K_TOOLCHAIN_FORTIFIES)
  set(CMAKE_REQUIRED_FLAGS "-O2")
  check_cxx_source_compiles("#include <cstdio>
    #ifndef _FORTIFY_SOURCE
    #error not fortified
    #endif
    int main() { return 0; }" NJ2K_TOOLCHAIN_FORTIFIES)
  unset(CMAKE_REQUIRED_FLAGS)
endif()

# Hardening for code that parses network data (plan section 2.5): control-flow guard and CET.
function(nj2k_hardening target)
  get_target_property(type ${target} TYPE)
  if(NJ2K_CL OR NJ2K_CLANG_CL)
    target_compile_options(${target} PRIVATE /guard:cf)
    if(type STREQUAL "SHARED_LIBRARY" OR type STREQUAL "EXECUTABLE" OR type STREQUAL "MODULE_LIBRARY")
      target_link_options(${target} PRIVATE /GUARD:CF /CETCOMPAT)
    endif()
  else()
    target_compile_options(${target} PRIVATE -fcf-protection=full)
    if(NOT NJ2K_TOOLCHAIN_FORTIFIES AND NOT NJ2K_SANITIZE)
      target_compile_definitions(${target} PRIVATE $<$<NOT:$<CONFIG:Debug>>:_FORTIFY_SOURCE=2>)
    endif()
    if(type STREQUAL "SHARED_LIBRARY" OR type STREQUAL "EXECUTABLE" OR type STREQUAL "MODULE_LIBRARY")
      if(NOT APPLE)
        target_link_options(${target} PRIVATE LINKER:-z,relro LINKER:-z,now LINKER:-z,noexecstack)
      endif()
    endif()
  endif()
endfunction()

# Sanitizers apply to everything built (library, oracle, harness), so reports carry full stacks.
# NJ2K_SANITIZE: a list of address, undefined, memory, thread (GCC/Clang); address (MSVC, clang-cl).
if(NJ2K_SANITIZE)
  if(NJ2K_GNU_LIKE)
    list(JOIN NJ2K_SANITIZE "," _nj2k_san)
    add_compile_options(-fsanitize=${_nj2k_san} -fno-omit-frame-pointer -fno-sanitize-recover=all -g)
    add_link_options(-fsanitize=${_nj2k_san})
    if("memory" IN_LIST NJ2K_SANITIZE)
      add_compile_options(-fsanitize-memory-track-origins=2)
    endif()
  elseif(NJ2K_CL OR NJ2K_CLANG_CL)
    if(NOT NJ2K_SANITIZE STREQUAL "address")
      message(FATAL_ERROR "neo-j2k: only NJ2K_SANITIZE=address is supported with MSVC front ends")
    endif()
    add_compile_options(/fsanitize=address /Zi)
    add_link_options(/INCREMENTAL:NO /DEBUG)
  endif()
endif()

# libFuzzer's coverage instrumentation for every object of an MSVC fuzz build (the fuzz programs
# link the engine, fuzz/CMakeLists.txt); GCC-style builds pass -fsanitize=fuzzer-no-link in their
# flags (the linux-clang-fuzz preset, ClusterFuzzLite).
if(NJ2K_BUILD_FUZZERS AND NJ2K_CL)
  add_compile_options(/fsanitize-coverage=inline-8bit-counters /fsanitize-coverage=edge
                      /fsanitize-coverage=trace-cmp /fsanitize-coverage=trace-div)
endif()

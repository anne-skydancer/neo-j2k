# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Anne Skydancer (neo-j2k)
#
# The test oracle: OpenJPEG's library at the pinned fork commit plus patches (oracle/PIN), built the
# way the shipped package builds it (3p-openjpeg build-cmd.sh: /O2 /Ob3 /Oi /Ot /Gy /Gw /fp:precise
# /arch:AVX2 with MSVC; -mavx2 -ffp-contract=off -fno-fast-math with GCC and Clang; no GPU back-end).
# What decides the output is kept exactly: /fp:precise or -ffp-contract=off -fno-fast-math, and the
# AVX2 code paths. Inlining depth is left to the build type (/Ob3 would only re-override /Ob2), as
# it changes no result; the identity gate against the shipped DLL checks that. clang-cl contracts
# under /fp:precise, so it also gets -ffp-contract=off. The oracle links next to neo-j2k without
# clashes: neo-j2k exports nj2k_* only.
include(CheckIncludeFile)
include(CheckSymbolExists)

set(OPENJPEG_VERSION_MAJOR 2)
set(OPENJPEG_VERSION_MINOR 5)
set(OPENJPEG_VERSION_BUILD 4)
set(PACKAGE_VERSION "2.5.4")
check_include_file(malloc.h OPJ_HAVE_MALLOC_H)
check_symbol_exists(_aligned_malloc malloc.h OPJ_HAVE__ALIGNED_MALLOC)
check_symbol_exists(posix_memalign stdlib.h OPJ_HAVE_POSIX_MEMALIGN)
check_symbol_exists(memalign malloc.h OPJ_HAVE_MEMALIGN)
check_symbol_exists(fseeko stdio.h OPJ_HAVE_FSEEKO)
configure_file(openjp2/opj_config.h.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/include/opj_config.h" @ONLY)
configure_file(openjp2/opj_config_private.h.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/include/opj_config_private.h" @ONLY)

set(NJ2K_ORACLE_SOURCES
  thread.c bio.c cio.c dwt.c event.c ht_dec.c image.c invert.c j2k.c jp2.c mct.c mqc.c openjpeg.c
  opj_clock.c pi.c t1.c t2.c tcd.c tgt.c function_list.c sparse_array.c)
list(TRANSFORM NJ2K_ORACLE_SOURCES PREPEND "openjp2/")

find_package(Threads REQUIRED)

function(nj2k_oracle_flags target)
  target_include_directories(${target} PUBLIC openjp2 "${CMAKE_CURRENT_BINARY_DIR}/include")
  if(WIN32)
    target_compile_definitions(${target} PRIVATE MUTEX_win32)
  else()
    target_compile_definitions(${target} PRIVATE MUTEX_pthread)
  endif()
  target_link_libraries(${target} PRIVATE Threads::Threads)
  if(UNIX)
    target_link_libraries(${target} PRIVATE m)
  endif()
  if(NJ2K_CL)
    target_compile_options(${target} PRIVATE /w /fp:precise /arch:AVX2
      $<$<NOT:$<CONFIG:Debug>>:/Oi /Gy /Gw>)
  elseif(NJ2K_CLANG_CL)
    target_compile_options(${target} PRIVATE /w /arch:AVX2 /clang:-ffp-contract=off
      $<$<NOT:$<CONFIG:Debug>>:/Oi /Gy /Gw>)
  else()
    target_compile_options(${target} PRIVATE -w -mavx2 -ffp-contract=off -fno-fast-math
      $<$<NOT:$<CONFIG:Debug>>:-O3>)
    # OpenJPEG calls through mismatched function-pointer types by design (fork audit S6), which
    # -fsanitize=function reports on every decode; neo-j2k's own code keeps the check.
    if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND "undefined" IN_LIST NJ2K_SANITIZE)
      target_compile_options(${target} PRIVATE -fno-sanitize=function)
    endif()
  endif()
  set_target_properties(${target} PROPERTIES C_STANDARD 99 C_EXTENSIONS ON POSITION_INDEPENDENT_CODE ON)
endfunction()

add_library(nj2k_oracle STATIC ${NJ2K_ORACLE_SOURCES} openjp2/opj_malloc.c)
target_compile_definitions(nj2k_oracle PUBLIC OPJ_STATIC)
nj2k_oracle_flags(nj2k_oracle)

# The bench's counting variant: a module loaded at run time, with OpenJPEG's allocation entry
# points counted (count/opj_malloc_counting.c). Never used as the oracle.
if(NJ2K_BUILD_BENCH AND NOT NJ2K_SANITIZE)
  add_library(nj2k_oracle_counting MODULE ${NJ2K_ORACLE_SOURCES} count/opj_malloc_counting.c)
  target_compile_definitions(nj2k_oracle_counting PRIVATE OPJ_EXPORTS)
  nj2k_oracle_flags(nj2k_oracle_counting)
  target_include_directories(nj2k_oracle_counting PRIVATE openjp2)
  set_target_properties(nj2k_oracle_counting PROPERTIES C_VISIBILITY_PRESET hidden PREFIX "")
  if(NOT WIN32)
    target_link_options(nj2k_oracle_counting PRIVATE LINKER:-Bsymbolic)
  endif()
endif()

# Bench only (NJ2K_BENCH_OPENJP2_DLL, on in the presets): the oracle, our pinned fork commit of OpenJPEG 2.5.4 (oracle/PIN),
# as a shared library, openjp2.dll (libopenjp2.so), so that an application's view of the module (modules/openjp2) can be
# measured against it through the same opj_* API (bench/api_bench.py). Matched with the module: the oracle's own flags
# above (they decide the output) and, with MSVC, the static runtime (/MT), which is what the released module is built with
# (-DNJ2K_MSVC_STATIC_RUNTIME=ON; the module's default build is /MD). Not matched: the module's hardening and
# no-IPO settings (nj2k_hardening, nj2k_no_ipo), and inlining (/Ob2 from the build type, not /Ob3). Its sources are the
# oracle's, unchanged; it sits in its own folder, away from the module's file of the same name.
if(NJ2K_BENCH_OPENJP2_DLL AND NOT NJ2K_SANITIZE)
  add_library(nj2k_oracle_openjp2 SHARED ${NJ2K_ORACLE_SOURCES} openjp2/opj_malloc.c)
  target_compile_definitions(nj2k_oracle_openjp2 PRIVATE OPJ_EXPORTS)
  nj2k_oracle_flags(nj2k_oracle_openjp2)
  set_target_properties(nj2k_oracle_openjp2 PROPERTIES
    OUTPUT_NAME openjp2
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/openjp2-shared"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/openjp2-shared"
    PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/openjp2-shared"
    C_VISIBILITY_PRESET hidden
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  if(NOT WIN32)
    target_link_options(nj2k_oracle_openjp2 PRIVATE LINKER:-Bsymbolic)
  endif()
endif()

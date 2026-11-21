# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Anne Skydancer (neo-j2k)
#
# Code shared by the tests, the corpus tool, the fuzz drivers and the bench: file and hash helpers,
# a codestream scanner, the PGX reader, the load sampler, and the decoder backends (the oracle,
# OpenJPEG-ABI libraries loaded at run time, neo-j2k through its compatibility header).
add_library(nj2k_test_common STATIC
  common.cpp
  generated.cpp
  j2k_scan.cpp
  pgx.cpp
  load.cpp
  viewer.cpp
  header_facts.cpp
  backend_openjpeg.cpp
  backend_neoj2k.cpp)
target_include_directories(nj2k_test_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(nj2k_test_common PUBLIC nj2k_oracle neoj2k_static ${CMAKE_DL_LIBS})
nj2k_fp_strict(nj2k_test_common)
nj2k_program_exceptions(nj2k_test_common)
if(NJ2K_GNU_LIKE)
  target_compile_options(nj2k_test_common PRIVATE -Wall -Wextra)
elseif(NJ2K_CL OR NJ2K_CLANG_CL)
  target_compile_options(nj2k_test_common PRIVATE /W3 /utf-8)
endif()
if(NOT NJ2K_BUILD_STATIC)
  message(FATAL_ERROR "the tests link the static library: keep NJ2K_BUILD_STATIC on")
endif()

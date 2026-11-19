# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Anne Skydancer (neo-j2k)
#
# Tests: API units, header checks, the oracle against its reference tables (whole files at every
# reduce level, every cut point of the small files), the layer-boundary oracle, the oracle's known
# resource defects, ISO class 1 conformance, the fuzz targets on their seeds (fuzz/CMakeLists.txt),
# and the export, global-state, LTO and instruction-set object checks. Compiled tests run with
# TEMP/TMP (TMPDIR) in NJ2K_TEST_TMP when it is set.
set(NJ2K_CORPUS "${PROJECT_SOURCE_DIR}/corpus/generated")
set(NJ2K_TESTDATA "$ENV{NJ2K_TESTDATA}" CACHE PATH "openjpeg-data checkout (tools/fetch_testdata.py)")
set(NJ2K_TEST_TMP "$ENV{NJ2K_TEST_TMP}" CACHE PATH "Folder for TEMP/TMP/TMPDIR while tests run")
file(TO_CMAKE_PATH "${NJ2K_TESTDATA}" NJ2K_TESTDATA)
file(TO_CMAKE_PATH "${NJ2K_TEST_TMP}" NJ2K_TEST_TMP)

function(nj2k_test_program name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE nj2k_test_common)
  nj2k_fp_strict(${name})
  nj2k_program_exceptions(${name})
endfunction()

nj2k_test_program(nj2k_diff tools/nj2k_diff.cpp)
nj2k_test_program(nj2k_conformance tools/nj2k_conformance.cpp)
nj2k_test_program(nj2k_api_unit unit/api_unit.cpp)

# The public headers as strict C (C99; MSVC has no C99 mode, so C11) and strict C++20.
add_library(nj2k_header_c OBJECT unit/header_c.c)
add_library(nj2k_header_cxx OBJECT unit/header_cxx.cpp)
foreach(t nj2k_header_c nj2k_header_cxx)
  target_include_directories(${t} PRIVATE "${PROJECT_SOURCE_DIR}/include")
  target_compile_definitions(${t} PRIVATE NJ2K_STATIC)
  if(NJ2K_GNU_LIKE)
    target_compile_options(${t} PRIVATE -Wall -Wextra -Wpedantic -pedantic-errors -Werror)
  else()
    target_compile_options(${t} PRIVATE /W4 /WX)
  endif()
endforeach()
if(NJ2K_GNU_LIKE)
  set_target_properties(nj2k_header_c PROPERTIES C_STANDARD 99 C_EXTENSIONS OFF)
else()
  set_target_properties(nj2k_header_c PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
endif()
set_target_properties(nj2k_header_cxx PROPERTIES CXX_STANDARD 20 CXX_EXTENSIONS OFF)
nj2k_program_exceptions(nj2k_header_cxx)

set(_nj2k_env)
if(NJ2K_TEST_TMP)
  set(_nj2k_env "TEMP=${NJ2K_TEST_TMP};TMP=${NJ2K_TEST_TMP};TMPDIR=${NJ2K_TEST_TMP}")
endif()
function(nj2k_add_test name)
  add_test(NAME ${name} COMMAND ${ARGN})
  if(_nj2k_env)
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "${_nj2k_env}")
  endif()
endfunction()

nj2k_add_test(api_unit nj2k_api_unit)

# The oracle against its reference tables (corpus/generated/oracle-reference*.tsv, written on
# Windows with MSVC): whole files in plain and viewer mode at reduce 0-5, and every cut point of the
# files up to 4 KB, one test per file so CTest runs them in parallel. Sanitizer builds take every
# 13th cut point (NJ2K_TEST_CUT_STEP=13); NJ2K_TEST_NIGHTLY adds every cut point of the files up to
# 16 KB. The tables are committed: a missing one, or a step it does not hold, stops the configure,
# and nj2k_diff fails a check that matches no line, so no test can pass having checked nothing.
if(NJ2K_SANITIZE)
  set(_nj2k_cut_step_default 13)
else()
  set(_nj2k_cut_step_default 1)
endif()
set(NJ2K_TEST_CUT_STEP "${_nj2k_cut_step_default}" CACHE STRING "Cut-point step of the reference tests: 1 or 13")
option(NJ2K_TEST_NIGHTLY "Also verify the nightly reference table (cut points of files up to 16 KB)" OFF)
set(_nj2k_ref "${NJ2K_CORPUS}/oracle-reference.tsv")
if(NOT EXISTS "${_nj2k_ref}")
  message(FATAL_ERROR "the oracle's reference table ${_nj2k_ref} is missing (it is committed; doc/testing.md)")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_nj2k_ref}")
nj2k_add_test(oracle_whole nj2k_diff verify --a oracle --ref "${_nj2k_ref}" --root "${NJ2K_CORPUS}" --no-cuts)
file(STRINGS "${_nj2k_ref}" _nj2k_cut_lines REGEX "^cuts\t")
set(_nj2k_cut_files)
set(_nj2k_cut_steps)
foreach(_line IN LISTS _nj2k_cut_lines)
  string(REPLACE "\t" ";" _fields "${_line}")
  list(GET _fields 1 _file)
  list(GET _fields 5 _step)
  list(APPEND _nj2k_cut_files "${_file}")
  list(APPEND _nj2k_cut_steps "${_step}")
endforeach()
list(REMOVE_DUPLICATES _nj2k_cut_files)
list(REMOVE_DUPLICATES _nj2k_cut_steps)
if(NOT _nj2k_cut_files OR NOT NJ2K_TEST_CUT_STEP IN_LIST _nj2k_cut_steps)
  message(FATAL_ERROR "NJ2K_TEST_CUT_STEP is '${NJ2K_TEST_CUT_STEP}'; the reference table holds the steps "
                      "'${_nj2k_cut_steps}'")
endif()
foreach(_file IN LISTS _nj2k_cut_files)
  string(REGEX REPLACE "\\.j2k$" "" _name "${_file}")
  nj2k_add_test(oracle_cuts.${_name} nj2k_diff verify --a oracle --ref "${_nj2k_ref}" --root "${NJ2K_CORPUS}"
                --no-whole --only "${_file}" --cut-steps ${NJ2K_TEST_CUT_STEP})
endforeach()
nj2k_add_test(oracle_layers nj2k_diff layers --a oracle --files "${NJ2K_CORPUS}"
              --ref "${CMAKE_CURRENT_SOURCE_DIR}/oracle-known-defects.txt")
if(NJ2K_TEST_NIGHTLY)
  set(_nj2k_ref_nightly "${NJ2K_CORPUS}/oracle-reference-nightly.tsv")
  if(NOT EXISTS "${_nj2k_ref_nightly}")
    message(FATAL_ERROR "the nightly reference table ${_nj2k_ref_nightly} is missing (it is committed)")
  endif()
  nj2k_add_test(oracle_cuts_nightly nj2k_diff verify --a oracle --ref "${_nj2k_ref_nightly}" --root "${NJ2K_CORPUS}")
endif()

# The oracle's known resource defects (doc/oracle.md, tests/oracle-known-resource-defects.txt):
# each input must still make the oracle ask the heap for what the list says, through the counting
# oracle module, which is built without sanitizers only; a line whose defect is gone fails.
if(TARGET nj2k_oracle_counting)
  nj2k_add_test(oracle_known_resources nj2k_diff resources --a "lib:$<TARGET_FILE:nj2k_oracle_counting>=counting"
                --ref "${CMAKE_CURRENT_SOURCE_DIR}/oracle-known-resource-defects.txt"
                --root "${CMAKE_CURRENT_SOURCE_DIR}/known-defects")
endif()
nj2k_add_test(conformance_class1 nj2k_conformance --a oracle --data "${NJ2K_TESTDATA}")
set_tests_properties(conformance_class1 PROPERTIES SKIP_RETURN_CODE 77)

# The oracle's encoder gives the committed corpus byte for byte on this compiler and platform (the
# corpus was written on Windows with MSVC): the encoder oracle stage 6a relies on.
if(NJ2K_BUILD_TOOLS)
  nj2k_add_test(corpus_regenerates nj2k_corpus_gen --verify "${NJ2K_CORPUS}")
endif()

# Checks of the built library itself (Python, no third-party modules).
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND AND NOT NJ2K_SANITIZE)
  if(NJ2K_BUILD_SHARED)
    nj2k_add_test(exports "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/tools/check_exports.py"
                  --library "$<TARGET_FILE:neoj2k_shared>" --header "${PROJECT_SOURCE_DIR}/include/neoj2k/neoj2k.h"
                  --list "${PROJECT_SOURCE_DIR}/abi/exports.txt")
  endif()
  # the scalar build (NJ2K_SIMD=SCALAR, "for other CPUs", D8) holds every object to baseline x86-64
  set(_nj2k_baseline)
  if(NJ2K_SIMD STREQUAL "SCALAR")
    set(_nj2k_baseline --baseline-library)
  endif()
  nj2k_add_test(global_state "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/tools/check_objects.py"
                --library "$<TARGET_FILE:neoj2k_static>"
                --cpu-check-objects "$<TARGET_OBJECTS:neoj2k_cpu_check_static>" ${_nj2k_baseline})
endif()

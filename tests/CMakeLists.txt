# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Anne Skydancer (neo-j2k)
#
# Tests: API units, header checks, the decode core's units, the oracle and neo-j2k against the
# oracle's reference tables (whole files at every reduce level, every cut point of the small files),
# the layer-boundary oracle, the oracle's known resource defects, ISO class 1 conformance, neo-j2k
# against the oracle on the test data, the fuzz targets on their seeds (fuzz/CMakeLists.txt),
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
# stage 1b: the header model and the limits, through the API and the library's internals (included
# by their path: src/ is no include directory, its limits and memory headers would hide the C ones)
nj2k_test_program(nj2k_header_unit unit/header_unit.cpp)
# stage 1c: the decode core's departures, kernels, memory decode and zero-heap gate
nj2k_test_program(nj2k_decode_unit unit/decode_unit.cpp)
nj2k_simd(nj2k_decode_unit)  # the AVX2 kernels' declarations, compared with their scalar references
# stage 6a: the encoder against the oracle's, byte for byte, at the viewer's settings
nj2k_test_program(nj2k_encode_unit unit/encode_unit.cpp)
# the encoder's MQ coder and Tier-1 against a frozen copy of their text before the MQ redesign (byte identity)
nj2k_test_program(nj2k_enc_t1_unit unit/enc_t1_unit.cpp unit/enc_t1_frozen.cpp)
nj2k_simd(nj2k_enc_t1_unit)
# the decoder's Tier-1 (MQ decoder, passes, code-block driver) against a frozen copy of its text before the MQ-NG
# redesign (coefficient identity); the vectors are code-blocks as Tier-1 receives them (tests/vectors/t1)
nj2k_test_program(nj2k_t1_unit unit/t1_unit.cpp unit/t1_frozen.cpp)
nj2k_simd(nj2k_t1_unit)
# the same with the mask form of the MQ decision (the form GCC gets) compiled in on any compiler: t1.cpp is built into the
# test (its objects take precedence over the library's), so both forms are tested everywhere
nj2k_test_program(nj2k_t1_unit_masks unit/t1_unit.cpp unit/t1_frozen.cpp ../src/t1.cpp)
target_compile_definitions(nj2k_t1_unit_masks PRIVATE NJ2K_MQ_FORCE_MASKS=1)
nj2k_simd(nj2k_t1_unit_masks)
# the Rust twin's plan, findings C2 (memory-limit outcomes and the codec's history: none found) and
# C3 (the host's workers' floating-point environment reaches the samples: fixed, held as same), each held as today's
# behaviour (unit/findings_unit.cpp)
nj2k_test_program(nj2k_findings_unit unit/findings_unit.cpp)
# the output does not depend on the host's floating-point environment (MXCSR)
nj2k_test_program(nj2k_fpenv_unit unit/fpenv_unit.cpp)
# the openjp2 module (modules/openjp2): OpenJPEG's API over neo-j2k, loaded at run time as a viewer loads
# its openjp2 library. The tests build it first: they load it by path.
if(TARGET neoj2k_openjp2)
  nj2k_test_program(nj2k_openjp2_unit unit/openjp2_unit.cpp)
  add_dependencies(nj2k_openjp2_unit neoj2k_openjp2)
  add_dependencies(nj2k_diff neoj2k_openjp2)
endif()
# the local gate on a viewer's texture cache (private: run where it is, doc/testing.md)
nj2k_test_program(nj2k_cache_check tools/nj2k_cache_check.cpp)
if(NJ2K_PROFILE_OPENSIM)
  target_compile_definitions(nj2k_header_unit PRIVATE NJ2K_TEST_OPENSIM=1)
endif()

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
nj2k_add_test(decode_unit nj2k_decode_unit "${NJ2K_CORPUS}")
# stage 5: threads in one image (one thread against 2-16 parts; the ThreadSanitizer job runs it)
nj2k_add_test(decode_threads nj2k_decode_unit "${NJ2K_CORPUS}" --threads)
nj2k_add_test(encode_unit nj2k_encode_unit "${NJ2K_CORPUS}")
nj2k_add_test(enc_t1_unit nj2k_enc_t1_unit)
nj2k_add_test(t1_unit nj2k_t1_unit "${PROJECT_SOURCE_DIR}/tests/vectors/t1")
nj2k_add_test(t1_unit_masks nj2k_t1_unit_masks "${PROJECT_SOURCE_DIR}/tests/vectors/t1")
# NJ2K_T1_VECTORS: a folder of the study's dumps of more files (the conformance set's 35,390 code-blocks), run too
set(NJ2K_T1_VECTORS "$ENV{NJ2K_T1_VECTORS}" CACHE PATH "Folder of .blk code-block dumps for t1_unit_more (not committed)")
if(NJ2K_T1_VECTORS)
  file(TO_CMAKE_PATH "${NJ2K_T1_VECTORS}" _nj2k_t1_vectors)
  nj2k_add_test(t1_unit_more nj2k_t1_unit --no-exhaustive "${_nj2k_t1_vectors}")
endif()
nj2k_add_test(findings_c2 nj2k_findings_unit --c2 "${NJ2K_CORPUS},${PROJECT_SOURCE_DIR}/fuzz/regressions" --expect none)
nj2k_add_test(findings_c3 nj2k_findings_unit --c3 "${NJ2K_CORPUS}" --expect same)
nj2k_add_test(fpenv_unit nj2k_fpenv_unit "${NJ2K_CORPUS}")
nj2k_add_test(header_unit nj2k_header_unit "${NJ2K_CORPUS}" "${PROJECT_SOURCE_DIR}/fuzz/bombs"
              "${CMAKE_CURRENT_SOURCE_DIR}/known-defects")

# Header facts (stage 1b gate): neo-j2k's header read gives the oracle's image and codestream
# information for every generated file at every reduce level, the factor in the parameters and set
# before the header, and every prefix through the main header refuses where the oracle refuses;
# with the conformance data, its codestreams and JP2 files too.
nj2k_add_test(header_facts nj2k_diff header --a oracle --b neoj2k --files "${NJ2K_CORPUS}" --root "${NJ2K_CORPUS}"
              --modes plain,viewer --reduce 0-5 --bytes 4096)
if(EXISTS "${NJ2K_TESTDATA}/input/conformance")
  nj2k_add_test(header_facts_conformance nj2k_diff header --a oracle --b neoj2k
                --files "${NJ2K_TESTDATA}/input/conformance" --modes plain,viewer --reduce 0-5 --bytes 1024)
endif()

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
  # every MQ decision of the redesigned encoder against the frozen one: 4.4 billion, a few minutes
  nj2k_add_test(enc_t1_exhaustive nj2k_enc_t1_unit --exhaustive-full)
  # every MQ decision of the redesigned decoder against the frozen one and the standard's model
  nj2k_add_test(t1_exhaustive nj2k_t1_unit --exhaustive-full)
endif()
if(NJ2K_TEST_NIGHTLY)
  set(_nj2k_ref_nightly "${NJ2K_CORPUS}/oracle-reference-nightly.tsv")
  if(NOT EXISTS "${_nj2k_ref_nightly}")
    message(FATAL_ERROR "the nightly reference table ${_nj2k_ref_nightly} is missing (it is committed)")
  endif()
  nj2k_add_test(oracle_cuts_nightly nj2k_diff verify --a oracle --ref "${_nj2k_ref_nightly}" --root "${NJ2K_CORPUS}")
endif()

# neo-j2k against the same tables (stage 1c gate): whole files at every reduce level and every cut
# point give the oracle's digests, one test per file as above. The files of neoj2k-departures.txt,
# where neo-j2k departs from the oracle on purpose (doc/departures.md), are left out and tested on
# their own (decode_unit); verify fails on a listed file the table does not hold. Then the
# layer-boundary oracle with no known defect, ISO class 1 conformance, and with the test data every
# conformance codestream and JP2 file in every mode at every reduce level equal to the oracle (with
# tools/fetch_testdata.py --all, OpenJPEG's non-regression files too, less their departures).
set(_nj2k_departures "${CMAKE_CURRENT_SOURCE_DIR}/neoj2k-departures.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_nj2k_departures}")
file(STRINGS "${_nj2k_departures}" _nj2k_departure_lines REGEX "^[^#]")
set(_nj2k_departure_files)
foreach(_line IN LISTS _nj2k_departure_lines)
  string(REGEX REPLACE "[ \t].*$" "" _file "${_line}")
  list(APPEND _nj2k_departure_files "${_file}")
endforeach()
nj2k_add_test(neoj2k_whole nj2k_diff verify --a neoj2k --ref "${_nj2k_ref}" --root "${NJ2K_CORPUS}" --no-cuts
              --departures "${_nj2k_departures}")
foreach(_file IN LISTS _nj2k_cut_files)
  if(_file IN_LIST _nj2k_departure_files)
    continue()
  endif()
  string(REGEX REPLACE "\\.j2k$" "" _name "${_file}")
  nj2k_add_test(neoj2k_cuts.${_name} nj2k_diff verify --a neoj2k --ref "${_nj2k_ref}" --root "${NJ2K_CORPUS}"
                --no-whole --only "${_file}" --cut-steps ${NJ2K_TEST_CUT_STEP})
endforeach()
nj2k_add_test(neoj2k_layers nj2k_diff layers --a neoj2k --files "${NJ2K_CORPUS}")
if(NJ2K_TEST_NIGHTLY)
  nj2k_add_test(neoj2k_cuts_nightly nj2k_diff verify --a neoj2k --ref "${_nj2k_ref_nightly}" --root "${NJ2K_CORPUS}")
endif()
nj2k_add_test(conformance_class1_neoj2k nj2k_conformance --a neoj2k --data "${NJ2K_TESTDATA}")
set_tests_properties(conformance_class1_neoj2k PROPERTIES SKIP_RETURN_CODE 77)
if(EXISTS "${NJ2K_TESTDATA}/input/conformance")
  nj2k_add_test(neoj2k_conformance_differential nj2k_diff compare --a oracle --b neoj2k --jp2
                --files "${NJ2K_TESTDATA}/input/conformance" --mode plain,strict,viewer --reduce 0-5)
endif()
if(EXISTS "${NJ2K_TESTDATA}/input/nonregression")
  nj2k_add_test(neoj2k_nonregression_differential nj2k_diff compare --a oracle --b neoj2k --jp2
                --files "${NJ2K_TESTDATA}/input/nonregression" --mode plain,viewer --reduce 0-5
                --departures "${CMAKE_CURRENT_SOURCE_DIR}/neoj2k-departures-nonregression.txt")
endif()

# The openjp2 module against the same tables: the viewer's own way in, OpenJPEG's names called through
# OpenJPEG's header on a library loaded by path (nj2k_diff's lib: backend). Whole files at every reduce
# level, the same on 4 threads of the module's own pool (the large corpus files reach it), the layer
# boundaries, the header reads, the unit (version, threads started and joined, decodes at 0-64 threads,
# decoders at once, the upload encode), and with the test data the conformance files too. Every cut point
# is the core's, tested above, and only the calls between differ.
if(TARGET neoj2k_openjp2)
  set(_nj2k_openjp2 "lib:$<TARGET_FILE:neoj2k_openjp2>=openjp2")
  nj2k_add_test(openjp2_whole nj2k_diff verify --a "${_nj2k_openjp2}" --ref "${_nj2k_ref}" --root "${NJ2K_CORPUS}"
                --no-cuts --departures "${_nj2k_departures}")
  nj2k_add_test(openjp2_whole_threads nj2k_diff verify --a "${_nj2k_openjp2}" --ref "${_nj2k_ref}"
                --root "${NJ2K_CORPUS}" --no-cuts --departures "${_nj2k_departures}" --threads 4)
  nj2k_add_test(openjp2_layers nj2k_diff layers --a "${_nj2k_openjp2}" --files "${NJ2K_CORPUS}")
  nj2k_add_test(openjp2_header nj2k_diff header --a oracle --b "${_nj2k_openjp2}" --files "${NJ2K_CORPUS}"
                --root "${NJ2K_CORPUS}" --modes plain,viewer --reduce 0-5 --bytes 4096)
  nj2k_add_test(openjp2_unit nj2k_openjp2_unit "$<TARGET_FILE:neoj2k_openjp2>" "${NJ2K_CORPUS}")
  if(EXISTS "${NJ2K_TESTDATA}/input/conformance")
    nj2k_add_test(openjp2_conformance_differential nj2k_diff compare --a oracle --b "${_nj2k_openjp2}" --jp2
                  --files "${NJ2K_TESTDATA}/input/conformance" --mode plain,strict,viewer --reduce 0-5)
  endif()
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
  # the openjp2 module exports exactly the functions of OpenJPEG's header, and its forwarders are current
  if(TARGET neoj2k_openjp2)
    nj2k_add_test(openjp2_exports "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/tools/check_openjp2.py"
                  --library "$<TARGET_FILE:neoj2k_openjp2>" --header "${PROJECT_SOURCE_DIR}/oracle/openjp2/openjpeg.h")
    nj2k_add_test(openjp2_forwarders "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/tools/gen_openjp2.py" --check)
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

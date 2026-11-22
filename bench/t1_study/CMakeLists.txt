# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Anne Skydancer (neo-j2k)
#
# The encoder Tier-1 study (doc/enc_t1_study.md): a standalone project, not part of the main build.
#   cmake -S bench/t1_study -B <dir> -G "Visual Studio 18 2026" -A x64 && cmake --build <dir> --config Release
# It builds the library twice from src/ with the library's own flags (MSVC, /O2 /arch:AVX2 /fp:precise, no
# exceptions, /guard:cf, no LTCG as the static library): lib_head unchanged, and lib_instr, whose encode.cpp is
# a copy with the stage clock and the code-block dump (hooks.h) inserted at configure time. src/ is never written.
cmake_minimum_required(VERSION 3.25)
project(nj2k_t1_study CXX C)
option(STUDY_SCALAR "baseline x86-64 (the library's NJ2K_SIMD=SCALAR build) instead of AVX2" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(SRC "${REPO}/src")

set(LIB_SOURCES version.cpp image.cpp defaults.cpp not_yet.cpp memory.cpp arena.cpp messages.cpp stream.cpp
  limit_profiles.cpp header.cpp jp2.cpp codec.cpp work_arena.cpp decode.cpp tile.cpp t2.cpp t1.cpp dwt.cpp mct.cpp
  output.cpp encode.cpp enc_t1.cpp enc_dwt.cpp enc_t2.cpp limit_profile_opensim.cpp)

function(study_flags target)
  target_compile_definitions(${target} PRIVATE NJ2K_STATIC _HAS_EXCEPTIONS=0)
  target_include_directories(${target} PRIVATE "${REPO}/include" "${SRC}" "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_options(${target} PRIVATE /O2 /Zi $<$<NOT:$<BOOL:${STUDY_SCALAR}>>:/arch:AVX2> /fp:precise /EHs-c- /GR- /guard:cf /permissive-
    /Zc:__cplusplus /Zc:preprocessor /utf-8 /W3)
endfunction()

function(study_lib name)
  set(srcs ${ARGN})
  add_library(${name} STATIC ${srcs} "${SRC}/cpu_check.c")
  study_flags(${name})
  if(NOT STUDY_SCALAR)
    set_source_files_properties("${SRC}/cpu_check.c" TARGET_DIRECTORY ${name} PROPERTIES COMPILE_DEFINITIONS
      "NJ2K_REQUIRE_X86_64_V3")
  endif()
endfunction()

set(HEAD_SOURCES)
foreach(s IN LISTS LIB_SOURCES)
  list(APPEND HEAD_SOURCES "${SRC}/${s}")
endforeach()
study_lib(lib_head ${HEAD_SOURCES})

# the instrumented copy of encode.cpp
file(READ "${SRC}/encode.cpp" ENC)
function(must_replace from to)
  string(FIND "${ENC}" "${from}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "t1_study: anchor not found in encode.cpp: ${from}")
  endif()
  string(REPLACE "${from}" "${to}" out "${ENC}")
  set(ENC "${out}" PARENT_SCOPE)
endfunction()
must_replace("    EncTile tile;\n    if (!build_tile(e, tile)) {\n        return NJ2K_E_NO_MEMORY;\n    }\n"
  "    study::mark_start();\n    EncTile tile;\n    if (!build_tile(e, tile)) {\n        return NJ2K_E_NO_MEMORY;\n    }\n    study::mark(study::kBuild);\n")
must_replace("    // Small images stay serial: executor dispatch"
  "    study::mark(study::kDcMct);\n    // Small images stay serial: executor dispatch")
must_replace("    // Tier-1, one job a code-block (opj_tcd_t1_encode)\n"
  "    study::mark(study::kDwt);\n    // Tier-1, one job a code-block (opj_tcd_t1_encode)\n")
must_replace("    // the rate allocation and Tier-2, behind the tile-part's SOT and SOD\n"
  "    study::t1_stage_end();\n    // the rate allocation and Tier-2, behind the tile-part's SOT and SOD\n")
must_replace("    *total = 12 + 2 + written;\n" "    study::mark(study::kT2);\n    *total = 12 + 2 + written;\n")
must_replace("    auto* j = static_cast<T1Jobs*>(ctx);\n" "    study::t1_job_begin();\n    auto* j = static_cast<T1Jobs*>(ctx);\n")
must_replace("    j->ok[index] = enc_t1_encode(t1, cblk,"
  "    study::t1_before(enc_t1_data(t1), cblk_w, cblk_h, band.bandno, job.compno, tilec.numres - 1 - job.resno, p.reversible, band.stepsize, j->mct_norms, j->mct_numcomps);\n    j->ok[index] = enc_t1_encode(t1, cblk,")
must_replace("                                 j->mct_numcomps) ? 1 : 0;\n"
  "                                 j->mct_numcomps) ? 1 : 0;\n    study::t1_after();\n")
file(WRITE "${CMAKE_BINARY_DIR}/instr/encode.cpp" "#include \"hooks.h\"\n${ENC}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SRC}/encode.cpp")

set(INSTR_SOURCES)
foreach(s IN LISTS LIB_SOURCES)
  if(s STREQUAL "encode.cpp")
    list(APPEND INSTR_SOURCES "${CMAKE_BINARY_DIR}/instr/encode.cpp")
  else()
    list(APPEND INSTR_SOURCES "${SRC}/${s}")
  endif()
endforeach()
study_lib(lib_instr ${INSTR_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/hooks.cpp")

# the stage profile and the code-block dump (instrumented library)
add_executable(t1_profile t1_profile.cpp driver.cpp)
study_flags(t1_profile)
target_link_libraries(t1_profile PRIVATE lib_instr)

# the Tier-1 replay: the unchanged library's enc_t1_encode against the study's copies (proto/)
add_executable(t1_replay t1_replay.cpp proto/enc_t1_count.cpp proto/enc_t1_passtime.cpp proto/enc_t1_copy.cpp
  proto/enc_t1_split.cpp proto/mq_variants.cpp proto/enc_t1_f0.cpp proto/enc_t1_f1.cpp proto/enc_t1_f2.cpp proto/enc_t1_f3.cpp proto/enc_t1_f4.cpp proto/enc_t1_f5.cpp proto/enc_t1_f6.cpp proto/enc_t1_f7.cpp proto/enc_t1_f8.cpp
  proto/enc_t1_f9.cpp proto/enc_t1_f10.cpp proto/enc_t1_f11.cpp proto/enc_t1_f4t.cpp)
study_flags(t1_replay)
target_link_libraries(t1_replay PRIVATE lib_head)

# the full encode through the unchanged library (end-to-end gate, same driver)
add_executable(t1_e2e t1_e2e.cpp driver.cpp)
study_flags(t1_e2e)
target_link_libraries(t1_e2e PRIVATE lib_head)

# the stage candidates in the whole encoder: the library with enc_t1.cpp replaced (lib_enc_t1_s*.cpp)
foreach(stage s1 s2)
  set(STAGE_SOURCES)
  foreach(s IN LISTS LIB_SOURCES)
    if(s STREQUAL "enc_t1.cpp")
      list(APPEND STAGE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/lib_enc_t1_${stage}.cpp")
    else()
      list(APPEND STAGE_SOURCES "${SRC}/${s}")
    endif()
  endforeach()
  study_lib(lib_${stage} ${STAGE_SOURCES})
  add_executable(t1_e2e_${stage} t1_e2e.cpp driver.cpp)
  study_flags(t1_e2e_${stage})
  target_link_libraries(t1_e2e_${stage} PRIVATE lib_${stage})
endforeach()

# every single MQ decision of the stage-2 coder against the library's MqEnc
add_executable(mq_exhaustive mq_exhaustive.cpp)
study_flags(mq_exhaustive)

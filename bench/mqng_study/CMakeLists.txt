# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Anne Skydancer (neo-j2k)
#
# The MQ-NG decoder study (doc/mqng_study.md): a standalone project, not part of the main build.
#   cmake -S bench/mqng_study -B <dir> -G "Visual Studio 18 2026" -A x64 && cmake --build <dir> --config Release
# It builds the library twice from src/ with the library's flags (MSVC, /O2 /arch:AVX2 /fp:precise, no
# exceptions, /guard:cf, no LTCG as the static library): lib_head unchanged, and lib_instr, whose decode.cpp
# and tile.cpp are copies with the stage clock and the code-block dump (hooks.h) inserted at configure time.
# The decoder variants (proto/variant.cpp, one object a variant) include a body generated from src/t1.cpp
# (tools/make_body.py). src/ is never written.
cmake_minimum_required(VERSION 3.25)
project(nj2k_mqng_study CXX C)
option(STUDY_SCALAR "baseline x86-64 (the library's NJ2K_SIMD=SCALAR build) instead of AVX2" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(SRC "${REPO}/src")
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(LIB_SOURCES version.cpp image.cpp defaults.cpp not_yet.cpp memory.cpp arena.cpp messages.cpp stream.cpp
  limit_profiles.cpp header.cpp jp2.cpp codec.cpp work_arena.cpp decode.cpp tile.cpp t2.cpp t1.cpp dwt.cpp mct.cpp
  output.cpp encode.cpp enc_t1.cpp enc_dwt.cpp enc_t2.cpp limit_profile_opensim.cpp)

function(study_flags target)
  target_compile_definitions(${target} PRIVATE NJ2K_STATIC _HAS_EXCEPTIONS=0 _CRT_SECURE_NO_WARNINGS)
  target_include_directories(${target} PRIVATE "${REPO}/include" "${SRC}" "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/proto" "${CMAKE_BINARY_DIR}/gen")
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

# the instrumented copies of decode.cpp and tile.cpp
function(instrument file)
  file(READ "${SRC}/${file}" TEXT)
  set(i 1)
  while(i LESS ARGC)
    math(EXPR j "${i} + 1")
    set(from "${ARGV${i}}")
    set(to "${ARGV${j}}")
    string(FIND "${TEXT}" "${from}" at)
    if(at EQUAL -1)
      message(FATAL_ERROR "mqng_study: anchor not found in ${file}: ${from}")
    endif()
    string(REPLACE "${from}" "${to}" TEXT "${TEXT}")
    math(EXPR i "${i} + 2")
  endwhile()
  file(WRITE "${CMAKE_BINARY_DIR}/instr/${file}" "#include \"hooks.h\"\n${TEXT}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SRC}/${file}")
endfunction()
instrument(decode.cpp
  "        nj2k_status_t st = t.read_packets(d.work, msg, resno);\n"
  "        mqs::mark_start();\n        nj2k_status_t st = t.read_packets(d.work, msg, resno);\n        mqs::mark(mqs::kT2);\n"
  "            st = t.decode_samples(d.t1, d.dwt.p, resno, msg, tile_exec ? &tj : nullptr);\n"
  "            mqs::mark_start();\n            st = t.decode_samples(d.t1, d.dwt.p, resno, msg, tile_exec ? &tj : nullptr);\n            mqs::mark(mqs::kDwt);\n")
instrument(tile.cpp
  "    // the inverse wavelet up to the resolution decoded (opj_tcd_dwt_decode)\n"
  "    mqs::mark(mqs::kT1);\n    // the inverse wavelet up to the resolution decoded (opj_tcd_dwt_decode)\n"
  "    if (!t1_decode_cblk(t1, b, msg)) {\n"
  "    mqs::dump(b);\n    if (!t1_decode_cblk(t1, b, msg)) {\n")

set(INSTR_SOURCES)
foreach(s IN LISTS LIB_SOURCES)
  if(s STREQUAL "decode.cpp" OR s STREQUAL "tile.cpp")
    list(APPEND INSTR_SOURCES "${CMAKE_BINARY_DIR}/instr/${s}")
  else()
    list(APPEND INSTR_SOURCES "${SRC}/${s}")
  endif()
endforeach()
study_lib(lib_instr ${INSTR_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/hooks.cpp")

# the stage profile and the code-block dump (instrumented library)
add_executable(mqng_profile profile.cpp "${REPO}/bench/t1_study/driver.cpp")
study_flags(mqng_profile)
target_link_libraries(mqng_profile PRIVATE lib_instr)

# the decoder body the variants include
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_body.py" "${REPO}"
  "${CMAKE_BINARY_DIR}/gen/t1_dec_body.inc" RESULT_VARIABLE gen_rc)
if(NOT gen_rc EQUAL 0)
  message(FATAL_ERROR "mqng_study: make_body.py failed")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SRC}/t1.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_body.py")

set(VARIANT_OBJS)
foreach(v RANGE 0 18)
  add_library(variant_${v} OBJECT proto/variant.cpp)
  study_flags(variant_${v})
  target_compile_definitions(variant_${v} PRIVATE STUDY_V=${v})
  list(APPEND VARIANT_OBJS $<TARGET_OBJECTS:variant_${v}>)
endforeach()

# the replay: the unchanged library's t1_decode_cblk against the variants
add_executable(mqng_replay replay.cpp proto/count.cpp proto/mq_alone.cpp ${VARIANT_OBJS})
study_flags(mqng_replay)
target_link_libraries(mqng_replay PRIVATE lib_head)

# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Anne Skydancer (neo-j2k)
#
# The EBCOT-NG study (doc/ebcotng_study.md): a standalone project, not part of the main build.
#   cmake -S bench/ebcotng_study -B <dir> -G "Visual Studio 18 2026" -A x64 && cmake --build <dir> --config Release
# It builds the library from src/ with the library's flags (MSVC, /O2 /arch:AVX2 /fp:precise, no exceptions,
# /guard:cf, no LTCG) as lib_head, the two stage-2 bases of the earlier studies from their own sources (read
# only): the encoder's f5 (bench/t1_study) and the decoder's wagg and ws (bench/mqng_study), and this study's
# prototypes, one object a variant (enc_v.cpp / dec_v.cpp with the switches of enc_ng.inc / dec_ng.inc).
# src/ and the other studies' files are never written.
cmake_minimum_required(VERSION 3.25)
project(nj2k_ebcotng_study CXX C)
option(STUDY_SCALAR "baseline x86-64 (the library's NJ2K_SIMD=SCALAR build) instead of AVX2" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(SRC "${REPO}/src")
set(MQNG "${REPO}/bench/mqng_study")
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(LIB_SOURCES version.cpp image.cpp defaults.cpp not_yet.cpp memory.cpp arena.cpp messages.cpp stream.cpp
  limit_profiles.cpp header.cpp jp2.cpp codec.cpp work_arena.cpp decode.cpp tile.cpp t2.cpp t1.cpp dwt.cpp mct.cpp
  output.cpp encode.cpp enc_t1.cpp enc_dwt.cpp enc_t2.cpp limit_profile_opensim.cpp)

function(study_flags target)
  target_compile_definitions(${target} PRIVATE NJ2K_STATIC _HAS_EXCEPTIONS=0 _CRT_SECURE_NO_WARNINGS)
  target_include_directories(${target} PRIVATE "${REPO}/include" "${SRC}" "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_options(${target} PRIVATE /O2 /Zi $<$<NOT:$<BOOL:${STUDY_SCALAR}>>:/arch:AVX2> /fp:precise /EHs-c- /GR- /guard:cf /permissive-
    /Zc:__cplusplus /Zc:preprocessor /utf-8 /W3)
endfunction()

set(HEAD_SOURCES)
foreach(s IN LISTS LIB_SOURCES)
  list(APPEND HEAD_SOURCES "${SRC}/${s}")
endforeach()
add_library(lib_head STATIC ${HEAD_SOURCES} "${SRC}/cpu_check.c")
study_flags(lib_head)
if(NOT STUDY_SCALAR)
  set_source_files_properties("${SRC}/cpu_check.c" TARGET_DIRECTORY lib_head PROPERTIES COMPILE_DEFINITIONS
    "NJ2K_REQUIRE_X86_64_V3")
endif()

# the encoder base: f5 of the encoder study
add_library(ext_f5 OBJECT ext_f5.cpp)
study_flags(ext_f5)

# the decoder bases: MQ-NG's ws (stage 1) and wagg (stages 1+2), from its variant.cpp and a body generated
# here from src/t1.cpp by its make_body.py (written into this build's gen/, never into the other study)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${MQNG}/tools/make_body.py" "${REPO}"
  "${CMAKE_BINARY_DIR}/gen/t1_dec_body.inc" RESULT_VARIABLE gen_rc)
if(NOT gen_rc EQUAL 0)
  message(FATAL_ERROR "ebcotng_study: make_body.py failed")
endif()
set(EXT_OBJS $<TARGET_OBJECTS:ext_f5>)
foreach(v 4 14)
  add_library(mqng_v${v} OBJECT "${MQNG}/proto/variant.cpp")
  study_flags(mqng_v${v})
  target_include_directories(mqng_v${v} BEFORE PRIVATE "${MQNG}/proto" "${MQNG}" "${CMAKE_BINARY_DIR}/gen")
  target_compile_definitions(mqng_v${v} PRIVATE STUDY_V=${v})
  list(APPEND EXT_OBJS $<TARGET_OBJECTS:mqng_v${v}>)
endforeach()

# the prototypes: name|switches|check (exact: bytes and passes or coefficients; bytes: bytes only; none)
set(ENC_VARIANTS
  "e0||exact"
  "e0copy||exact"
  "ecount|E_COUNT=1|exact"
  "eclk|E_PASSCLK=1|exact"
  "emodel|E_MODEL=1|none"
  "emodelclk|E_MODEL=1,E_PASSCLK=1|none"
  "emodel_nonmse|E_MODEL=1,E_NO_NMSE=1|none"
  "emodel_noctx|E_MODEL=1,E_NO_CTX=1|none"
  "emodel_dupupd|E_MODEL=1,E_DUP_UPD=1|none"
  "emodel_masks|E_MODEL=1,E_REF_MASK=1,E_SPP_MASK=1,E_CLN_MASK=1|none"
  "enonmse|E_NO_NMSE=1|bytes"
  "edupupd|E_DUP_UPD=1|exact"
  "emagreg|E_MAGREG=1|exact"
  "erefmask|E_REF_MASK=1|exact"
  "erefmagreg|E_REF_MASK=1,E_MAGREG=1|exact"
  "esppmask|E_SPP_MASK=1|exact"
  "esppmaskbf|E_SPP_MASK=1,E_BRFREE=1|exact"
  "eclnmask|E_CLN_MASK=1|exact"
  "esimdpre|E_SIMD_PRE=1|exact"
  "emasks|E_REF_MASK=1,E_SPP_MASK=1,E_CLN_MASK=1|exact"
  "eall|E_MAGREG=1,E_REF_MASK=1,E_SPP_MASK=1,E_CLN_MASK=1,E_SIMD_PRE=1|exact"
  "ebest|E_REF_MASK=1,E_CLN_MASK=1,E_SIMD_PRE=1|exact"
  "esplit|E_SPLIT=7|exact"
  "esplitclk|E_SPLIT=7,E_PASSCLK=1|exact"
  "esplitref|E_SPLIT=2|exact"
  "esplitsig|E_SPLIT=1|exact"
  "ehyb|E_SPLIT=2,E_CLN_MASK=1,E_SIMD_PRE=1|exact"
  "ehyb2|E_SPLIT=3,E_CLN_MASK=1,E_SIMD_PRE=1|exact"
  "esplitsigskip|E_SPLIT=1,E_SPP_SKIP=1|exact"
  "ehyb3|E_SPLIT=3,E_SPP_SKIP=1,E_CLN_MASK=1,E_SIMD_PRE=1|exact"
  "eprebf|E_PRE_BRFREE=1|exact"
  "ehybs|E_SPLIT=2,E_CLN_MASK=1,E_PRE_BRFREE=1|exact"
  "ebests|E_REF_MASK=1,E_CLN_MASK=1,E_PRE_BRFREE=1|exact"
  "ehyb3s|E_SPLIT=3,E_SPP_SKIP=1,E_CLN_MASK=1,E_PRE_BRFREE=1|exact"
)
set(DEC_VARIANTS
  "d0||exact"
  "d0copy||exact"
  "dnoagg|D_NOAGG=1|exact"
  "dcount|D_COUNT=1,D_NOAGG=1|exact"
  "dclk|D_PASSCLK=1|exact"
  "drec|D_REC=1,D_NOAGG=1|exact"
  "dmodel|D_MODEL=1|model"
  "dmodelclk|D_MODEL=1,D_PASSCLK=1|model"
  "dmodel_noctx|D_MODEL=1,D_NO_CTX=1|modelnone"
  "dmodel_nostore|D_MODEL=1,D_NO_STORE=1|modelnone"
  "dmodel_dupupd|D_MODEL=1,D_DUP_UPD=1|model"
  "dmodel_masks|D_MODEL=1,D_REF_MASK=1,D_SPP_MASK=1,D_CLN_MASK=1|model"
  "ddupupd|D_DUP_UPD=1|exact"
  "dmagreg|D_MAGREG=1|exact"
  "drefmask|D_REF_MASK=1|exact"
  "drefmagreg|D_REF_MASK=1,D_MAGREG=1|exact"
  "dsppmask|D_SPP_MASK=1|exact"
  "dsppmaskbf|D_SPP_MASK=1,D_BRFREE=1|exact"
  "dclnmask|D_CLN_MASK=1|exact"
  "dmasks|D_REF_MASK=1,D_SPP_MASK=1,D_CLN_MASK=1|exact"
  "dall|D_MAGREG=1,D_REF_MASK=1,D_SPP_MASK=1,D_CLN_MASK=1|exact"
  "dlist|D_REF_LIST=1|exact"
  "dlistcln|D_REF_LIST=1,D_CLN_MASK=1|exact"
  "dlistnoagg|D_REF_LIST=1,D_NOAGG=1|exact"
)
set(VAR_INC "")
set(VAR_OBJS)
foreach(item IN LISTS ENC_VARIANTS)
  string(REPLACE "|" ";" parts "${item}")
  list(GET parts 0 name)
  list(GET parts 1 defs)
  list(GET parts 2 check)
  string(REPLACE "," ";" defs "${defs}")
  add_library(v_${name} OBJECT enc_v.cpp)
  study_flags(v_${name})
  target_compile_definitions(v_${name} PRIVATE ENG_NS=ng_${name} ${defs})
  list(APPEND VAR_OBJS $<TARGET_OBJECTS:v_${name}>)
  string(APPEND VAR_INC "ENC_VARIANT(ng_${name}, \"${name}\", ${check})\n")
endforeach()
foreach(item IN LISTS DEC_VARIANTS)
  string(REPLACE "|" ";" parts "${item}")
  list(GET parts 0 name)
  list(GET parts 1 defs)
  list(GET parts 2 check)
  string(REPLACE "," ";" defs "${defs}")
  add_library(v_${name} OBJECT dec_v.cpp)
  study_flags(v_${name})
  target_compile_definitions(v_${name} PRIVATE DNG_NS=ng_${name} ${defs})
  list(APPEND VAR_OBJS $<TARGET_OBJECTS:v_${name}>)
  string(APPEND VAR_INC "DEC_VARIANT(ng_${name}, \"${name}\", ${check})\n")
endforeach()
file(WRITE "${CMAKE_BINARY_DIR}/gen/variants.inc.tmp" "${VAR_INC}")
configure_file("${CMAKE_BINARY_DIR}/gen/variants.inc.tmp" "${CMAKE_BINARY_DIR}/gen/variants.inc" COPYONLY)

add_executable(ebc_replay replay.cpp ${EXT_OBJS} ${VAR_OBJS})
study_flags(ebc_replay)
target_include_directories(ebc_replay PRIVATE "${CMAKE_BINARY_DIR}/gen")
target_link_libraries(ebc_replay PRIVATE lib_head)

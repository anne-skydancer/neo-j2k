# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Anne Skydancer (neo-j2k)
#
# The openjp2 module (NJ2K_MODULE_OPENJP2): OpenJPEG's API as the library a viewer already loads,
# openjp2.dll on Windows and libopenjp2.so.7 on Linux, with neo-j2k inside. A viewer that talks to
# OpenJPEG keeps talking to it, unchanged: this is the adapter between its calls and neo-j2k's.
#
# It links the core's static library, so the core is the same code as in neoj2k.dll; the shared
# library exports opj_* and nothing else (the core's nj2k_* stay inside). The core never depends on
# it. What the adapter adds is in openjp2.cpp; the forwarders are generated (tools/gen_openjp2.py).
if(NOT NJ2K_BUILD_STATIC)
  message(FATAL_ERROR "NJ2K_MODULE_OPENJP2 links the core's static library: keep NJ2K_BUILD_STATIC on")
endif()

set(_nj2k_openjp2_sources openjp2.cpp opj_forward.inc)
if(WIN32)
  # what Explorer shows for the file (Properties, Details): neo-j2k, not OpenJPEG, whatever the file is renamed to
  enable_language(RC)
  configure_file(openjp2.rc.in "${CMAKE_CURRENT_BINARY_DIR}/openjp2.rc" @ONLY)
  list(APPEND _nj2k_openjp2_sources "${CMAKE_CURRENT_BINARY_DIR}/openjp2.rc")
endif()
add_library(neoj2k_openjp2 SHARED ${_nj2k_openjp2_sources})
set_source_files_properties(opj_forward.inc PROPERTIES HEADER_FILE_ONLY ON)
set_source_files_properties(openjp2.cpp PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/opj_forward.inc")
target_include_directories(neoj2k_openjp2 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(neoj2k_openjp2 PRIVATE neoj2k_static)

# The library OpenJPEG 2.5.4 builds: libopenjp2.so.7 -> libopenjp2.so.2.5.4, openjp2.dll.
set_target_properties(neoj2k_openjp2 PROPERTIES
  OUTPUT_NAME openjp2
  VERSION 2.5.4
  SOVERSION 7
  C_VISIBILITY_PRESET hidden
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  POSITION_INDEPENDENT_CODE ON)

nj2k_warnings(neoj2k_openjp2)
nj2k_fp_strict(neoj2k_openjp2)
nj2k_hardening(neoj2k_openjp2)
# Baseline x86-64 for the adapter's own code, so that the CPU check runs before any instruction of the
# core's does; threads need exceptions (std::thread), which stop at the module's boundary.
nj2k_baseline_isa(neoj2k_openjp2)
nj2k_program_exceptions(neoj2k_openjp2)
# No link-time code generation: it could mix the baseline adapter with the AVX2 core in one function.
nj2k_no_ipo(neoj2k_openjp2)

# The adapter takes and gives neo-j2k's types where a viewer's header says OpenJPEG's: the same layout under
# other names, which clang's -fsanitize=function reports at every call through the viewer's pointer type (the
# oracle is built the same way, oracle/CMakeLists.txt). The check skips a callee built without it.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND "undefined" IN_LIST NJ2K_SANITIZE)
  target_compile_options(neoj2k_openjp2 PRIVATE -fno-sanitize=function)
endif()

if(NOT WIN32)
  target_link_options(neoj2k_openjp2 PRIVATE "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/openjp2.map")
  if(NOT NJ2K_SANITIZE)
    target_link_options(neoj2k_openjp2 PRIVATE LINKER:--no-undefined)
  endif()
  set_property(TARGET neoj2k_openjp2 APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/openjp2.map")
  find_package(Threads REQUIRED)
  target_link_libraries(neoj2k_openjp2 PRIVATE Threads::Threads)
endif()

if(NJ2K_INSTALL)
  include(GNUInstallDirs)
  install(TARGETS neoj2k_openjp2
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

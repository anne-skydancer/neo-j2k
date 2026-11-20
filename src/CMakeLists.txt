# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Anne Skydancer (neo-j2k)
#
# The library: a static library and a shared one (neoj2k.dll, libneoj2k.so) from the same sources.
# They are separate targets, not one OBJECT library, so the static objects carry no dllexport.
include(GNUInstallDirs)
include(CheckIPOSupported)

set(NJ2K_SOURCES
  version.cpp
  image.cpp
  defaults.cpp
  not_yet.cpp
  memory.cpp
  arena.cpp
  messages.cpp
  stream.cpp
  limit_profiles.cpp
  header.cpp
  jp2.cpp
  codec.cpp
  work_arena.cpp
  decode.cpp
  tile.cpp
  t2.cpp
  t1.cpp
  dwt.cpp
  mct.cpp
  output.cpp
)
# The decoder limit profiles are modules (owner decision D10): Second Life's is built in, OpenSim's
# is its own source file, left out with NJ2K_PROFILE_OPENSIM=OFF.
if(NJ2K_PROFILE_OPENSIM)
  list(APPEND NJ2K_SOURCES limit_profile_opensim.cpp)
else()
  list(APPEND NJ2K_SOURCES limit_profile_opensim_off.cpp)
endif()

# No link-time code generation, in every configuration: the generic IPO property alone is not
# enough, since INTERPROCEDURAL_OPTIMIZATION_<CONFIG> (which CMAKE_INTERPROCEDURAL_OPTIMIZATION_<CONFIG>
# initialises, for example in a superproject that turns LTO on everywhere) overrides it.
function(nj2k_no_ipo target)
  set(_configs Debug Release RelWithDebInfo MinSizeRel ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
  list(REMOVE_DUPLICATES _configs)
  set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION OFF)
  foreach(_config IN LISTS _configs)
    string(TOUPPER "${_config}" _config)
    set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_${_config} OFF)
  endforeach()
endfunction()

# nj2k_cpu_supported() is compiled for baseline x86-64 whatever NJ2K_SIMD and the toolchain's default
# say, so it lives in an object library with the baseline flags, and without LTO, which could compile
# it again at link time with the flags of the objects it meets there.
function(nj2k_cpu_check_objects name defs)
  add_library(${name} OBJECT cpu_check.c)
  target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/include")
  target_compile_definitions(${name} PRIVATE ${defs})
  if(NJ2K_SIMD STREQUAL "AVX2")
    target_compile_definitions(${name} PRIVATE NJ2K_REQUIRE_X86_64_V3)
  endif()
  set_target_properties(${name} PROPERTIES C_VISIBILITY_PRESET hidden POSITION_INDEPENDENT_CODE ON)
  nj2k_baseline_isa(${name})
  nj2k_no_ipo(${name})
  nj2k_warnings(${name})
  nj2k_fp_strict(${name})
  nj2k_hardening(${name})
endfunction()

function(nj2k_library_common target)
  target_sources(${target} PRIVATE ${NJ2K_SOURCES})
  target_include_directories(${target} PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  set_target_properties(${target} PROPERTIES
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON)
  nj2k_warnings(${target})
  nj2k_fp_strict(${target})
  nj2k_simd(${target})
  nj2k_no_exceptions(${target})
  nj2k_hardening(${target})
endfunction()

set(NJ2K_LIBRARY_TARGETS)

if(NJ2K_BUILD_STATIC)
  nj2k_cpu_check_objects(neoj2k_cpu_check_static "NJ2K_STATIC")
  add_library(neoj2k_static STATIC $<TARGET_OBJECTS:neoj2k_cpu_check_static>)
  nj2k_library_common(neoj2k_static)
  target_compile_definitions(neoj2k_static PUBLIC NJ2K_STATIC)
  # neoj2k.lib is the DLL's import library on Windows, so the static one is neoj2k_static.lib there.
  if(WIN32)
    set_target_properties(neoj2k_static PROPERTIES OUTPUT_NAME neoj2k_static)
  else()
    set_target_properties(neoj2k_static PROPERTIES OUTPUT_NAME neoj2k)
  endif()
  # No link-time code generation in the static library: Microsoft advises against shipping /GL
  # objects, and GCC LTO objects only link with the GCC that wrote them (plan section 2.5);
  # tools/check_objects.py refuses such objects.
  nj2k_no_ipo(neoj2k_static)
  set_target_properties(neoj2k_static PROPERTIES EXPORT_NAME static)
  add_library(neoj2k::static ALIAS neoj2k_static)
  list(APPEND NJ2K_LIBRARY_TARGETS neoj2k_static)
endif()

if(NJ2K_BUILD_SHARED)
  nj2k_cpu_check_objects(neoj2k_cpu_check_shared "NJ2K_BUILDING")
  add_library(neoj2k_shared SHARED $<TARGET_OBJECTS:neoj2k_cpu_check_shared>)
  nj2k_library_common(neoj2k_shared)
  target_compile_definitions(neoj2k_shared PRIVATE NJ2K_BUILDING)
  # 0.x: the ABI may change with every minor version, so the soname carries it (libneoj2k.so.0.0);
  # from 1.0 on it is the major version alone.
  if(PROJECT_VERSION_MAJOR EQUAL 0)
    set(_nj2k_soversion "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
  else()
    set(_nj2k_soversion "${PROJECT_VERSION_MAJOR}")
  endif()
  set_target_properties(neoj2k_shared PROPERTIES
    OUTPUT_NAME neoj2k
    VERSION ${PROJECT_VERSION}
    SOVERSION ${_nj2k_soversion})
  if(NOT WIN32)
    target_link_options(neoj2k_shared PRIVATE "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/neoj2k.map")
    # every symbol resolved at link time; not with sanitizers, whose runtime the program provides
    if(NOT NJ2K_SANITIZE)
      target_link_options(neoj2k_shared PRIVATE LINKER:--no-undefined)
    endif()
    set_property(TARGET neoj2k_shared APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/neoj2k.map")
  endif()
  # The DLL keeps link-time code generation inside: its boundary is one call per image.
  if(NOT NJ2K_SANITIZE)
    check_ipo_supported(RESULT _nj2k_ipo LANGUAGES C CXX)
    if(_nj2k_ipo)
      set_target_properties(neoj2k_shared PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
        INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
    endif()
  endif()
  set_target_properties(neoj2k_shared PROPERTIES EXPORT_NAME shared)
  add_library(neoj2k::shared ALIAS neoj2k_shared)
  list(APPEND NJ2K_LIBRARY_TARGETS neoj2k_shared)
endif()

# The lane-per-code-block MQ prototype bench's copy of the library (bench/nj2k_mq_lanes.cpp, stage
# 4): Tier-1 built with its trace hook, which reports every MQ segment and decision. Never shipped
# or installed; the library targets above are built without the hook. AVX2 builds only, as its one
# user (bench/CMakeLists.txt).
if(NJ2K_BUILD_BENCH AND NJ2K_SIMD STREQUAL "AVX2")
  nj2k_cpu_check_objects(nj2k_t1_trace_cpu_check "NJ2K_STATIC")
  add_library(nj2k_t1_trace STATIC $<TARGET_OBJECTS:nj2k_t1_trace_cpu_check>)
  nj2k_library_common(nj2k_t1_trace)
  target_compile_definitions(nj2k_t1_trace PUBLIC NJ2K_STATIC NJ2K_T1_TRACE)
  nj2k_no_ipo(nj2k_t1_trace)
endif()

if(NJ2K_INSTALL AND NJ2K_LIBRARY_TARGETS)
  include(CMakePackageConfigHelpers)
  install(TARGETS ${NJ2K_LIBRARY_TARGETS} EXPORT neoj2kTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/neoj2k" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(EXPORT neoj2kTargets NAMESPACE neoj2k:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/neoj2k)
  configure_package_config_file("${PROJECT_SOURCE_DIR}/cmake/neoj2kConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/neoj2kConfig.cmake" INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/neoj2k)
  write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/neoj2kConfigVersion.cmake"
    COMPATIBILITY SameMinorVersion)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/neoj2kConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/neoj2kConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/neoj2k)
  if(NJ2K_BUILD_SHARED)
    configure_file("${PROJECT_SOURCE_DIR}/cmake/neoj2k.pc.in" "${CMAKE_CURRENT_BINARY_DIR}/neoj2k.pc" @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/neoj2k.pc" DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
  endif()
  # The library holds code ported from OpenJPEG, whose licence asks for its notice in binary
  # distributions too: it ships next to neo-j2k's.
  install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/LICENSE.OpenJPEG" "${PROJECT_SOURCE_DIR}/NOTICE.md"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/neoj2k)
endif()

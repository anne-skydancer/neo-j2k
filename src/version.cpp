// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
#include <neoj2k/neoj2k.h>

#define NJ2K_STR2(x) #x
#define NJ2K_STR(x) NJ2K_STR2(x)

namespace {

constexpr char kVersion[] =
    NJ2K_STR(NJ2K_VERSION_MAJOR) "." NJ2K_STR(NJ2K_VERSION_MINOR) "." NJ2K_STR(NJ2K_VERSION_PATCH);

// The decoded output's version: it changes only when a decode can give other pixels, so a host's
// texture cache is not purged by releases that leave the output identical (owner decision D13).
constexpr char kEngine[] = "neo-j2k " NJ2K_STR(NJ2K_VERSION_MAJOR) "." NJ2K_STR(NJ2K_VERSION_MINOR);
static_assert(sizeof(kEngine) - 1 <= NJ2K_ENGINE_STRING_MAX, "the engine string must fit a 32-byte cache field");

}  // namespace

extern "C" {

NJ2K_API const char* NJ2K_CALLCONV nj2k_version(void)
{
    return kVersion;
}

NJ2K_API const char* NJ2K_CALLCONV nj2k_engine_string(void)
{
    return kEngine;
}

}  // extern "C"

// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// One encoder variant of the EBCOT-NG study: CMakeLists.txt compiles this file once a variant with ENG_NS
// and the switches of enc_ng.inc defined.
#include <cstring>

#include "arena.h"
#include "common.h"
#include "enc_luts.h"
#include "enc_tile.h"

namespace nj2k::ENG_NS {
#include "enc_ng.inc"
}  // namespace nj2k::ENG_NS

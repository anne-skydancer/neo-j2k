// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The scratch the study's decoder variants decode into (the replay owns it).
#pragma once

#include <cstddef>
#include <cstdint>

#include "t1.h"

namespace ebc {
struct DecScratch {
    std::int32_t* data;
    std::uint32_t* flags;
    std::uint8_t* cblkdata;
    std::size_t cblkdata_size;
};
}  // namespace ebc

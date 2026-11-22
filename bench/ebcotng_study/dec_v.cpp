// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// One decoder variant of the EBCOT-NG study: CMakeLists.txt compiles this file once a variant with DNG_NS and
// the switches of dec_ng.inc defined. Entry: bool ebc::DNG_NS::run(DecScratch&, const T1Block&).
#include <cstring>

#include "common.h"
#include "dec_scratch.h"

namespace ebc::DNG_NS {
#include "dec_ng.inc"

bool run(DecScratch& s, const nj2k::detail::T1Block& b) noexcept
{
    static Dt1 t{};
    t.data = s.data;
    t.flags = s.flags;
    t.cblkdata = s.cblkdata;
    t.cblkdata_size = s.cblkdata_size;
    return decode_cblk(t, b);
}
}  // namespace ebc::DNG_NS

// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The encoder's coefficient preprocessing (EBCOT-NG step E1): each sample becomes its magnitude with the
// sign in bit 31, and the block's largest magnitude is returned. It is OpenJPEG's per-sample branch on the
// sign (a negative sample: INT32_MIN taken as INT32_MIN + 1, negated, bit 31 set; any other sample kept)
// without the branch, in scalar code with no intrinsics:
//   neg = 0 - (x >> 31)             all ones for a negative sample
//   a   = (x ^ neg) - neg           |x|, and 0x80000000 for INT32_MIN (the one value that does not negate)
//   a  -= a >> 31                   0x80000000 becomes 0x7FFFFFFF = -(INT32_MIN + 1); no other a has bit 31
//   out = a | (x & 0x80000000)      the sign back in bit 31
// A magnitude is at most 0x7FFFFFFF, so the maximum is the same unsigned or signed. The function is the
// same as the earlier one for all 2^32 inputs: tests/unit/enc_t1_unit.cpp checks that exhaustively.
// Kept apart from enc_tile.h so that the test can call it alone; it needs <cstdint> only.
#pragma once

#include <cstdint>

namespace nj2k::detail {

inline std::int32_t enc_preprocess(std::int32_t* data, std::uint32_t n) noexcept
{
    std::uint32_t* const p = reinterpret_cast<std::uint32_t*>(data);  // unsigned twin of the same objects
    std::uint32_t mx = 0;
#if defined(_MSC_VER) && !defined(__clang__)
    // scalar code, as the step is specified: MSVC would otherwise vectorise the maximum (pmaxud) behind a
    // run-time CPU check (__isa_available), which a scalar build must not hold (global_state)
#pragma loop(no_vector)
#endif
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint32_t x = p[i];
        const std::uint32_t neg = 0U - (x >> 31);
        std::uint32_t a = (x ^ neg) - neg;
        a -= a >> 31;
        mx = a > mx ? a : mx;
        p[i] = a | (x & 0x80000000U);
    }
    return static_cast<std::int32_t>(mx);
}

}  // namespace nj2k::detail

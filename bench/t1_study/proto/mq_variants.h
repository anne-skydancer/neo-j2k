// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The MQ coder variants of the Tier-1 study, run on a code-block's decisions as the passes made them:
// one byte a decision (context << 1 | bit) and the index of each pass's end. Every variant must give
// the bytes and the raw rates (bytes so far + 3 at a pass end, the flushed length for the last pass)
// of src/enc_t1.cpp's MqEnc.
#pragma once

#include <cstdint>

namespace nj2k {

struct MqResult {
    std::uint32_t nbytes;
    bool overflow;
    std::uint32_t rates[75];
};

// buf[-1] is the fake byte (0); at most cap bytes from buf (the library's out + 1, kOutCapacity)
using MqFn = void (*)(const std::uint8_t* syms, const std::uint32_t* pass_ends, std::uint32_t npasses,
                      std::uint8_t* buf, std::uint32_t cap, MqResult& r);

struct MqVariant {
    const char* name;
    MqFn fn;
};
extern const MqVariant kMqVariants[];
extern const int kMqVariantCount;

namespace study_split {
struct EncT1;
}

}  // namespace nj2k

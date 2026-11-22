// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The study's upload encode: the synthetic texture of bench/nj2k_encode_bench.cpp and the viewer's
// parameters of tests/common/replay.inl (replay_upload_ex), one thread, through OpenJPEG's API names.
#pragma once

#include <cstdint>
#include <vector>

namespace study {

std::vector<std::uint8_t> texture(int width, int height, int comps);

struct UploadTimes {
    std::uint64_t setup = 0, start = 0, encode = 0, end = 0, total = 0;  // TSC cycles
};

// false when the encoder refused
bool upload(const std::vector<std::uint8_t>& raw, int width, int height, int comps, bool reversible,
            std::vector<std::uint8_t>& out, UploadTimes* times);

std::uint64_t fnv64(const std::vector<std::uint8_t>& v);
double tsc_ghz();

}  // namespace study

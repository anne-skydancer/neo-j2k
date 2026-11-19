// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// PGX, the one-component format of the ISO/IEC 15444-4 reference images: a text header
// "PG <ML|LM> [+|-]<depth> <width> <height>" and one whitespace byte, then the samples in 1, 2 or 4
// bytes, big-endian for ML.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nj2k::test {

struct Pgx {
    int width = 0, height = 0, depth = 0;
    bool is_signed = false;
    std::vector<std::int32_t> samples;
};

std::optional<Pgx> read_pgx(const std::string& path, std::string* why = nullptr);

}  // namespace nj2k::test

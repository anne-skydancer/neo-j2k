// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
#include "pgx.h"

#include <cctype>
#include <cstdlib>

#include "common.h"

namespace nj2k::test {

namespace {

bool fail(std::string* why, const char* text)
{
    if (why) {
        *why = text;
    }
    return false;
}

}  // namespace

std::optional<Pgx> read_pgx(const std::string& path, std::string* why)
{
    const auto bytes = read_file(path);
    if (!bytes) {
        fail(why, "cannot read the file");
        return std::nullopt;
    }
    const auto& b = *bytes;
    std::size_t p = 0;
    auto skip_spaces = [&] {
        while (p < b.size() && (b[p] == ' ' || b[p] == '\t')) {
            ++p;
        }
    };
    auto number = [&](long& v) {
        skip_spaces();
        const std::size_t start = p;
        while (p < b.size() && std::isdigit(b[p])) {
            ++p;
        }
        if (p == start || p - start > 9) {
            return false;
        }
        v = std::strtol(std::string(b.begin() + long(start), b.begin() + long(p)).c_str(), nullptr, 10);
        return true;
    };
    if (b.size() < 8 || b[0] != 'P' || b[1] != 'G') {
        fail(why, "not a PGX file");
        return std::nullopt;
    }
    p = 2;
    skip_spaces();
    if (p + 2 > b.size()) {
        fail(why, "header cut");
        return std::nullopt;
    }
    const bool big_endian = b[p] == 'M' && b[p + 1] == 'L';
    if (!big_endian && !(b[p] == 'L' && b[p + 1] == 'M')) {
        fail(why, "bad byte order");
        return std::nullopt;
    }
    p += 2;
    skip_spaces();
    Pgx out;
    if (p < b.size() && (b[p] == '+' || b[p] == '-')) {
        out.is_signed = b[p] == '-';
        ++p;
    }
    long depth = 0, w = 0, h = 0;
    if (!number(depth) || !number(w) || !number(h) || depth < 1 || depth > 32 || w < 1 || h < 1) {
        fail(why, "bad header numbers");
        return std::nullopt;
    }
    if (p >= b.size() || !std::isspace(b[p])) {
        fail(why, "no whitespace after the header");
        return std::nullopt;
    }
    ++p;  // one whitespace byte ends the header
    out.depth = int(depth);
    out.width = int(w);
    out.height = int(h);
    const std::size_t bytes_per = depth <= 8 ? 1 : depth <= 16 ? 2 : 4;
    const std::size_t n = std::size_t(w) * std::size_t(h);
    if (b.size() - p < n * bytes_per) {
        fail(why, "samples cut");
        return std::nullopt;
    }
    out.samples.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        std::uint32_t v = 0;
        for (std::size_t k = 0; k < bytes_per; ++k) {
            const std::uint32_t byte = b[p + i * bytes_per + k];
            v = big_endian ? (v << 8) | byte : v | (byte << (8 * k));
        }
        if (out.is_signed && bytes_per < 4) {
            const std::uint32_t sign = 1u << (8 * bytes_per - 1);
            out.samples[i] = (v & sign) ? std::int32_t(std::int64_t(v) - (std::int64_t(sign) << 1)) : std::int32_t(v);
        } else {
            out.samples[i] = std::int32_t(v);
        }
    }
    return out;
}

}  // namespace nj2k::test

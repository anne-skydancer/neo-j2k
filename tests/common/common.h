// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// Small helpers shared by the tests, the corpus tool, the fuzz drivers and the bench.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nj2k::test {

using Bytes = std::vector<std::uint8_t>;

std::optional<Bytes> read_file(const std::string& path);
bool write_file(const std::string& path, std::span<const std::uint8_t> bytes);

// A list argument: a directory (its *.j2k, *.j2c, *.jpc files, sorted, and *.jp2, *.jph with
// with_jp2), a text file with one path per line ('#' starts a comment; relative paths are taken
// from the list's folder), or one file.
std::vector<std::string> expand_inputs(const std::string& arg, bool with_jp2 = false);
std::string base_name(const std::string& path);
std::string join_path(const std::string& dir, const std::string& name);
// path relative to root when it lies under it (with '/' separators), else the path unchanged
std::string relative_to(const std::string& path, const std::string& root);

std::vector<std::string> split(std::string_view s, char sep);
std::vector<int> parse_int_list(std::string_view s);  // "0,1,2" or "0-5"
std::string trim(std::string_view s);

// FNV-1a, 64 bits: identity checks and digests (not cryptographic).
class Fnv64 {
public:
    void bytes(const void* data, std::size_t n) noexcept
    {
        const auto* p = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < n; ++i) {
            h_ = (h_ ^ p[i]) * 1099511628211ull;
        }
    }
    void u32(std::uint32_t v) noexcept
    {
        const std::uint8_t b[4] = {std::uint8_t(v), std::uint8_t(v >> 8), std::uint8_t(v >> 16), std::uint8_t(v >> 24)};
        bytes(b, 4);
    }
    void u64(std::uint64_t v) noexcept
    {
        u32(std::uint32_t(v));
        u32(std::uint32_t(v >> 32));
    }
    std::uint64_t value() const noexcept { return h_; }

private:
    std::uint64_t h_ = 1469598103934665603ull;
};

std::string hex64(std::uint64_t v);
std::optional<std::uint64_t> parse_hex64(std::string_view s);

// Monotonic wall time in nanoseconds.
std::uint64_t now_ns() noexcept;

// Keeps work whose result nothing reads (the viewer's 8-bit copy in a timed round) from being
// optimised away: the bytes escape into another translation unit.
void keep_alive(const void* data, std::size_t n) noexcept;

// An environment variable's value, "" when it is not set.
std::string env_value(const char* name);

// Every failure reported on the console, never in a dialog, so a run on a desktop cannot stop on a
// message box. On Windows: no Windows Error Reporting or critical-error box (for the processes this
// one starts too), abort() writes nothing and exits with code 3, and assert() and the debug C
// runtime's reports go to stderr. Nothing to do elsewhere. First thing in every program's main()
// (the fuzz targets: fuzz_setup()).
void console_reports() noexcept;

// OpenJPEG starts worker threads when OPJ_NUM_THREADS is set (opj_j2k_create_decompress and
// opj_j2k_create_compress read it), which would make timings and heap counts include its pool and
// run its threaded paths where no ThreadSanitizer looks. The tests, tools, fuzz targets and the
// bench clear it before any OpenJPEG codec exists; this returns the value it had ("" when unset).
std::string single_thread_openjpeg();

}  // namespace nj2k::test

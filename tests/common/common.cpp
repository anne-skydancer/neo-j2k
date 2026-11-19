// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
#include "common.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace nj2k::test {

std::optional<Bytes> read_file(const std::string& path)
{
    std::ifstream in(fs::path(path), std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size < 0) {
        return std::nullopt;
    }
    in.seekg(0, std::ios::beg);
    Bytes data(static_cast<std::size_t>(size));
    if (size > 0 && !in.read(reinterpret_cast<char*>(data.data()), size)) {
        return std::nullopt;
    }
    return data;
}

bool write_file(const std::string& path, std::span<const std::uint8_t> bytes)
{
    std::ofstream out(fs::path(path), std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

std::string trim(std::string_view s)
{
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) {
        ++a;
    }
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) {
        --b;
    }
    return std::string(s.substr(a, b - a));
}

std::vector<std::string> split(std::string_view s, char sep)
{
    std::vector<std::string> out;
    std::size_t a = 0;
    while (a <= s.size()) {
        std::size_t b = s.find(sep, a);
        if (b == std::string_view::npos) {
            b = s.size();
        }
        out.emplace_back(s.substr(a, b - a));
        a = b + 1;
    }
    return out;
}

std::vector<int> parse_int_list(std::string_view s)
{
    std::vector<int> out;
    for (const auto& part : split(s, ',')) {
        const std::string t = trim(part);
        if (t.empty()) {
            continue;
        }
        const auto dash = t.find('-', 1);
        if (dash != std::string::npos) {
            const int a = std::stoi(t.substr(0, dash)), b = std::stoi(t.substr(dash + 1));
            for (int v = a; v <= b; ++v) {
                out.push_back(v);
            }
        } else {
            out.push_back(std::stoi(t));
        }
    }
    return out;
}

static bool is_codestream_name(const fs::path& p)
{
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext == ".j2k" || ext == ".j2c" || ext == ".jpc";
}

std::vector<std::string> expand_inputs(const std::string& arg)
{
    std::vector<std::string> out;
    const fs::path p = fs::path(arg);
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
        for (const auto& e : fs::directory_iterator(p, ec)) {
            if (e.is_regular_file() && is_codestream_name(e.path())) {
                out.push_back(e.path().generic_string());
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }
    if (is_codestream_name(p)) {
        out.push_back(p.generic_string());
        return out;
    }
    std::ifstream in(p);
    std::string line;
    const fs::path base = p.parent_path();
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') {
            continue;
        }
        fs::path item = fs::path(t);
        if (item.is_relative()) {
            item = base / item;
        }
        out.push_back(item.generic_string());
    }
    return out;
}

std::string base_name(const std::string& path)
{
    return fs::path(path).filename().string();
}

std::string join_path(const std::string& dir, const std::string& name)
{
    return (fs::path(dir) / fs::path(name)).generic_string();
}

std::string relative_to(const std::string& path, const std::string& root)
{
    std::error_code ec;
    const fs::path rel = fs::relative(fs::path(path), fs::path(root), ec);
    if (ec || rel.empty() || *rel.begin() == "..") {
        return fs::path(path).generic_string();
    }
    return rel.generic_string();
}

std::string hex64(std::uint64_t v)
{
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

std::optional<std::uint64_t> parse_hex64(std::string_view s)
{
    if (s.empty() || s.size() > 16) {
        return std::nullopt;
    }
    std::uint64_t v = 0;
    for (char c : s) {
        int d;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            d = c - 'A' + 10;
        } else {
            return std::nullopt;
        }
        v = (v << 4) | std::uint64_t(d);
    }
    return v;
}

std::uint64_t now_ns() noexcept
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void keep_alive(const void* data, std::size_t n) noexcept
{
    static volatile std::uint8_t sink;
    if (n) {
        sink = static_cast<const std::uint8_t*>(data)[n - 1];
    }
}

std::string env_value(const char* name)
{
    std::string out;
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&value, &len, name) == 0 && value) {
        out = value;
        std::free(value);
    }
#else
    if (const char* value = std::getenv(name)) {
        out = value;
    }
#endif
    return out;
}

std::string single_thread_openjpeg()
{
    std::string old = env_value("OPJ_NUM_THREADS");
#if defined(_WIN32)
    _putenv_s("OPJ_NUM_THREADS", "");  // an empty value removes the variable
#else
    unsetenv("OPJ_NUM_THREADS");
#endif
    return old;
}

}  // namespace nj2k::test

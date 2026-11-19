// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The fuzz filters' own test: every input of fuzz/bombs must be refused by the filter of the
// target its name starts with (decode-, header-), and the Second Life sized files of the corpus
// (sl_*.j2k) must pass both, at every reduce level they have. The bombs are only scanned here,
// never decoded.
//
//   nj2k_fuzz_filter_check <bombs dir> <corpus dir>
// Exit status: 0, 1 when a filter lets a bomb through or refuses a corpus file, 2 on an input error.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "fuzz_common.h"

namespace fs = std::filesystem;

int main(int argc, char** argv)
{
    nj2k::test::console_reports();
    if (argc != 3) {
        std::fprintf(stderr, "usage: nj2k_fuzz_filter_check <bombs dir> <corpus dir>\n");
        return 2;
    }
    std::vector<fs::path> bombs, corpus;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(argv[1], ec)) {
        if (e.is_regular_file() && e.path().extension() != ".md") {
            bombs.push_back(e.path());
        }
    }
    for (const auto& e : fs::directory_iterator(argv[2], ec)) {
        const std::string name = e.path().filename().string();
        if (e.is_regular_file() && name.rfind("sl_", 0) == 0 && e.path().extension() == ".j2k") {
            corpus.push_back(e.path());
        }
    }
    std::sort(bombs.begin(), bombs.end());
    std::sort(corpus.begin(), corpus.end());
    if (bombs.empty() || corpus.empty()) {
        std::fprintf(stderr, "nj2k_fuzz_filter_check: no bombs or no sl_*.j2k files\n");
        return 2;
    }
    int bad = 0;
    for (const auto& path : bombs) {
        const auto bytes = nj2k::test::read_file(path.string());
        if (!bytes) {
            std::fprintf(stderr, "cannot read %s\n", path.string().c_str());
            return 2;
        }
        const std::string name = path.filename().string();
        bool let_through;
        if (name.rfind("decode-", 0) == 0) {
            const auto in = nj2k::fuzz::split_input(bytes->data(), bytes->size());
            let_through = !in.codestream.empty() && nj2k::fuzz::oracle_may_decode(in.codestream, in.reduce);
        } else if (name.rfind("header-", 0) == 0) {
            let_through = nj2k::fuzz::oracle_may_read_header(*bytes);
        } else {
            std::fprintf(stderr, "%s: the name names no target (decode-, header-)\n", name.c_str());
            return 2;
        }
        if (let_through) {
            ++bad;
            std::printf("LET THROUGH %s\n", name.c_str());
        }
    }
    std::size_t passed = 0;
    for (const auto& path : corpus) {
        const auto bytes = nj2k::test::read_file(path.string());
        const auto mh = bytes ? nj2k::test::parse_main_header(*bytes) : std::nullopt;
        if (!mh) {
            std::fprintf(stderr, "cannot read %s as a codestream\n", path.string().c_str());
            return 2;
        }
        bool ok = nj2k::fuzz::oracle_may_read_header(*bytes);
        for (int r = 0; ok && r < mh->min_resolutions; ++r) {
            ok = nj2k::fuzz::oracle_may_decode(*bytes, r);
        }
        if (!ok) {
            ++bad;
            std::printf("REFUSED %s\n", path.filename().string().c_str());
        } else {
            ++passed;
        }
    }
    std::printf("nj2k_fuzz_filter_check: %zu bombs, %d let through; %zu of %zu corpus files pass\n", bombs.size(),
                bad - int(corpus.size() - passed), passed, corpus.size());
    return bad ? 1 : 0;
}

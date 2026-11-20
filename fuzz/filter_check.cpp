// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The fuzz targets' guards of the oracle, checked against the resource bombs of fuzz/bombs (named
// for the target they are made for: decode-, header-) and the Second Life sized files of the corpus
// (sl_*.j2k):
// - every decode bomb is kept from the oracle by the decode target's OD-3 guard, or refused by the
//   oracle under the decode target's limits, with a limit's message ("the decoder's ... limit"),
//   which the oracle checks before it allocates what the limit bounds; it is decoded here exactly as
//   the target decodes it;
// - every header bomb is refused by the header filter (only scanned: the header target reads with
//   no limits);
// - every sl_*.j2k file passes both guards and decodes under the limits at every reduce level it
//   has, in plain and in viewer mode.
//
//   nj2k_fuzz_filter_check <bombs dir> <corpus dir>
// Exit status: 0, 1 when a bomb gets through or a corpus file is refused, 2 on an input error.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "fuzz_common.h"
#include "j2k_scan.h"

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
    nj2k::fuzz::fuzz_setup();
    const auto oracle = nj2k::test::make_oracle_backend();
    int through = 0, refused = 0;
    for (const auto& path : bombs) {
        const auto bytes = nj2k::test::read_file(path.string());
        if (!bytes) {
            std::fprintf(stderr, "cannot read %s\n", path.string().c_str());
            return 2;
        }
        const std::string name = path.filename().string();
        std::string verdict;
        bool guarded = true;
        if (name.rfind("decode-", 0) == 0) {
            const auto in = nj2k::fuzz::split_input(bytes->data(), bytes->size());
            if (in.codestream.empty()) {
                verdict = "no codestream";
            } else if (in.mode != nj2k::test::Mode::Strict && nj2k::fuzz::oracle_psot_bomb(in.codestream)) {
                verdict = "kept from the oracle (OD-3 guard)";
            } else {
                const nj2k::test::Outcome o = oracle->decode(nj2k::fuzz::decode_request(in));
                guarded = !o.ok && o.last_error.find("the decoder's") != std::string::npos;
                verdict = o.ok ? std::string("decoded by the oracle") : "refused by the oracle: " + o.last_error;
            }
        } else if (name.rfind("header-", 0) == 0) {
            guarded = !nj2k::fuzz::oracle_may_read_header(*bytes);
            verdict = guarded ? "refused by the header filter" : "passes the header filter";
        } else {
            std::fprintf(stderr, "%s: the name names no target (decode-, header-)\n", name.c_str());
            return 2;
        }
        std::printf("%-12s %s: %s\n", guarded ? "guarded" : "GOT THROUGH", name.c_str(), verdict.c_str());
        through += guarded ? 0 : 1;
    }
    std::size_t passed = 0;
    for (const auto& path : corpus) {
        const auto bytes = nj2k::test::read_file(path.string());
        const auto mh = bytes ? nj2k::test::parse_main_header(*bytes) : std::nullopt;
        if (!mh) {
            std::fprintf(stderr, "cannot read %s as a codestream\n", path.string().c_str());
            return 2;
        }
        bool ok = nj2k::fuzz::oracle_may_read_header(*bytes) && !nj2k::fuzz::oracle_psot_bomb(*bytes);
        for (int r = 0; ok && r < mh->min_resolutions; ++r) {
            for (const auto mode : {nj2k::test::Mode::Plain, nj2k::test::Mode::Viewer}) {
                nj2k::fuzz::Input in;
                in.codestream = *bytes;
                in.reduce = r;
                in.mode = mode;
                ok = ok && oracle->decode(nj2k::fuzz::decode_request(in)).ok;
            }
        }
        if (!ok) {
            ++refused;
            std::printf("REFUSED %s\n", path.filename().string().c_str());
        } else {
            ++passed;
        }
    }
    std::printf("nj2k_fuzz_filter_check: %zu bombs, %d got through; %zu of %zu corpus files pass\n", bombs.size(),
                through, passed, corpus.size());
    return through || refused ? 1 : 0;
}

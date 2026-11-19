// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// A driver for fuzz targets built without libFuzzer: runs LLVMFuzzerTestOneInput once per file
// named on the command line (folders are walked, their Markdown notes skipped), so every compiler
// and platform replays the seed corpus and the regression inputs as an ordinary test. A target that
// aborts (its checks failed, after it said why) ends the replay with exit code 134 and the input's
// name, as an ordinary failing test on every platform: CTest reads its output (the known-defect
// checks match it), and no signal or crash dialog is involved.
#include <algorithm>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "common.h"
#include "fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace {

std::string replaying;  // the input being replayed, for the abort report

// abort() raises SIGABRT on the thread that calls it, so the report can use stdio
void on_abort(int)
{
    std::fprintf(stderr, "replay: the target aborted on %s\n", replaying.c_str());
    std::fflush(stderr);
    std::_Exit(134);
}

}  // namespace

int main(int argc, char** argv)
{
    nj2k::test::console_reports();
    std::signal(SIGABRT, on_abort);
    std::size_t inputs = 0;
    for (int i = 1; i < argc; ++i) {
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        if (std::filesystem::is_directory(argv[i], ec)) {
            for (const auto& e : std::filesystem::recursive_directory_iterator(argv[i], ec)) {
                if (e.is_regular_file() && e.path().extension() != ".md") {
                    files.push_back(e.path());
                }
            }
            std::sort(files.begin(), files.end());
        } else {
            files.emplace_back(argv[i]);
        }
        for (const auto& f : files) {
            const auto bytes = nj2k::test::read_file(f.string());
            if (!bytes) {
                std::fprintf(stderr, "cannot read %s\n", f.string().c_str());
                return 2;
            }
            replaying = f.string();
            LLVMFuzzerTestOneInput(bytes->data(), bytes->size());
            ++inputs;
        }
    }
    std::printf("replayed %zu inputs, %zu reached the code under test\n", inputs, nj2k::fuzz::exercised());
    if (inputs && !nj2k::fuzz::exercised()) {
        std::fprintf(stderr, "no input got past the target's filters and guards: the replay tested nothing\n");
        return 1;
    }
    return inputs ? 0 : 2;
}

// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// A driver for fuzz targets built without libFuzzer: runs LLVMFuzzerTestOneInput once per file
// named on the command line (folders are walked), so every compiler and platform replays the seed
// corpus and the regression inputs as an ordinary test.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

#include "common.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

int main(int argc, char** argv)
{
    std::size_t inputs = 0;
    for (int i = 1; i < argc; ++i) {
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        if (std::filesystem::is_directory(argv[i], ec)) {
            for (const auto& e : std::filesystem::recursive_directory_iterator(argv[i], ec)) {
                if (e.is_regular_file()) {
                    files.push_back(e.path());
                }
            }
        } else {
            files.emplace_back(argv[i]);
        }
        for (const auto& f : files) {
            const auto bytes = nj2k::test::read_file(f.string());
            if (!bytes) {
                std::fprintf(stderr, "cannot read %s\n", f.string().c_str());
                return 2;
            }
            LLVMFuzzerTestOneInput(bytes->data(), bytes->size());
            ++inputs;
        }
    }
    std::printf("replayed %zu inputs\n", inputs);
    return inputs ? 0 : 2;
}

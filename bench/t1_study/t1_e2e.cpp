// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// t1_e2e: the one-thread upload encode through the unchanged library (lib_head), median TSC cycles of
// repeats, with the codestream's size and FNV-1a hash (the end-to-end identity of a candidate build).
//   t1_e2e --size N --mode lossy|lossless [--reps 15] [--pin 7]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "driver.h"

int main(int argc, char** argv)
{
    int size = 256, reps = 15, pin = -1;
    bool reversible = false;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--size") {
            size = std::atoi(argv[i + 1]);
        } else if (a == "--mode") {
            reversible = std::string(argv[i + 1]) == "lossless";
        } else if (a == "--reps") {
            reps = std::atoi(argv[i + 1]);
        } else if (a == "--pin") {
            pin = std::atoi(argv[i + 1]);
        }
    }
#if defined(_WIN32)
    if (pin >= 0) {
        SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << pin);
    }
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#endif
    const auto raw = study::texture(size, size, 3);
    std::vector<std::uint8_t> out;
    if (!study::upload(raw, size, size, 3, reversible, out, nullptr)) {
        return 1;
    }
    std::vector<std::uint64_t> t;
    for (int r = 0; r < reps; ++r) {
        study::UploadTimes ut;
        study::upload(raw, size, size, 3, reversible, out, &ut);
        t.push_back(ut.total);
    }
    std::sort(t.begin(), t.end());
    std::printf("%d,%s,%zu,%016llx,%.3f,%.3f\n", size, reversible ? "lossless" : "lossy", out.size(),
                static_cast<unsigned long long>(study::fnv64(out)), double(t[t.size() / 2]) / 1e6, double(t[0]) / 1e6);
    return 0;
}

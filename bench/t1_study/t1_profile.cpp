// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// t1_profile: the one-thread upload encode's stage shares (TSC cycles, median of repeats) from the
// instrumented copy of encode.cpp, and the dump of every code-block's Tier-1 input for t1_replay.
//   t1_profile --size N --mode lossy|lossless [--reps 15] [--pin 7] [--dump file]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "driver.h"
#include "hooks.h"

namespace {

std::uint64_t median(std::vector<std::uint64_t> v)
{
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

bool write_dump(const char* path)
{
    std::FILE* f = std::fopen(path, "wb");
    if (!f) {
        return false;
    }
    const std::uint32_t n = static_cast<std::uint32_t>(study::g_blocks.size());
    std::fwrite("NJT1", 1, 4, f);
    std::fwrite(&n, 4, 1, f);
    for (const auto& b : study::g_blocks) {
        const std::uint32_t hdr[8] = {b.w, b.h, b.orient, b.compno, b.level, b.reversible, b.mct_numcomps, b.mct_table};
        std::fwrite(hdr, 4, 8, f);
        std::fwrite(&b.stepsize, 8, 1, f);
        std::fwrite(b.data.data(), 4, b.data.size(), f);
    }
    std::fclose(f);
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    int size = 256, reps = 15, pin = -1;
    bool reversible = false;
    const char* dump = nullptr;
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
        } else if (a == "--dump") {
            dump = argv[i + 1];
        }
    }
#if defined(_WIN32)
    if (pin >= 0) {
        SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << pin);
    }
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#endif
    const double ghz = study::tsc_ghz();
    const auto raw = study::texture(size, size, 3);
    std::vector<std::uint8_t> out;
    study::g_dump = dump != nullptr;
    if (!study::upload(raw, size, size, 3, reversible, out, nullptr)) {
        std::fprintf(stderr, "refused\n");
        return 1;
    }
    study::g_dump = false;
    if (dump && !write_dump(dump)) {
        std::fprintf(stderr, "cannot write %s\n", dump);
        return 1;
    }
    std::vector<std::uint64_t> st[study::kStages], total, start, encode;
    for (int r = 0; r < reps; ++r) {
        study::g_clock = study::Clock{};
        study::UploadTimes t;
        study::upload(raw, size, size, 3, reversible, out, &t);
        for (int s = 0; s < study::kStages; ++s) {
            st[s].push_back(study::g_clock.cycles[s]);
        }
        total.push_back(t.total);
        start.push_back(t.start);
        encode.push_back(t.encode);
    }
    static const char* names[] = {"build_tile", "dc+mct", "dwt", "t1_copy+quant", "t1_kernel", "t1_commit",
                                  "rate+t2"};
    const double tot = double(median(total));
    std::printf("size=%d mode=%s bytes=%zu fnv=%016llx reps=%d tsc_ghz=%.3f blocks=%zu\n", size,
                reversible ? "lossless" : "lossy", out.size(), static_cast<unsigned long long>(study::fnv64(out)), reps,
                ghz, study::g_blocks.size());
    std::printf("  %-16s %14s %8s\n", "stage", "Mcycles(med)", "share");
    std::printf("  %-16s %14.3f %7.1f%%\n", "upload total", tot / 1e6, 100.0);
    std::printf("  %-16s %14.3f %7.1f%%\n", "start_compress", double(median(start)) / 1e6, 100.0 * median(start) / tot);
    std::printf("  %-16s %14.3f %7.1f%%\n", "encode()", double(median(encode)) / 1e6, 100.0 * median(encode) / tot);
    for (int s = 0; s < study::kStages; ++s) {
        const double m = double(median(st[s]));
        std::printf("  %-16s %14.3f %7.1f%%\n", names[s], m / 1e6, 100.0 * m / tot);
    }
    return 0;
}

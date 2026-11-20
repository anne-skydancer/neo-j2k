// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// nj2k_kernels: the stage gates' kernel timings, each new kernel against the code it replaces,
// interleaved round by round (the median of each), on synthetic planes. Every pair is checked
// equal before it is timed.
//
//   nj2k_kernels [--size 1024] [--rounds 41] [--only output]
//
// - output: the end of a 1024^2 RGB 9/7 decode into the viewer's raw image (8 bits, interleaved,
//   bottom-up): stage 1c's steps (the ICT over the three planes, the DC shift of each, the strided
//   copy of the low bytes) against stage 2's fused pass.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <neoj2k/neoj2k.h>

// the library's internals (by their path: src/ is no include directory)
#include "../src/mct.h"
#include "../src/output.h"

namespace {

using clk = std::chrono::steady_clock;

double median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    return v.empty() ? 0 : v[v.size() / 2];
}

template <typename A, typename B>
void pair(const char* name, int rounds, A&& a, B&& b)
{
    std::vector<double> ta, tb;
    for (int r = 0; r < rounds; ++r) {
        for (int k = 0; k < 2; ++k) {
            const bool first = ((r + k) & 1) == 0;
            const auto t0 = clk::now();
            if (first) {
                a();
            } else {
                b();
            }
            const double us = std::chrono::duration<double, std::micro>(clk::now() - t0).count();
            (first ? ta : tb).push_back(us);
        }
    }
    const double ma = median(ta), mb = median(tb);
    std::printf("%-8s before %10.1f us   after %10.1f us   %6.2fx\n", name, ma, mb, mb > 0 ? ma / mb : 0.0);
}

std::uint32_t float_bits(float f)
{
    std::uint32_t u;
    std::memcpy(&u, &f, sizeof u);
    return u;
}

int bench_output(std::uint32_t n, int rounds)
{
    using namespace nj2k::detail;
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> d(-160.0f, 160.0f);
    const std::size_t samples = std::size_t(n) * n;
    std::vector<std::int32_t> src[3];
    for (auto& s : src) {
        s.resize(samples);
        for (auto& v : s) {
            v = static_cast<std::int32_t>(float_bits(d(rng)));
        }
    }
    std::vector<std::int32_t> work[3];
    std::vector<std::uint8_t> raw_a(samples * 3), raw_b(samples * 3);
    const Finish f{128, 0, 255, false};
    const std::int64_t row = std::int64_t(n) * 3;
    auto before = [&] {
        for (int c = 0; c < 3; ++c) {
            work[c] = src[c];  // the decode's buffer, as the wavelet leaves it
        }
        mct_decode_ict(work[0].data(), work[1].data(), work[2].data(), samples);
        for (int c = 0; c < 3; ++c) {
            dc_shift_97(work[c].data(), n, n, n, f.shift, f.lo, f.hi);
        }
        // stage 1c's write_planes: the low byte, one sample at a time
        for (int c = 0; c < 3; ++c) {
            std::uint8_t* base = raw_a.data() + std::size_t(n - 1) * std::size_t(row) + c;
            for (std::uint32_t j = 0; j < n; ++j) {
                const std::int32_t* s = work[c].data() + std::size_t(j) * n;
                std::uint8_t* r = base - std::int64_t(j) * row;
                for (std::uint32_t i = 0; i < n; ++i) {
                    r[std::size_t(i) * 3] = static_cast<std::uint8_t>(s[i]);
                }
            }
        }
    };
    auto after = [&] {
        for (int c = 0; c < 3; ++c) {
            work[c] = src[c];
        }
        CopyComp cc[3];
        nj2k_plane_t planes[3];
        for (std::uint32_t c = 0; c < 3; ++c) {
            cc[c] = CopyComp{work[c].data(), n, 0, 0, n, n, f, c, true};
            planes[c] = nj2k_plane_t{raw_b.data() + std::size_t(n - 1) * std::size_t(row) + c, -row, 3, c};
        }
        write_group(cc, 3, Mct::kIct, NJ2K_SAMPLE_U8, planes, 3);
    };
    before();
    after();
    if (raw_a != raw_b) {
        std::printf("output: the fused pass differs from stage 1c's steps\n");
        return 1;
    }
    // the copies of the wavelet's output are in both; time them alone to subtract
    std::vector<double> tc;
    for (int r = 0; r < rounds; ++r) {
        const auto t0 = clk::now();
        for (int c = 0; c < 3; ++c) {
            work[c] = src[c];
        }
        tc.push_back(std::chrono::duration<double, std::micro>(clk::now() - t0).count());
    }
    std::printf("output %u^2 RGB 9/7 into 8-bit interleaved bottom-up rows (each time includes %.1f us of plane "
                "copies made for the timing)\n",
                n, median(tc));
    pair("output", rounds, before, after);
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    std::uint32_t size = 1024;
    int rounds = 41;
    std::string only;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--size" && i + 1 < argc) {
            size = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (a == "--rounds" && i + 1 < argc) {
            rounds = std::atoi(argv[++i]);
        } else if (a == "--only" && i + 1 < argc) {
            only = argv[++i];
        } else {
            std::fprintf(stderr, "usage: nj2k_kernels [--size N] [--rounds R] [--only output]\n");
            return 2;
        }
    }
    int fails = 0;
    if (only.empty() || only == "output") {
        fails += bench_output(size, rounds);
    }
    return fails ? 1 : 0;
}

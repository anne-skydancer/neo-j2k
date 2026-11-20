// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// nj2k_kernels: the stage gates' kernel timings, each new kernel against the code it replaces,
// interleaved round by round (the median of each), on synthetic planes. Every pair is checked
// equal before it is timed. The input buffers are restored from their sources before each timed
// call, untimed, so each time is the kernel's alone (net), its input where the restore left it (in
// the L3 at 1024^2, as a decode leaves the wavelet's output) and at a 64-byte boundary (as the
// decoder places each component's samples); gross adds the restore to both.
//
//   nj2k_kernels [--size 1024] [--rounds 41] [--only output|floor|dwt97|dwt53]
//
// - output: the end of a 1024^2 RGB 9/7 decode into the viewer's raw image (8 bits, interleaved,
//   bottom-up): stage 1c's steps (the ICT over the three planes, the DC shift of each, the strided
//   copy of the low bytes) against stage 2's fused pass.
// - floor: what the output pass cannot go below: its three int32 planes read and nothing else (an
//   AVX2 sum), then that read with the 8-bit pixels' bytes written (one xor a sample, plain stores),
//   both restored as for output.
// - dwt97, dwt53: the inverse wavelet of one size^2 plane, five levels, stage 1c's transform
//   (OpenJPEG's structure) against the current one.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

#include <neoj2k/neoj2k.h>

// the library's internals (by their path: src/ is no include directory)
#include "../src/dwt.h"
#include "../src/mct.h"
#include "../src/output.h"

namespace {

using clk = std::chrono::steady_clock;

double median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    return v.empty() ? 0 : v[v.size() / 2];
}

// each round: the restore (untimed) then a, the restore then b, the order alternating; prints the
// medians, their ratio (net) and the ratio with the restore's median added to both (gross)
template <typename P, typename A, typename B>
void pair(const char* name, int rounds, P&& restore, A&& a, B&& b)
{
    std::vector<double> ta, tb, tr;
    for (int r = 0; r < rounds; ++r) {
        for (int k = 0; k < 2; ++k) {
            const bool first = ((r + k) & 1) == 0;
            const auto t0 = clk::now();
            restore();
            const auto t1 = clk::now();
            if (first) {
                a();
            } else {
                b();
            }
            const auto t2 = clk::now();
            tr.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
            (first ? ta : tb).push_back(std::chrono::duration<double, std::micro>(t2 - t1).count());
        }
    }
    const double ma = median(ta), mb = median(tb), mr = median(tr);
    std::printf("%-8s before %10.1f us   after %10.1f us   net %6.2fx   gross %6.2fx (restore %.1f us)\n", name, ma,
                mb, mb > 0 ? ma / mb : 0.0, (ma + mr) / (mb + mr), mr);
}

// samples at a 64-byte boundary, as the decoder places each component's (Decoder's planes)
class Plane {
public:
    explicit Plane(std::size_t n = 0) { resize(n); }
    void resize(std::size_t n)
    {
        buf_.assign(n + 16, 0);
        p_ = reinterpret_cast<std::int32_t*>((reinterpret_cast<std::uintptr_t>(buf_.data()) + 63) & ~std::uintptr_t(63));
        n_ = n;
    }
    std::int32_t* data() noexcept { return p_; }
    const std::int32_t* data() const noexcept { return p_; }
    std::vector<std::int32_t> copy() const { return {p_, p_ + n_}; }

private:
    std::vector<std::int32_t> buf_;
    std::int32_t* p_ = nullptr;
    std::size_t n_ = 0;
};

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
    Plane work[3];
    for (auto& w : work) {
        w.resize(samples);
    }
    std::vector<std::uint8_t> raw_a(samples * 3), raw_b(samples * 3);
    const Finish f{128, 0, 255, false};
    const std::int64_t row = std::int64_t(n) * 3;
    // the decode's buffers, as the wavelet leaves them
    auto restore = [&] {
        for (int c = 0; c < 3; ++c) {
            std::memcpy(work[c].data(), src[c].data(), samples * sizeof(std::int32_t));
        }
    };
    auto before = [&] {
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
        CopyComp cc[3];
        nj2k_plane_t planes[3];
        for (std::uint32_t c = 0; c < 3; ++c) {
            cc[c] = CopyComp{work[c].data(), n, 0, 0, n, n, f, c, true};
            planes[c] = nj2k_plane_t{raw_b.data() + std::size_t(n - 1) * std::size_t(row) + c, -row, 3, c};
        }
        write_group(cc, 3, Mct::kIct, NJ2K_SAMPLE_U8, planes, 3);
    };
    restore();
    before();
    restore();
    after();
    if (raw_a != raw_b) {
        std::printf("output: the fused pass differs from stage 1c's steps\n");
        return 1;
    }
    std::printf("output %u^2 RGB 9/7 into 8-bit interleaved bottom-up rows (%.1f MB of int32 planes in, %.1f MB of "
                "pixels out)\n",
                n, 3.0 * double(samples) * 4 / 1e6, 3.0 * double(samples) / 1e6);
    pair("output", rounds, restore, before, after);
    return 0;
}

// The output pass's floor at this size: its three planes read and nothing else, then that read with
// the pixels' bytes written by plain stores (one xor a sample, no format), both restored as the
// output bench restores its planes
int bench_floor(std::uint32_t n, int rounds)
{
#if defined(__AVX2__)
    const std::size_t samples = std::size_t(n) * n;
    std::vector<std::int32_t> src[3];
    Plane work[3];
    for (int c = 0; c < 3; ++c) {
        src[c].assign(samples, c + 1);
        work[c].resize(samples);
    }
    std::vector<std::uint8_t> raw(samples * 3 + 64);
    auto restore = [&] {
        for (int c = 0; c < 3; ++c) {
            std::memcpy(work[c].data(), src[c].data(), samples * sizeof(std::int32_t));
        }
    };
    volatile std::int32_t sink = 0;
    auto read = [&] {
        __m256i acc = _mm256_setzero_si256();
        for (std::size_t i = 0; i + 8 <= samples; i += 8) {
            for (int c = 0; c < 3; ++c) {
                acc = _mm256_add_epi32(acc, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(work[c].data() + i)));
            }
        }
        sink = _mm256_extract_epi32(acc, 0) + _mm256_extract_epi32(acc, 7);
    };
    // the same traffic with the least work: sixteen pixels a step, 48 bytes into bottom-up rows as
    // the viewer's image, each the xor of the three planes' samples (not the format, the floor)
    auto copy = [&] {
        const std::size_t row = std::size_t(n) * 3;
        for (std::uint32_t j = 0; j < n; ++j) {
            std::uint8_t* d = raw.data() + std::size_t(n - 1 - j) * row;
            const std::int32_t* s0 = work[0].data() + std::size_t(j) * n;
            const std::int32_t* s1 = work[1].data() + std::size_t(j) * n;
            const std::int32_t* s2 = work[2].data() + std::size_t(j) * n;
            for (std::uint32_t i = 0; i + 16 <= n; i += 16, d += 48) {
                const auto at = [&](const std::int32_t* p, std::uint32_t o) {
                    return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + i + o));
                };
                const __m256i v = _mm256_xor_si256(_mm256_xor_si256(at(s0, 0), at(s1, 0)), at(s2, 0));
                const __m256i w = _mm256_xor_si256(_mm256_xor_si256(at(s0, 8), at(s1, 8)), at(s2, 8));
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(d), v);
                _mm_storeu_si128(reinterpret_cast<__m128i*>(d + 32), _mm256_castsi256_si128(w));
            }
        }
    };
    std::printf("floor %u^2: three int32 planes read (%.1f MB; before), then with %.1f MB of pixels written (after)\n",
                n, 3.0 * double(samples) * 4 / 1e6, 3.0 * double(samples) / 1e6);
    pair("floor", rounds, restore, read, copy);
#else
    (void)n;
    (void)rounds;
    std::printf("floor: AVX2 builds only\n");
#endif
    return 0;
}

int bench_dwt(std::uint32_t n, bool reversible, int rounds)
{
    using namespace nj2k::detail;
    std::mt19937 rng(11);
    std::uniform_int_distribution<int> di(-2000, 2000);
    std::uniform_real_distribution<float> df(-200.0f, 200.0f);
    const std::uint32_t numres = 6;
    ResRect res[6];
    for (std::uint32_t r = 0; r < numres; ++r) {
        const std::int32_t s = static_cast<std::int32_t>((n + (1u << (numres - 1 - r)) - 1) >> (numres - 1 - r));
        res[r] = ResRect{0, 0, s, s};
    }
    const std::size_t samples = std::size_t(n) * n;
    std::vector<std::int32_t> src(samples), b;
    Plane a(samples);
    for (auto& v : src) {
        v = reversible ? di(rng) : static_cast<std::int32_t>(float_bits(df(rng)));
    }
    std::vector<std::uint8_t> scratch_a(kernels::dwt_scratch_bytes_reference(res, numres, reversible) + 64);
    std::vector<std::uint8_t> scratch_b(dwt_scratch_bytes(res, numres, reversible) + 64);
    auto align = [](std::vector<std::uint8_t>& v) {
        return reinterpret_cast<void*>((reinterpret_cast<std::uintptr_t>(v.data()) + 63) & ~std::uintptr_t(63));
    };
    void* sa = align(scratch_a);
    void* sb = align(scratch_b);
    // one plane for both, restored before each call
    auto restore = [&] { std::memcpy(a.data(), src.data(), samples * sizeof(std::int32_t)); };
    auto before = [&] { kernels::dwt_decode_reference(a.data(), n, res, numres, reversible, sa); };
    auto after = [&] { dwt_decode(a.data(), n, res, numres, reversible, sb); };
    restore();
    before();
    b = a.copy();
    restore();
    after();
    if (a.copy() != b) {
        std::printf("dwt: the transform differs from stage 1c's\n");
        return 1;
    }
    std::printf("%s %u^2, %u levels\n", reversible ? "dwt53" : "dwt97", n, numres - 1);
    pair(reversible ? "dwt53" : "dwt97", rounds, restore, before, after);
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
            std::fprintf(stderr, "usage: nj2k_kernels [--size N] [--rounds R] [--only output|floor|dwt97|dwt53]\n");
            return 2;
        }
    }
    int fails = 0;
    if (only.empty() || only == "output") {
        fails += bench_output(size, rounds);
    }
    if (only.empty() || only == "floor") {
        fails += bench_floor(size, rounds);
    }
    if (only.empty() || only == "dwt97") {
        fails += bench_dwt(size, false, rounds);
    }
    if (only.empty() || only == "dwt53") {
        fails += bench_dwt(size, true, rounds);
    }
    return fails ? 1 : 0;
}

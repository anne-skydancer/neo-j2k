// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// nj2k_enc_t1_ratio: the encoder's Tier-1 kernel (enc_t1_encode, the redesigned MQ coder) against the frozen
// reference (tests/unit/enc_t1_frozen.cpp, the coder and passes as they were before the redesign), in ONE
// process, in interleaved rounds, on generated 64 x 64 code-blocks. The single-process ratio is the metric:
// a shared, noisy host moves both sides alike, so read the ratio's median, p10 and p90, and the absolute
// Mcycles only as a sanity check. Old and new outputs (passes, distortion bit for bit, bytes) are compared
// after every encode, outside the timed part; any difference stops the run with exit code 1.
//
//   nj2k_enc_t1_ratio [--rounds 41] [--blocks 256] [--warmup 3] [--pin CPU] [--quiet]
//
// Two block sets: "lossy" (9/7, small sparse coefficients, about 6 bit-planes) and "lossless" (5/3, larger
// ones, about 8 bit-planes). Each round times every block of the set once through the new coder and once
// through the old (the order alternates from round to round so drift cancels), as the sum of the TSC
// cycles (x86) or nanoseconds (elsewhere) of the enc_t1_encode calls alone.
//
// Output: the environment lines (compiler, CPU, build, which decision form), then per set a line a round
//   "<set> round <n>  new <Mcyc>  old <Mcyc>  ratio <new/old>"
// then "SUMMARY <set> ratio median .. p10 .. p90 .. | old Mcyc median .. new Mcyc median .. | rounds .. blocks .."
// and one machine-readable "RESULT ..." line a set.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sched.h>
#include <time.h>
#endif
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define NJ2K_X86 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#include <x86intrin.h>
#endif
#endif

#include "../src/arena.h"
#include "../src/enc_tile.h"
#include "../tests/unit/enc_t1_frozen.h"

namespace {

namespace d = nj2k::detail;
namespace fz = nj2k::frozen;

struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed) {}
    std::uint32_t next() noexcept
    {
        s += 0x9e3779b97f4a7c15ull;
        std::uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return static_cast<std::uint32_t>((z ^ (z >> 31)) >> 16);
    }
    double unit() noexcept { return (next() + 0.5) / 4294967296.0; }
};

// the clock: TSC cycles on x86, nanoseconds elsewhere
inline std::uint64_t ticks() noexcept
{
#if defined(NJ2K_X86)
    return __rdtsc();
#elif defined(_WIN32)
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<std::uint64_t>(t.QuadPart);
#else
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
#endif
}
const char* tick_unit() noexcept
{
#if defined(NJ2K_X86)
    return "Mcycles (TSC)";
#else
    return "Mns";
#endif
}

std::string cpu_model()
{
    std::string model;
#if defined(__linux__)
    if (std::FILE* f = std::fopen("/proc/cpuinfo", "r")) {
        char line[512];
        while (std::fgets(line, sizeof line, f)) {
            if (std::strncmp(line, "model name", 10) == 0) {
                const char* c = std::strchr(line, ':');
                if (c) {
                    model = c + 1;
                    while (!model.empty() && (model.back() == '\n' || model.back() == '\r')) {
                        model.pop_back();
                    }
                    model.erase(0, model.find_first_not_of(" \t"));
                }
                break;
            }
        }
        std::fclose(f);
    }
#endif
#if defined(NJ2K_X86)
    if (model.empty()) {
        int r[4] = {0, 0, 0, 0};
        char brand[49] = {0};
#if defined(_MSC_VER)
        __cpuid(r, static_cast<int>(0x80000000));
#else
        __cpuid(0x80000000u, r[0], r[1], r[2], r[3]);
#endif
        if (static_cast<unsigned>(r[0]) >= 0x80000004u) {
            for (unsigned i = 0; i < 3; ++i) {
#if defined(_MSC_VER)
                __cpuid(r, static_cast<int>(0x80000002u + i));
#else
                __cpuid(0x80000002u + i, r[0], r[1], r[2], r[3]);
#endif
                std::memcpy(brand + 16 * i, r, 16);
            }
            model = brand;
            model.erase(0, model.find_first_not_of(' '));
        }
    }
#endif
    while (!model.empty() && (model.back() == ' ')) {
        model.pop_back();
    }
    return model.empty() ? "unknown" : model;
}

std::string compiler_id()
{
    char buf[256];
#if defined(__clang__)
    std::snprintf(buf, sizeof buf, "clang %s", __clang_version__);
#elif defined(__GNUC__)
    std::snprintf(buf, sizeof buf, "gcc %d.%d.%d (%s)", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__, __VERSION__);
#elif defined(_MSC_VER)
    std::snprintf(buf, sizeof buf, "msvc %d", static_cast<int>(_MSC_FULL_VER));
#else
    std::snprintf(buf, sizeof buf, "unknown compiler");
#endif
    return buf;
}

// ---- the blocks ----

struct Block {
    std::vector<std::int32_t> data;  // 64 x 64
    std::uint32_t orient;
    std::uint32_t level;
    double stepsize;
};

// A Laplacian-like high band: value = sign * scale * -ln(u), dead-zoned. scale is in quantiser steps;
// the encoder's samples carry 6 fractional bits (T1_NMSEDEC_FRACBITS), so the integers are scaled by 64.
std::vector<Block> make_blocks(bool lossless, std::uint32_t n)
{
    std::vector<Block> blocks(n);
    Rng rng(lossless ? 7001 : 3001);
    for (std::uint32_t b = 0; b < n; ++b) {
        Block& k = blocks[b];
        k.data.resize(64 * 64);
        k.orient = b & 3u;
        k.level = (b >> 2) % 5u;
        k.stepsize = 0.0625 + (b % 7) * 0.05;
        // per block activity: busy, medium, quiet (a texture has all three)
        const double act = 0.35 + 2.2 * std::pow(rng.unit(), 2.0);
        const double scale = (lossless ? 24.0 : 5.0) * act;       // in steps
        const double dead = lossless ? 0.0 : 0.6;                  // the lossy dead zone
        for (std::uint32_t y = 0; y < 64; ++y) {
            for (std::uint32_t x = 0; x < 64; ++x) {
                // an edge-like ridge makes the activity vary inside the block
                const double ridge = 1.0 + 3.0 * std::exp(-std::fabs(static_cast<double>(x) - 32.0 - 9.0 * std::sin(y * 0.12)) * 0.25);
                double v = -std::log(rng.unit()) * scale * ridge;
                v = v < dead ? 0.0 : v;
                std::int32_t q = static_cast<std::int32_t>(v * 64.0);
                if (rng.next() & 1u) {
                    q = -q;
                }
                k.data[y * 64 + x] = q;
            }
        }
    }
    return blocks;
}

// ---- running and checking ----

struct Out {
    std::uint32_t numbps = 0, total = 0;
    bool ok = true;
    std::uint64_t hash = 1469598103934665603ull;
};

inline void mix(Out& o, const void* p, std::size_t n) noexcept
{
    const auto* b = static_cast<const std::uint8_t*>(p);
    for (std::size_t i = 0; i < n; ++i) {
        o.hash = (o.hash ^ b[i]) * 1099511628211ull;
    }
}

// Hash of what the encoder produced: numbps, the passes (rate, len, term, distortion bits) and the bytes.
void digest(Out& o, const d::EncCblk& c)
{
    mix(o, &c.numbps, 4);
    mix(o, &c.totalpasses, 4);
    for (std::uint32_t i = 0; i < c.totalpasses; ++i) {
        mix(o, &c.passes[i].rate, 4);
        mix(o, &c.passes[i].len, 4);
        mix(o, &c.passes[i].term, 1);
        mix(o, &c.passes[i].distortiondec, 8);
    }
    if (c.totalpasses) {
        mix(o, c.data, c.passes[c.totalpasses - 1].rate);
    }
}

struct Pair {
    d::EncT1* neo;
    fz::EncT1* ref;
    d::Allocator alloc;
};

// One round over a set with one coder: the timed sum of the encode calls, and the digest of every result.
template <bool kNew>
std::uint64_t run_set(Pair& p, const std::vector<Block>& blocks, bool reversible, Out& out, bool* all_ok)
{
    std::uint64_t t = 0;
    out = Out{};
    d::Arena arena;
    arena.init(&p.alloc);
    for (const Block& b : blocks) {
        d::EncCblk c{};
        c.x1 = 64;
        c.y1 = 64;
        std::int32_t* dst = kNew ? d::enc_t1_data(p.neo) : fz::enc_t1_data(p.ref);
        std::memcpy(dst, b.data.data(), 64 * 64 * sizeof(std::int32_t));
        const std::uint64_t t0 = ticks();
        const bool ok = kNew ? d::enc_t1_encode(p.neo, c, b.orient, 0, b.level, reversible, b.stepsize, nullptr, 0)
                             : fz::enc_t1_encode(p.ref, c, b.orient, 0, b.level, reversible, b.stepsize, nullptr, 0);
        t += ticks() - t0;
        if (!ok) {
            out.ok = false;
            *all_ok = false;
            continue;
        }
        arena.reset(1u << 16);
        if (c.totalpasses) {
            const bool kept = kNew ? d::enc_t1_commit(p.neo, arena, c) : fz::enc_t1_commit(p.ref, arena, c);
            if (!kept) {
                *all_ok = false;
                continue;
            }
        }
        digest(out, c);
    }
    return t;
}

double pct(std::vector<double> v, double q)
{
    std::sort(v.begin(), v.end());
    const double pos = q * static_cast<double>(v.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(pos);
    const std::size_t hi = std::min(lo + 1, v.size() - 1);
    return v[lo] + (v[hi] - v[lo]) * (pos - static_cast<double>(lo));
}

bool run(const char* name, bool lossless, std::uint32_t nblocks, int rounds, int warmup, bool quiet, Pair& p)
{
    const std::vector<Block> blocks = make_blocks(lossless, nblocks);
    const bool reversible = lossless;
    bool all_ok = true;
    Out on, of;
    for (int w = 0; w < warmup; ++w) {
        run_set<true>(p, blocks, reversible, on, &all_ok);
        run_set<false>(p, blocks, reversible, of, &all_ok);
    }
    std::vector<double> ratio, mn, mo;
    std::uint64_t first_hash = 0;
    for (int r = 0; r < rounds; ++r) {
        std::uint64_t tn, to;
        if (r & 1) {
            to = run_set<false>(p, blocks, reversible, of, &all_ok);
            tn = run_set<true>(p, blocks, reversible, on, &all_ok);
        } else {
            tn = run_set<true>(p, blocks, reversible, on, &all_ok);
            to = run_set<false>(p, blocks, reversible, of, &all_ok);
        }
        if (on.hash != of.hash) {
            std::fprintf(stderr, "MISMATCH: %s round %d: the new and the frozen coder's outputs differ\n", name, r);
            return false;
        }
        if (r == 0) {
            first_hash = on.hash;
        } else if (on.hash != first_hash) {
            std::fprintf(stderr, "MISMATCH: %s round %d: the output changed between rounds\n", name, r);
            return false;
        }
        const double a = static_cast<double>(tn) / 1e6, b = static_cast<double>(to) / 1e6;
        ratio.push_back(a / b);
        mn.push_back(a);
        mo.push_back(b);
        if (!quiet) {
            std::printf("%s round %2d  new %9.3f  old %9.3f  ratio %.4f\n", name, r + 1, a, b, a / b);
        }
    }
    if (!all_ok) {
        std::fprintf(stderr, "a block overflowed or failed to commit in %s\n", name);
        return false;
    }
    const double med = pct(ratio, 0.5), p10 = pct(ratio, 0.1), p90 = pct(ratio, 0.9);
    const double nm = pct(mn, 0.5), om = pct(mo, 0.5);
    std::printf("SUMMARY %s ratio median %.4f p10 %.4f p90 %.4f | old %.3f new %.3f %s median | rounds %d blocks %u | output hash %016llx identical\n",
                name, med, p10, p90, om, nm, tick_unit(), rounds, nblocks, static_cast<unsigned long long>(first_hash));
    std::printf("RESULT set=%s ratio_median=%.4f ratio_p10=%.4f ratio_p90=%.4f old_mcyc=%.3f new_mcyc=%.3f\n", name, med, p10, p90, om, nm);
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    int rounds = 41, warmup = 3, pin = -1;
    std::uint32_t blocks = 256;
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--quiet") {
            quiet = true;
        } else if (i + 1 < argc && a == "--rounds") {
            rounds = std::max(3, std::atoi(argv[++i]));
        } else if (i + 1 < argc && a == "--blocks") {
            blocks = static_cast<std::uint32_t>(std::max(8, std::atoi(argv[++i])));
        } else if (i + 1 < argc && a == "--warmup") {
            warmup = std::max(0, std::atoi(argv[++i]));
        } else if (i + 1 < argc && a == "--pin") {
            pin = std::atoi(argv[++i]);
        } else {
            std::fprintf(stderr, "usage: nj2k_enc_t1_ratio [--rounds N] [--blocks N] [--warmup N] [--pin CPU] [--quiet]\n");
            return 2;
        }
    }
#if defined(_WIN32)
    if (pin >= 0) {
        SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(1) << pin);
    }
#elif defined(__linux__)
    if (pin >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(pin, &set);
        sched_setaffinity(0, sizeof set, &set);
    }
#endif
    const char* form =
#if defined(__GNUC__) && !defined(__clang__)
        "masks (GCC)";
#else
        "selects";
#endif
    const char* simd =
#if defined(__AVX2__)
        "AVX2";
#else
        "baseline (no AVX2)";
#endif
    std::printf("nj2k_enc_t1_ratio: compiler %s\n", compiler_id().c_str());
    std::printf("nj2k_enc_t1_ratio: cpu %s\n", cpu_model().c_str());
    std::printf("nj2k_enc_t1_ratio: build %s, decision form %s, clock %s, pin %d, rounds %d, blocks %u, warmup %d\n", simd, form,
                tick_unit(), pin, rounds, blocks, warmup);

    Pair p{};
    p.alloc = d::Allocator::system();
    p.neo = d::enc_t1_create(p.alloc);
    p.ref = fz::enc_t1_create(p.alloc);
    if (!p.neo || !p.ref) {
        std::fprintf(stderr, "no memory\n");
        return 1;
    }
    bool ok = run("lossy", false, blocks, rounds, warmup, quiet, p);
    ok = run("lossless", true, blocks, rounds, warmup, quiet, p) && ok;
    d::enc_t1_destroy(p.neo, p.alloc);
    fz::enc_t1_destroy(p.ref, p.alloc);
    if (!ok) {
        return 1;
    }
    std::printf("nj2k_enc_t1_ratio: ok, outputs byte-identical in every round\n");
    return 0;
}

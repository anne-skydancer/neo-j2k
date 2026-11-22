// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// mqng_replay: the dumped code-blocks (mqng_profile) through the unchanged library's t1_decode_cblk and
// through the study's variants (proto/variant.cpp). Every variant's coefficients and result are checked
// against the library's on every block of every dump given; then, per configuration, the counting variant's
// tallies, the MQ decoder alone and the passes alone on the recording, the pass clock, and the variants timed
// in interleaved rounds (the order rotating each round), median of the per-round ratios to the library.
//
//   mqng_replay --dir DIR [--configs 256_lossy,...] [--check-dir DIR2] [--rounds 11] [--only a,b]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <intrin.h>
#define NOMINMAX
#include <windows.h>

#include "blockio.h"
#include "proto/common.h"

using nj2k::detail::T1Block;

bool run_v0(mqs::Scratch&, const T1Block&) noexcept;
bool run_v1(mqs::Scratch&, const T1Block&) noexcept;
bool run_v2(mqs::Scratch&, const T1Block&) noexcept;
bool run_v3(mqs::Scratch&, const T1Block&) noexcept;
bool run_v4(mqs::Scratch&, const T1Block&) noexcept;
bool run_v5(mqs::Scratch&, const T1Block&) noexcept;
bool run_v6(mqs::Scratch&, const T1Block&) noexcept;
bool run_v7(mqs::Scratch&, const T1Block&) noexcept;
bool run_v8(mqs::Scratch&, const T1Block&) noexcept;
bool run_v9(mqs::Scratch&, const T1Block&) noexcept;
bool run_v10(mqs::Scratch&, const T1Block&) noexcept;
bool run_v11(mqs::Scratch&, const T1Block&) noexcept;
bool run_v12(mqs::Scratch&, const T1Block&) noexcept;
bool run_v13(mqs::Scratch&, const T1Block&) noexcept;
bool run_v14(mqs::Scratch&, const T1Block&) noexcept;
bool run_v15(mqs::Scratch&, const T1Block&) noexcept;
bool run_v16(mqs::Scratch&, const T1Block&) noexcept;
bool run_v17(mqs::Scratch&, const T1Block&) noexcept;
bool run_v18(mqs::Scratch&, const T1Block&) noexcept;

namespace mqs {
std::size_t alone_m0(Recording& r) noexcept;
std::size_t alone_m1(Recording& r) noexcept;
std::size_t alone_m2(Recording& r) noexcept;
std::size_t alone_m3(Recording& r) noexcept;
extern std::uint64_t g_pass_cycles[4], g_pass_count[4];
}  // namespace mqs

namespace {

using Run = bool (*)(mqs::Scratch&, const T1Block&) noexcept;
struct Variant {
    const char* name;
    Run run;
};
const Variant kVariants[] = {
    {"copy", run_v0}, {"pb", run_v1},  {"ps", run_v2},    {"wb", run_v3},    {"ws", run_v4},
    {"mix", run_v5},  {"wmix", run_v6}, {"err", run_v7},  {"count", run_v8}, {"time", run_v10},
    {"psr", run_v11}, {"mixnd", run_v12}, {"wmixs", run_v13}, {"wagg", run_v14}, {"wspec", run_v15}, {"wchk", run_v16}, {"wall", run_v17}, {"wfn", run_v18},
};

struct Set {
    std::string name;
    std::vector<mqs::DumpBlock> blocks;
    std::vector<nj2k::detail::Chunk> chunks;
    std::vector<std::vector<nj2k::detail::Segment>> segs;
    std::vector<T1Block> t1;
    std::size_t samples = 0, bytes = 0, max_bytes = 0;
};

bool load(const std::string& path, Set& s)
{
    if (!mqs::read_blocks(path, s.blocks)) {
        return false;
    }
    s.chunks.resize(s.blocks.size());
    s.segs.resize(s.blocks.size());
    for (std::size_t i = 0; i < s.blocks.size(); ++i) {
        const mqs::DumpBlock& d = s.blocks[i];
        T1Block b{};
        b.w = d.w, b.h = d.h, b.numbps = d.numbps, b.orient = d.orient, b.roishift = d.roishift, b.cblksty = d.cblksty;
        s.chunks[i] = nj2k::detail::Chunk{d.bytes.data(), static_cast<std::uint32_t>(d.bytes.size())};
        b.chunks = &s.chunks[i];
        b.nchunks = d.bytes.empty() ? 0 : 1;
        for (std::size_t k = 0; k < d.seg_len.size(); ++k) {
            nj2k::detail::Segment g{};
            g.len = d.seg_len[k];
            g.real_num_passes = d.seg_passes[k];
            s.segs[i].push_back(g);
        }
        b.segs = s.segs[i].data();
        b.nsegs = static_cast<std::uint32_t>(s.segs[i].size());
        s.t1.push_back(b);
        s.samples += std::size_t(d.w) * d.h;
        s.bytes += d.bytes.size();
        s.max_bytes = std::max(s.max_bytes, d.bytes.size());
    }
    return true;
}

struct Env {
    nj2k::detail::T1 lib;
    nj2k::detail::Allocator alloc = nj2k::detail::Allocator::system();
    nj2k::detail::Messages quiet;
    mqs::Scratch scratch{};
    std::vector<std::int32_t> data;
    std::vector<std::uint32_t> flags;
    std::vector<std::uint8_t> cblk;
};

void prepare(Env& e, std::size_t max_bytes)
{
    e.lib.ready(e.alloc);
    e.lib.reserve(e.alloc, max_bytes);
    e.data.assign(4096 + 16, 0);
    e.flags.assign(3078 + 16, 0);
    if (e.cblk.size() < max_bytes + 64) {
        e.cblk.assign(max_bytes + 64, 0);
    }
    e.scratch = mqs::Scratch{e.data.data(), e.flags.data(), e.cblk.data(), e.cblk.size()};
}

// every variant against the library on every block; the number of blocks that differ
std::size_t check(Env& e, const Set& s, const Variant* only, std::size_t nonly)
{
    std::size_t bad = 0;
    std::vector<std::int32_t> ref;
    for (const T1Block& b : s.t1) {
        if (b.nchunks == 0) {
            continue;
        }
        const bool r = nj2k::detail::t1_decode_cblk(e.lib, b, e.quiet);
        ref.assign(e.lib.data, e.lib.data + std::size_t(b.w) * b.h);
        for (std::size_t v = 0; v < nonly; ++v) {
            if (only[v].run == run_v8) {
                continue;  // checked through its own run (it records)
            }
            const bool rv = only[v].run(e.scratch, b);
            if (rv != r || (r && std::memcmp(ref.data(), e.data.data(), ref.size() * 4) != 0)) {
                if (bad < 5) {
                    std::printf("  MISMATCH %s: %s block %ux%u sty %x\n", s.name.c_str(), only[v].name, b.w, b.h, b.cblksty);
                }
                ++bad;
            }
        }
    }
    return bad;
}

std::uint64_t time_lib(Env& e, const Set& s)
{
    const std::uint64_t t0 = __rdtsc();
    for (const T1Block& b : s.t1) {
        if (b.nchunks) {
            nj2k::detail::t1_decode_cblk(e.lib, b, e.quiet);
        }
    }
    return __rdtsc() - t0;
}

std::uint64_t time_variant(Env& e, const Set& s, Run run)
{
    const std::uint64_t t0 = __rdtsc();
    for (const T1Block& b : s.t1) {
        if (b.nchunks) {
            run(e.scratch, b);
        }
    }
    return __rdtsc() - t0;
}

double median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    return v.empty() ? 0 : v[v.size() / 2];
}

void stats(Env& e, const Set& s, int rounds)
{
    mqs::g_counts = {};
    mqs::g_rec = {};
    mqs::g_rec.on = true;
    std::size_t bad = 0;
    std::vector<std::int32_t> ref;
    for (const T1Block& b : s.t1) {
        if (!b.nchunks) {
            continue;
        }
        nj2k::detail::t1_decode_cblk(e.lib, b, e.quiet);
        ref.assign(e.lib.data, e.lib.data + std::size_t(b.w) * b.h);
        run_v8(e.scratch, b);
        bad += std::memcmp(ref.data(), e.data.data(), ref.size() * 4) != 0;
    }
    mqs::g_rec.on = false;
    const mqs::Counts& k = mqs::g_counts;
    const double n = double(k.decisions);
    std::printf("[%s] count variant %s; %zu blocks, %zu samples, %zu bytes\n", s.name.c_str(), bad ? "MISMATCH" : "identical",
                s.t1.size(), s.samples, s.bytes);
    std::printf("  decisions %llu (%.3f a sample), raw %llu; segments %llu; passes sig %llu ref %llu cln %llu raw %llu\n",
                (unsigned long long)k.decisions, n / double(s.samples), (unsigned long long)k.raw_decisions,
                (unsigned long long)k.segments, (unsigned long long)k.passes[0], (unsigned long long)k.passes[1],
                (unsigned long long)k.passes[2], (unsigned long long)k.raw_passes);
    std::printf("  decisions sig/ref/cln %% %.1f / %.1f / %.1f; LPS interval %.1f %%; MPS without renorm %.1f %%; "
                "renorm %.1f %% (%.2f shifts); LPS symbol %.1f %%; d=1 %.1f %%\n",
                100 * k.pass_dec[0] / n, 100 * k.pass_dec[1] / n, 100 * k.pass_dec[2] / n, 100 * k.lps_interval / n,
                100 * k.mps_norenorm / n, 100 * k.renorms / n, double(k.shifts) / double(k.renorms),
                100 * k.d_lps_symbol / n, 100 * k.d_one / n);
    std::printf("  byte-ins a decision %.3f (markers %.4f)\n", k.byteins / n, k.markers / n);
    std::printf("  branch sites (taken %%, last-outcome misses %%): LPS-interval %.1f/%.1f, renorm %.1f/%.1f, "
                "exchange %.1f/%.1f, d %.1f/%.1f\n",
                100 * k.site_taken[0] / n, 100 * k.site_flips[0] / n, 100 * k.site_taken[1] / n, 100 * k.site_flips[1] / n,
                100 * k.site_taken[2] / n, 100 * k.site_flips[2] / n, 100 * k.site_taken[3] / n, 100 * k.site_flips[3] / n);
    std::printf("  contexts %%:");
    for (int c = 0; c < 19; ++c) {
        std::printf(" %d:%.1f", c, 100 * k.ctx[c] / n);
    }
    std::printf("\n  MPS-no-renorm share by context %%:");
    for (int c = 0; c < 19; ++c) {
        std::printf(" %d:%.0f", c, k.ctx[c] ? 100.0 * k.ctx_mps_norenorm[c] / double(k.ctx[c]) : 0.0);
    }
    const char* bn[8] = {"1", "2", "3-4", "5-8", "9-16", "17-32", "33-64", "65+"};
    std::printf("\n  same-context MPS-no-renorm runs:");
    for (int b = 0; b < 8; ++b) {
        std::printf(" %s:%llu", bn[b], (unsigned long long)k.run_hist[b]);
    }
    std::printf("; decisions in runs >= 2: %.1f %%\n", 100 * k.run_hist[8] / n);
    std::printf("  AGG MPS-no-renorm runs:");
    for (int b = 0; b < 8; ++b) {
        std::printf(" %s:%llu", bn[b], (unsigned long long)k.agg_run_hist[b]);
    }
    std::printf("; decisions in runs >= 2: %.1f %%\n", 100 * k.agg_run_hist[8] / n);

    // the MQ decoder alone and the passes alone, against the library's whole Tier-1, interleaved
    std::vector<double> rlib, r0, r1, r2, r3, rmod, tlib;
    std::size_t bad_alone = 0;
    for (int r = 0; r < rounds; ++r) {
        for (int k2 = 0; k2 < 6; ++k2) {
            const int which = (k2 + r) % 6;
            std::uint64_t t0 = __rdtsc(), dt = 0;
            switch (which) {
            case 0:
                dt = time_lib(e, s);
                tlib.push_back(double(dt));
                break;
            case 1:
                bad_alone += mqs::alone_m0(mqs::g_rec);
                dt = __rdtsc() - t0;
                r0.push_back(double(dt));
                break;
            case 2:
                bad_alone += mqs::alone_m1(mqs::g_rec);
                dt = __rdtsc() - t0;
                r1.push_back(double(dt));
                break;
            case 3:
                bad_alone += mqs::alone_m2(mqs::g_rec);
                dt = __rdtsc() - t0;
                r2.push_back(double(dt));
                break;
            case 4:
                bad_alone += mqs::alone_m3(mqs::g_rec);
                dt = __rdtsc() - t0;
                r3.push_back(double(dt));
                break;
            case 5:
                mqs::g_bits = mqs::g_rec.sym.data();
                dt = time_variant(e, s, run_v9);
                rmod.push_back(double(dt));
                break;
            }
        }
    }
    const double L = median(tlib);
    std::printf("  library Tier-1 %.2f Mcycles = %.1f cycles a decision; fractions of it: MQ alone M0 %.3f M1 %.3f M2 %.3f "
                "M3 %.3f, passes alone %.3f %s\n",
                L / 1e6, L / n, median(r0) / L, median(r1) / L, median(r2) / L, median(r3) / L, median(rmod) / L,
                bad_alone ? "ALONE MISMATCH" : "(alone identical)");
    // the pass clock
    for (int i = 0; i < 4; ++i) {
        mqs::g_pass_cycles[i] = mqs::g_pass_count[i] = 0;
    }
    time_variant(e, s, run_v10);
    const std::uint64_t pt = mqs::g_pass_cycles[0] + mqs::g_pass_cycles[1] + mqs::g_pass_cycles[2] + mqs::g_pass_cycles[3];
    std::printf("  pass time %% sig/ref/cln/raw %.1f / %.1f / %.1f / %.1f (passes %llu/%llu/%llu/%llu)\n",
                100.0 * mqs::g_pass_cycles[0] / pt, 100.0 * mqs::g_pass_cycles[1] / pt, 100.0 * mqs::g_pass_cycles[2] / pt,
                100.0 * mqs::g_pass_cycles[3] / pt, (unsigned long long)mqs::g_pass_count[0],
                (unsigned long long)mqs::g_pass_count[1], (unsigned long long)mqs::g_pass_count[2],
                (unsigned long long)mqs::g_pass_count[3]);
}

void timing(Env& e, const Set& s, const std::vector<const Variant*>& vs, int rounds)
{
    const std::size_t nv = vs.size();
    std::vector<std::vector<double>> ratio(nv);
    for (int r = 0; r < rounds; ++r) {
        std::vector<double> t(nv + 1);
        for (std::size_t k = 0; k <= nv; ++k) {
            const std::size_t which = (k + std::size_t(r)) % (nv + 1);
            t[which] = which == nv ? double(time_lib(e, s)) : double(time_variant(e, s, vs[which]->run));
        }
        for (std::size_t v = 0; v < nv; ++v) {
            ratio[v].push_back(t[v] / t[nv]);
        }
    }
    std::printf("[%s] against the library (median of %d interleaved rounds; min-max):", s.name.c_str(), rounds);
    for (std::size_t v = 0; v < nv; ++v) {
        auto x = ratio[v];
        std::sort(x.begin(), x.end());
        std::printf(" %s %.3f (%.3f-%.3f)", vs[v]->name, median(ratio[v]), x.front(), x.back());
    }
    std::printf("\n");
}

std::vector<std::string> split(const std::string& s)
{
    std::vector<std::string> v;
    std::size_t i = 0;
    while (i <= s.size()) {
        const std::size_t j = s.find(',', i);
        const std::string t = s.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (!t.empty()) {
            v.push_back(t);
        }
        if (j == std::string::npos) {
            break;
        }
        i = j + 1;
    }
    return v;
}

}  // namespace

int main(int argc, char** argv)
{
    std::string dir, check_dir, configs = "256_lossy,256_lossless,512_lossy,512_lossless,1024_lossy,1024_lossless", only;
    int rounds = 11;
    bool no_stats = false, no_time = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--dir" && i + 1 < argc) {
            dir = argv[++i];
        } else if (a == "--check-dir" && i + 1 < argc) {
            check_dir = argv[++i];
        } else if (a == "--configs" && i + 1 < argc) {
            configs = argv[++i];
        } else if (a == "--rounds" && i + 1 < argc) {
            rounds = std::atoi(argv[++i]);
        } else if (a == "--only" && i + 1 < argc) {
            only = argv[++i];
        } else if (a == "--no-stats") {
            no_stats = true;
        } else if (a == "--no-time") {
            no_time = true;
        }
    }
    SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << 7);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    std::vector<const Variant*> timed;
    for (const Variant& v : kVariants) {
        const bool skip = v.run == run_v8 || v.run == run_v10;
        if (skip) {
            continue;
        }
        if (only.empty()) {
            timed.push_back(&v);
        } else {
            for (const std::string& o : split(only)) {
                if (o == v.name) {
                    timed.push_back(&v);
                }
            }
        }
    }
    Env e;
    int failures = 0;
    // exactness on the extra dumps (mode switches, conformance)
    if (!check_dir.empty()) {
        std::size_t files = 0, blocks = 0, bad = 0, sty[64] = {};
        for (const auto& f : std::filesystem::directory_iterator(check_dir)) {
            Set s;
            s.name = f.path().filename().string();
            if (!load(f.path().string(), s)) {
                std::printf("cannot read %s\n", s.name.c_str());
                continue;
            }
            prepare(e, s.max_bytes);
            bad += check(e, s, kVariants, sizeof kVariants / sizeof kVariants[0]);
            for (const T1Block& b : s.t1) {
                ++sty[b.cblksty & 63];
            }
            blocks += s.t1.size();
            ++files;
        }
        std::printf("check: %zu files, %zu blocks, %zu mismatches; blocks by code-block style:", files, blocks, bad);
        for (int i = 0; i < 64; ++i) {
            if (sty[i]) {
                std::printf(" %02x:%zu", i, sty[i]);
            }
        }
        std::printf("\n");
        failures += bad != 0;
    }
    for (const std::string& c : split(configs)) {
        Set s;
        s.name = c;
        if (!load(dir + "/" + c + ".blk", s)) {
            std::printf("cannot read %s\n", c.c_str());
            return 2;
        }
        prepare(e, s.max_bytes);
        const std::size_t bad = check(e, s, kVariants, sizeof kVariants / sizeof kVariants[0]);
        std::printf("[%s] exactness: %zu blocks differ\n", c.c_str(), bad);
        failures += bad != 0;
        if (!no_stats) {
            stats(e, s, rounds);
        }
        if (!no_time) {
            timing(e, s, timed, rounds);
        }
    }
    return failures ? 1 : 0;
}

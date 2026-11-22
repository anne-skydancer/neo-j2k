// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// ebc_replay: the EBCOT-NG study's measurements on the dumps of the two earlier studies.
//   encoder: ebc_replay enc --dump F.bin [--rounds 11] [--only a,b] [--base f5]
//     the dumped code-blocks (bench/t1_study t1_profile, "NJT1") through the library's enc_t1_encode, f5 and
//     the prototypes: identity (bytes; rates, lengths, terminations, distortions bit for bit), tallies, pass
//     and stage clocks, then interleaved timing (TSC, median of per-round ratios)
//   decoder: ebc_replay dec --blk F.blk [--check-dir D] [--rounds 11] [--only a,b]
//     the dumped code-blocks (bench/mqng_study mqng_profile, "MQNG") through t1_decode_cblk, ws, wagg and the
//     prototypes: identity (coefficients and result on every block, every file of --check-dir), tallies,
//     pass clocks, then interleaved timing
// One thread, pinned to CPU 7, high priority.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>

#include "arena.h"
#include "common.h"
#include "dec_scratch.h"
#include "enc_tile.h"
#include "memory.h"
#include "../mqng_study/blockio.h"
#include "../mqng_study/proto/common.h"

using nj2k::detail::Allocator;
using nj2k::detail::Arena;
using nj2k::detail::EncCblk;
using nj2k::detail::EncPass;
using nj2k::detail::T1Block;

#define DECLARE_ENC(ns)                                                                                          \
    namespace nj2k::ns {                                                                                         \
    struct EncT1;                                                                                                \
    EncT1* enc_t1_create(const Allocator& alloc) noexcept;                                                       \
    void enc_t1_destroy(EncT1* t1, const Allocator& alloc) noexcept;                                             \
    std::int32_t* enc_t1_data(EncT1* t1) noexcept;                                                               \
    bool enc_t1_commit(const EncT1* t1, Arena& arena, EncCblk& cblk) noexcept;                                   \
    bool enc_t1_encode(EncT1* t1, EncCblk& cblk, std::uint32_t orient, std::uint32_t compno, std::uint32_t level, \
                       bool reversible, double stepsize, const double* mct_norms, std::uint32_t mct_numcomps) noexcept; \
    }
DECLARE_ENC(study_f5)
#define DEC_DECL(ns)                                                                                             \
    namespace ebc::ns {                                                                                          \
    bool run(DecScratch& s, const T1Block& b) noexcept;                                                         \
    }
#define ENC_VARIANT(ns, name, check) DECLARE_ENC(ns)
#define DEC_VARIANT(ns, name, check) DEC_DECL(ns)
#include "variants.inc"
#undef ENC_VARIANT
#undef DEC_VARIANT
// MQ-NG's variants (bench/mqng_study/proto/variant.cpp): 4 ws, 14 wagg
bool run_v4(mqs::Scratch&, const T1Block&) noexcept;
bool run_v14(mqs::Scratch&, const T1Block&) noexcept;

namespace {

enum Check { exact, bytes, none, model, modelnone };

Allocator g_alloc = Allocator::system();

double median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    return v.empty() ? 0 : v[v.size() / 2];
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

bool wanted(const std::vector<std::string>& only, const std::string& name)
{
    if (only.empty()) {
        return true;
    }
    return std::find(only.begin(), only.end(), name) != only.end();
}

void print_counts(const char* side)
{
    const ebc::Counts& k = ebc::g_counts;
    const double dec = double(k.dec[0] + k.dec[1] + k.dec[2]);
    std::printf("[%s counts] blocks %llu samples %llu bit-planes/block %.2f passes %llu/%llu/%llu\n", side,
                (unsigned long long)k.blocks, (unsigned long long)k.samples, double(k.bitplanes) / double(k.blocks),
                (unsigned long long)k.passes[0], (unsigned long long)k.passes[1], (unsigned long long)k.passes[2]);
    std::printf("  decisions %.0f (%.3f a sample): spp %.1f%% mrp %.1f%% cln %.1f%%; sign %llu/%llu/%llu; AGG %llu UNI %llu\n",
                dec, dec / double(k.samples), 100 * k.dec[0] / dec, 100 * k.dec[1] / dec, 100 * k.dec[2] / dec,
                (unsigned long long)k.sc[0], (unsigned long long)k.sc[1], (unsigned long long)k.sc[2],
                (unsigned long long)k.agg, (unsigned long long)k.uni);
    for (int p = 0; p < 3; ++p) {
        const char* pn[3] = {"spp", "mrp", "cln"};
        std::printf("  %s: columns %llu (%.1f%% with work), samples tested %llu, decisions %llu (%.3f a tested sample, "
                    "%.3f a column visited)\n",
                    pn[p], (unsigned long long)k.cols[p], 100.0 * k.cols_work[p] / double(k.cols[p]),
                    (unsigned long long)k.tested[p], (unsigned long long)k.dec[p],
                    k.tested[p] ? double(k.dec[p]) / double(k.tested[p]) : 0.0, double(k.dec[p]) / double(k.cols[p]));
    }
    std::printf("  newly significant %llu (%.3f a sample), flag RMWs %llu (%.2f a decision), context LUT loads %.2f a "
                "decision, nmsedec loads %llu, data loads %llu (%.2f a decision)\n",
                (unsigned long long)k.sig_events, double(k.sig_events) / double(k.samples), (unsigned long long)k.upd_rmw,
                k.upd_rmw / dec, k.lut_loads / dec, (unsigned long long)k.nmse_loads, (unsigned long long)k.data_loads,
                k.data_loads / dec);
    const char* sn[ebc::kSites] = {"spp column", "spp eligible", "spp bit", "mrp column", "mrp eligible", "cln run-mode",
                                   "cln AGG bit", "cln eligible", "cln bit"};
    std::printf("  branch sites (n, taken %%, last-outcome misses %%; misses a decision):\n");
    for (int s = 0; s < ebc::kSites; ++s) {
        if (!k.site_n[s]) {
            continue;
        }
        std::printf("    %-13s n %10llu taken %5.1f%% miss %5.1f%%  (%.3f a decision)\n", sn[s],
                    (unsigned long long)k.site_n[s], 100.0 * k.site_taken[s] / double(k.site_n[s]),
                    100.0 * k.site_miss[s] / double(k.site_n[s]), k.site_miss[s] / dec);
    }
    std::printf("  contexts %%:");
    for (int c = 0; c < 19; ++c) {
        std::printf(" %d:%.1f", c, 100.0 * k.dec_ctx[c] / dec);
    }
    std::printf("\n");
}

// ======================================================== encoder
struct EBlock {
    std::uint32_t w, h, orient, compno, level, reversible, mct_numcomps, mct_table;
    double stepsize;
    std::vector<std::int32_t> data;
};
struct EResult {
    bool ok;
    std::uint32_t numbps, totalpasses;
    std::vector<EncPass> passes;
    std::vector<std::uint8_t> bytes;
};
std::vector<EBlock> load_enc(const char* path)
{
    std::vector<EBlock> v;
    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        return v;
    }
    char magic[4];
    std::uint32_t n = 0;
    if (std::fread(magic, 1, 4, f) != 4 || std::memcmp(magic, "NJT1", 4) != 0 || std::fread(&n, 4, 1, f) != 1) {
        std::fclose(f);
        return v;
    }
    v.resize(n);
    for (auto& b : v) {
        std::uint32_t hdr[8];
        (void)std::fread(hdr, 4, 8, f);
        b.w = hdr[0], b.h = hdr[1], b.orient = hdr[2], b.compno = hdr[3], b.level = hdr[4], b.reversible = hdr[5];
        b.mct_numcomps = hdr[6], b.mct_table = hdr[7];
        (void)std::fread(&b.stepsize, 8, 1, f);
        b.data.resize(std::size_t(b.w) * b.h);
        (void)std::fread(b.data.data(), 4, b.data.size(), f);
    }
    std::fclose(f);
    return v;
}
const double* norms(const EBlock& b)
{
    return b.mct_table == 0 ? nullptr : b.mct_table == 1 ? nj2k::detail::kEncMctNorms : nj2k::detail::kEncMctNormsReal;
}
struct EImpl {
    std::string name;
    Check check;
    std::function<void*()> create;
    std::function<std::int32_t*(void*)> data;
    std::function<bool(void*, EncCblk&, const EBlock&)> encode;
    std::function<bool(void*, Arena&, EncCblk&)> commit;
    std::function<void(void*)> destroy;
};
#define EIMPL(ns, label, chk)                                                                                    \
    EImpl                                                                                                        \
    {                                                                                                            \
        label, chk, [] { return static_cast<void*>(nj2k::ns::enc_t1_create(g_alloc)); },                        \
            [](void* t) { return nj2k::ns::enc_t1_data(static_cast<nj2k::ns::EncT1*>(t)); },                    \
            [](void* t, EncCblk& c, const EBlock& b) {                                                           \
                return nj2k::ns::enc_t1_encode(static_cast<nj2k::ns::EncT1*>(t), c, b.orient, b.compno, b.level, \
                                               b.reversible != 0, b.stepsize, norms(b), b.mct_numcomps);         \
            },                                                                                                   \
            [](void* t, Arena& a, EncCblk& c) { return nj2k::ns::enc_t1_commit(static_cast<nj2k::ns::EncT1*>(t), a, c); }, \
            [](void* t) { nj2k::ns::enc_t1_destroy(static_cast<nj2k::ns::EncT1*>(t), g_alloc); }                 \
    }
EncCblk cblk_of(const EBlock& b)
{
    EncCblk c = {};
    c.x1 = static_cast<std::int32_t>(b.w);
    c.y1 = static_cast<std::int32_t>(b.h);
    return c;
}
std::vector<EResult> erun(const EImpl& im, const std::vector<EBlock>& blocks)
{
    std::vector<EResult> out(blocks.size());
    void* t = im.create();
    Arena arena;
    arena.init(&g_alloc);
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const EBlock& b = blocks[i];
        std::memcpy(im.data(t), b.data.data(), b.data.size() * 4);
        EncCblk c = cblk_of(b);
        EResult& r = out[i];
        r.ok = im.encode(t, c, b);
        r.numbps = c.numbps;
        r.totalpasses = r.ok ? c.totalpasses : 0;
        if (r.ok && im.commit(t, arena, c) && c.totalpasses) {
            r.passes.assign(c.passes, c.passes + c.totalpasses);
            r.bytes.assign(c.data, c.data + c.passes[c.totalpasses - 1].rate);
        }
        arena.release_all();
    }
    im.destroy(t);
    return out;
}
bool esame(const EResult& a, const EResult& b, bool distortions)
{
    if (a.ok != b.ok || a.numbps != b.numbps || a.totalpasses != b.totalpasses || a.bytes != b.bytes ||
        a.passes.size() != b.passes.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.passes.size(); ++i) {
        const EncPass& x = a.passes[i];
        const EncPass& y = b.passes[i];
        if (x.rate != y.rate || x.len != y.len || x.term != y.term) {
            return false;
        }
        if (distortions && std::memcmp(&x.distortiondec, &y.distortiondec, sizeof(double)) != 0) {
            return false;
        }
    }
    return true;
}
std::uint64_t etime(const EImpl& im, void* t, const std::vector<EBlock>& blocks)
{
    std::uint64_t sum = 0;
    for (const EBlock& b : blocks) {
        std::memcpy(im.data(t), b.data.data(), b.data.size() * 4);
        EncCblk c = cblk_of(b);
        const std::uint64_t t0 = __rdtsc();
        (void)im.encode(t, c, b);
        sum += __rdtsc() - t0;
    }
    return sum;
}

int enc_main(const char* dump, int rounds, const std::vector<std::string>& only, const std::string& basename)
{
    const auto blocks = load_enc(dump);
    if (blocks.empty()) {
        std::fprintf(stderr, "no blocks in %s\n", dump);
        return 2;
    }
    std::vector<EImpl> all;
    all.push_back(EIMPL(detail, "head", exact));
    all.push_back(EIMPL(study_f5, "f5", exact));
#define ENC_VARIANT(ns, label, chk) all.push_back(EIMPL(ns, label, chk));
#define DEC_VARIANT(ns, label, chk)
#include "variants.inc"
#undef ENC_VARIANT
#undef DEC_VARIANT
    auto find = [&](const std::string& n) -> const EImpl* {
        for (auto& im : all) {
            if (im.name == n) {
                return &im;
            }
        }
        return nullptr;
    };
    // identity
    const auto ref = erun(all[0], blocks);
    int fails = 0;
    std::string line = "identity:";
    for (std::size_t k = 1; k < all.size(); ++k) {
        if (all[k].check == none) {
            continue;
        }
        const auto res = erun(all[k], blocks);
        std::size_t bad = 0;
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            bad += esame(ref[i], res[i], all[k].check == exact) ? 0 : 1;
        }
        line += " " + all[k].name + (bad ? "=DIFFERS(" + std::to_string(bad) + ")" : "=ok");
        fails += bad != 0;
    }
    std::printf("%s (%zu blocks)\n", line.c_str(), blocks.size());
    // tallies
    {
        ebc::g_counts = {};
        (void)erun(*find("ecount"), blocks);
        print_counts("enc");
    }
    // pass and stage clocks (median of rounds): the full kernel (eclk) and the modelling alone (emodelclk)
    {
        for (const char* nm : {"eclk", "emodelclk", "esplitclk"}) {
            const EImpl* im = find(nm);
            void* t = im->create();
            std::vector<double> p[4], s[3];
            for (int r = 0; r < rounds; ++r) {
                std::memset(ebc::g_pass_cycles, 0, sizeof ebc::g_pass_cycles);
                std::memset(ebc::g_stage_cycles, 0, sizeof ebc::g_stage_cycles);
                (void)etime(*im, t, blocks);
                for (int q = 0; q < 3; ++q) {
                    p[q].push_back(double(ebc::g_pass_cycles[q]));
                    s[q].push_back(double(ebc::g_stage_cycles[q]));
                }
                p[3].push_back(double(ebc::g_pass_cycles[3]));
            }
            im->destroy(t);
            std::printf("[%s] pass Mcycles spp %.3f mrp %.3f cln %.3f (pair generation %.3f); stages Mcycles pre %.3f passes %.3f post %.3f\n", nm,
                        median(p[0]) / 1e6, median(p[1]) / 1e6, median(p[2]) / 1e6, median(p[3]) / 1e6, median(s[0]) / 1e6,
                        median(s[1]) / 1e6, median(s[2]) / 1e6);
        }
    }
    // timing
    std::vector<const EImpl*> timed;
    for (auto& im : all) {
        if (im.name == "ecount" || im.name == "eclk" || im.name == "emodelclk" || im.name == "esplitclk") {
            continue;
        }
        if (im.name == "head" || im.name == basename || wanted(only, im.name)) {
            timed.push_back(&im);
        }
    }
    const std::size_t nv = timed.size();
    std::size_t ibase = 0;
    for (std::size_t v = 0; v < nv; ++v) {
        if (timed[v]->name == basename) {
            ibase = v;
        }
    }
    std::vector<void*> ts;
    for (auto* im : timed) {
        ts.push_back(im->create());
    }
    std::vector<std::vector<double>> rh(nv), rb(nv), cyc(nv);
    for (int r = -1; r < rounds; ++r) {
        std::vector<double> t(nv);
        for (std::size_t k = 0; k < nv; ++k) {
            const std::size_t which = (k + std::size_t(r + 1)) % nv;
            t[which] = double(etime(*timed[which], ts[which], blocks));
        }
        if (r < 0) {
            continue;
        }
        for (std::size_t v = 0; v < nv; ++v) {
            rh[v].push_back(t[v] / t[0]);
            rb[v].push_back(t[v] / t[ibase]);
            cyc[v].push_back(t[v]);
        }
    }
    for (std::size_t k = 0; k < nv; ++k) {
        timed[k]->destroy(ts[k]);
    }
    std::printf("timing (%d interleaved rounds, median of per-round ratios; spread = min-max of the ratio to head):\n", rounds);
    for (std::size_t v = 0; v < nv; ++v) {
        auto x = rh[v];
        std::sort(x.begin(), x.end());
        std::printf("  %-14s %9.3f Mcyc  x head %.3f (%.3f-%.3f)  x %s %.3f\n", timed[v]->name.c_str(), median(cyc[v]) / 1e6,
                    median(rh[v]), x.front(), x.back(), basename.c_str(), median(rb[v]));
    }
    std::printf("%s\n", fails ? "MISMATCH" : "ALL EXACT VARIANTS IDENTICAL");
    return fails ? 3 : 0;
}

// ======================================================== decoder
struct DSet {
    std::string name;
    std::vector<mqs::DumpBlock> blocks;
    std::vector<nj2k::detail::Chunk> chunks;
    std::vector<std::vector<nj2k::detail::Segment>> segs;
    std::vector<T1Block> t1;
    std::size_t samples = 0, bytes = 0, max_bytes = 0;
};
bool load_dec(const std::string& path, DSet& s)
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
using DRun = bool (*)(ebc::DecScratch&, const T1Block&) noexcept;
struct DImpl {
    std::string name;
    Check check;
    DRun run;          // ours
    bool (*mrun)(mqs::Scratch&, const T1Block&) noexcept;  // MQ-NG's
    bool lib;
};
struct DEnv {
    nj2k::detail::T1 lib;
    nj2k::detail::Messages quiet;
    std::vector<std::int32_t> data;
    std::vector<std::uint32_t> flags;
    std::vector<std::uint8_t> cblk;
    ebc::DecScratch s{};
    mqs::Scratch ms{};
};
void dprepare(DEnv& e, std::size_t max_bytes)
{
    e.lib.ready(g_alloc);
    e.lib.reserve(g_alloc, max_bytes);
    e.data.assign(4096 + 16, 0);
    e.flags.assign(3078 + 64, 0);
    if (e.cblk.size() < max_bytes + 64) {
        e.cblk.assign(max_bytes + 64, 0);
    }
    e.s = ebc::DecScratch{e.data.data(), e.flags.data(), e.cblk.data(), e.cblk.size()};
    e.ms = mqs::Scratch{e.data.data(), e.flags.data(), e.cblk.data(), e.cblk.size()};
}
bool drun1(DEnv& e, const DImpl& im, const T1Block& b)
{
    if (im.lib) {
        return nj2k::detail::t1_decode_cblk(e.lib, b, e.quiet);
    }
    return im.run ? im.run(e.s, b) : im.mrun(e.ms, b);
}
const std::int32_t* dout(DEnv& e, const DImpl& im) { return im.lib ? e.lib.data : e.data.data(); }

std::vector<std::uint8_t> g_recbuf;
void record(DEnv& e, const DSet& s, const DImpl& rec)
{
    g_recbuf.assign(64u << 20, 0);
    ebc::g_rec.bits = g_recbuf.data();
    ebc::g_rec.cap = g_recbuf.size();
    ebc::g_rec.n = 0;
    for (const T1Block& b : s.t1) {
        if (b.nchunks) {
            drun1(e, rec, b);
        }
    }
    if (ebc::g_rec.n >= ebc::g_rec.cap) {
        std::printf("recording too short\n");
    }
}

std::size_t dcheck(DEnv& e, const DSet& s, const std::vector<DImpl>& all, bool modelok, std::vector<std::size_t>* per)
{
    std::size_t bad = 0;
    std::vector<std::int32_t> ref;
    ebc::g_bits = g_recbuf.data();
    std::vector<const std::uint8_t*> cursor(all.size(), g_recbuf.data());
    for (const T1Block& b : s.t1) {
        if (b.nchunks == 0) {
            continue;
        }
        const bool r = drun1(e, all[0], b);
        ref.assign(dout(e, all[0]), dout(e, all[0]) + std::size_t(b.w) * b.h);
        for (std::size_t v = 1; v < all.size(); ++v) {
            const Check c = all[v].check;
            if (c == none || c == modelnone || (c == model && !modelok)) {
                continue;
            }
            if (c == model) {
                ebc::g_bits = cursor[v];
            }
            const bool rv = drun1(e, all[v], b);
            if (c == model) {
                cursor[v] = ebc::g_bits;
            }
            if (rv != r || (r && std::memcmp(ref.data(), dout(e, all[v]), ref.size() * 4) != 0)) {
                if (bad < 5) {
                    std::printf("  MISMATCH %s: %s block %ux%u sty %x\n", s.name.c_str(), all[v].name.c_str(), b.w, b.h,
                                b.cblksty);
                }
                ++bad;
                if (per) {
                    ++(*per)[v];
                }
            }
        }
    }
    return bad;
}

std::uint64_t dtime(DEnv& e, const DSet& s, const DImpl& im)
{
    ebc::g_bits = g_recbuf.data();
    const std::uint64_t t0 = __rdtsc();
    for (const T1Block& b : s.t1) {
        if (b.nchunks) {
            drun1(e, im, b);
        }
    }
    return __rdtsc() - t0;
}

int dec_main(const std::string& blk, const std::string& check_dir, int rounds, const std::vector<std::string>& only,
             const std::string& basename)
{
    std::vector<DImpl> all;
    all.push_back(DImpl{"lib", exact, nullptr, nullptr, true});
    all.push_back(DImpl{"ws", exact, nullptr, run_v4, false});
    all.push_back(DImpl{"wagg", exact, nullptr, run_v14, false});
#define ENC_VARIANT(ns, label, chk)
#define DEC_VARIANT(ns, label, chk) all.push_back(DImpl{label, chk, ebc::ns::run, nullptr, false});
#include "variants.inc"
#undef ENC_VARIANT
#undef DEC_VARIANT
    auto find = [&](const std::string& n) -> const DImpl* {
        for (auto& im : all) {
            if (im.name == n) {
                return &im;
            }
        }
        return nullptr;
    };
    DEnv e;
    int fails = 0;
    if (!check_dir.empty()) {
        std::size_t files = 0, blocks = 0, bad = 0, sty[64] = {};
        std::vector<std::size_t> per(all.size(), 0);
        for (const auto& f : std::filesystem::directory_iterator(check_dir)) {
            DSet s;
            s.name = f.path().filename().string();
            if (!load_dec(f.path().string(), s)) {
                continue;
            }
            dprepare(e, s.max_bytes);
            bad += dcheck(e, s, all, false, &per);
            for (const T1Block& b : s.t1) {
                ++sty[b.cblksty & 63];
            }
            blocks += s.t1.size();
            ++files;
        }
        std::printf("check-dir: %zu files, %zu blocks, %zu mismatches; styles:", files, blocks, bad);
        for (int i = 0; i < 64; ++i) {
            if (sty[i]) {
                std::printf(" %02x:%zu", i, sty[i]);
            }
        }
        std::printf("\n  per variant:");
        for (std::size_t v = 1; v < all.size(); ++v) {
            if (all[v].check == exact) {
                std::printf(" %s=%zu", all[v].name.c_str(), per[v]);
            }
        }
        std::printf("\n");
        fails += bad != 0;
    }
    if (blk.empty()) {
        return fails ? 3 : 0;
    }
    DSet s;
    s.name = std::filesystem::path(blk).stem().string();
    if (!load_dec(blk, s)) {
        std::fprintf(stderr, "cannot read %s\n", blk.c_str());
        return 2;
    }
    dprepare(e, s.max_bytes);
    record(e, s, *find("drec"));
    const std::size_t nrec = ebc::g_rec.n;
    const std::size_t bad = dcheck(e, s, all, true, nullptr);
    ebc::g_rec.cap = 0;  // the recording is complete; drec writes no more
    std::printf("[%s] identity: %zu block results differ (%zu blocks, %zu samples, %zu bytes, %zu decisions recorded)\n",
                s.name.c_str(), bad, s.t1.size(), s.samples, s.bytes, nrec);
    fails += bad != 0;
    ebc::g_counts = {};
    for (const T1Block& b : s.t1) {
        if (b.nchunks) {
            drun1(e, *find("dcount"), b);
        }
    }
    print_counts("dec");
    for (const char* nm : {"dclk", "dmodelclk"}) {
        std::vector<double> p[4];
        for (int r = 0; r < rounds; ++r) {
            std::memset(ebc::g_pass_cycles, 0, sizeof ebc::g_pass_cycles);
            (void)dtime(e, s, *find(nm));
            for (int q = 0; q < 4; ++q) {
                p[q].push_back(double(ebc::g_pass_cycles[q]));
            }
        }
        std::printf("[%s] pass Mcycles spp %.3f mrp %.3f cln %.3f raw %.3f\n", nm, median(p[0]) / 1e6, median(p[1]) / 1e6,
                    median(p[2]) / 1e6, median(p[3]) / 1e6);
    }
    std::vector<const DImpl*> timed;
    for (auto& im : all) {
        if (im.name == "dcount" || im.name == "dclk" || im.name == "dmodelclk" || im.name == "drec") {
            continue;
        }
        if (im.lib || im.name == basename || wanted(only, im.name)) {
            timed.push_back(&im);
        }
    }
    const std::size_t nv = timed.size();
    std::size_t ibase = 0;
    for (std::size_t v = 0; v < nv; ++v) {
        if (timed[v]->name == basename) {
            ibase = v;
        }
    }
    std::vector<std::vector<double>> rh(nv), rb(nv), cyc(nv);
    for (int r = -1; r < rounds; ++r) {
        std::vector<double> t(nv);
        for (std::size_t k = 0; k < nv; ++k) {
            const std::size_t which = (k + std::size_t(r + 1)) % nv;
            t[which] = double(dtime(e, s, *timed[which]));
        }
        if (r < 0) {
            continue;
        }
        for (std::size_t v = 0; v < nv; ++v) {
            rh[v].push_back(t[v] / t[0]);
            rb[v].push_back(t[v] / t[ibase]);
            cyc[v].push_back(t[v]);
        }
    }
    std::printf("timing (%d interleaved rounds, median of per-round ratios):\n", rounds);
    for (std::size_t v = 0; v < nv; ++v) {
        auto x = rh[v];
        std::sort(x.begin(), x.end());
        std::printf("  %-14s %9.3f Mcyc  x lib %.3f (%.3f-%.3f)  x %s %.3f\n", timed[v]->name.c_str(), median(cyc[v]) / 1e6,
                    median(rh[v]), x.front(), x.back(), basename.c_str(), median(rb[v]));
    }
    std::printf("%s\n", fails ? "MISMATCH" : "ALL EXACT VARIANTS IDENTICAL");
    return fails ? 3 : 0;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: ebc_replay enc --dump F | dec --blk F [--check-dir D]\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string mode = argv[1];
    std::string dump, blk, check_dir, only, base;
    int rounds = 11;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--dump") {
            dump = argv[i + 1];
        } else if (a == "--blk") {
            blk = argv[i + 1];
        } else if (a == "--check-dir") {
            check_dir = argv[i + 1];
        } else if (a == "--rounds") {
            rounds = std::atoi(argv[i + 1]);
        } else if (a == "--only") {
            only = argv[i + 1];
        } else if (a == "--base") {
            base = argv[i + 1];
        }
    }
    SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << 7);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    if (mode == "enc") {
        return enc_main(dump.c_str(), rounds, split(only), base.empty() ? "f5" : base);
    }
    return dec_main(blk, check_dir, rounds, split(only), base.empty() ? "wagg" : base);
}

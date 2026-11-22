// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// t1_replay: the code-blocks of a t1_profile dump through the library's enc_t1_encode (lib_head, src/ as it
// is) and through the study's copies (proto/), every result checked against the library's (bytes, passes:
// rate, len, term, distortion bit for bit), then timed in interleaved rounds (TSC cycles, median).
//   t1_replay --dump file [--rounds 15] [--pin 7] [--only name,...]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include <intrin.h>

#include "arena.h"
#include "enc_tile.h"
#include "memory.h"
#include "proto/mq_variants.h"

using nj2k::detail::Allocator;
using nj2k::detail::Arena;
using nj2k::detail::EncCblk;
using nj2k::detail::EncPass;

#define DECLARE_T1(ns)                                                                                           \
    namespace nj2k::ns {                                                                                         \
    struct EncT1;                                                                                                \
    EncT1* enc_t1_create(const Allocator& alloc) noexcept;                                                       \
    void enc_t1_destroy(EncT1* t1, const Allocator& alloc) noexcept;                                             \
    std::int32_t* enc_t1_data(EncT1* t1) noexcept;                                                               \
    bool enc_t1_commit(const EncT1* t1, Arena& arena, EncCblk& cblk) noexcept;                                   \
    std::size_t enc_t1_size() noexcept;                                                                          \
    }
#define DECLARE_T1_ENCODE(ns)                                                                                    \
    namespace nj2k::ns {                                                                                         \
    bool enc_t1_encode(EncT1* t1, EncCblk& cblk, std::uint32_t orient, std::uint32_t compno, std::uint32_t level, \
                       bool reversible, double stepsize, const double* mct_norms, std::uint32_t mct_numcomps) noexcept; \
    }
DECLARE_T1(study_copy)
DECLARE_T1_ENCODE(study_copy)
DECLARE_T1(study_count)
DECLARE_T1_ENCODE(study_count)
DECLARE_T1(study_passtime)
DECLARE_T1_ENCODE(study_passtime)
DECLARE_T1(study_split)
DECLARE_T1(study_f0)
DECLARE_T1_ENCODE(study_f0)
DECLARE_T1(study_f1)
DECLARE_T1_ENCODE(study_f1)
DECLARE_T1(study_f2)
DECLARE_T1_ENCODE(study_f2)
DECLARE_T1(study_f3)
DECLARE_T1_ENCODE(study_f3)
DECLARE_T1(study_f4)
DECLARE_T1_ENCODE(study_f4)
DECLARE_T1(study_f5)
DECLARE_T1_ENCODE(study_f5)
DECLARE_T1(study_f6)
DECLARE_T1_ENCODE(study_f6)
DECLARE_T1(study_f7)
DECLARE_T1_ENCODE(study_f7)
DECLARE_T1(study_f8)
DECLARE_T1_ENCODE(study_f8)
DECLARE_T1(study_f9)
DECLARE_T1_ENCODE(study_f9)
DECLARE_T1(study_f10)
DECLARE_T1_ENCODE(study_f10)
DECLARE_T1(study_f11)
DECLARE_T1_ENCODE(study_f11)
DECLARE_T1(study_f4t)
DECLARE_T1_ENCODE(study_f4t)
namespace nj2k::study_f4t {
inline std::uint64_t g_pass_cycles[3];
}
namespace nj2k::study_split {
bool enc_t1_encode(EncT1* t1, EncCblk& cblk, std::uint32_t orient, std::uint32_t compno, std::uint32_t level,
                   bool reversible, double stepsize, const double* mct_norms, std::uint32_t mct_numcomps, int phases,
                   MqFn coder) noexcept;
const std::uint8_t* enc_t1_syms(const EncT1* t1, const std::uint32_t** pass_ends) noexcept;
}  // namespace nj2k::study_split

#include "proto/proto_stats_decl.h"

namespace {

struct Block {
    std::uint32_t w, h, orient, compno, level, reversible, mct_numcomps, mct_table;
    double stepsize;
    std::vector<std::int32_t> data;
};

struct Result {
    bool ok;
    std::uint32_t numbps, totalpasses;
    std::vector<EncPass> passes;
    std::vector<std::uint8_t> bytes;
};

std::vector<Block> load(const char* path)
{
    std::vector<Block> v;
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
        b.w = hdr[0];
        b.h = hdr[1];
        b.orient = hdr[2];
        b.compno = hdr[3];
        b.level = hdr[4];
        b.reversible = hdr[5];
        b.mct_numcomps = hdr[6];
        b.mct_table = hdr[7];
        (void)std::fread(&b.stepsize, 8, 1, f);
        b.data.resize(std::size_t(b.w) * b.h);
        (void)std::fread(b.data.data(), 4, b.data.size(), f);
    }
    std::fclose(f);
    return v;
}

const double* norms(const Block& b)
{
    return b.mct_table == 0 ? nullptr : b.mct_table == 1 ? nj2k::detail::kEncMctNorms : nj2k::detail::kEncMctNormsReal;
}

EncCblk cblk_of(const Block& b)
{
    EncCblk c = {};
    c.x0 = 0;
    c.y0 = 0;
    c.x1 = static_cast<std::int32_t>(b.w);
    c.y1 = static_cast<std::int32_t>(b.h);
    return c;
}

// one implementation of Tier-1: create, data pointer, encode one block, commit, destroy
struct Impl {
    std::string name;
    std::function<void*()> create;
    std::function<std::int32_t*(void*)> data;
    std::function<bool(void*, EncCblk&, const Block&)> encode;
    std::function<bool(void*, Arena&, EncCblk&)> commit;
    std::function<void(void*)> destroy;
    std::size_t scratch = 0;
};

Allocator g_alloc = Allocator::system();

#define IMPL_OF(ns, label)                                                                                       \
    Impl                                                                                                         \
    {                                                                                                            \
        label, [] { return static_cast<void*>(nj2k::ns::enc_t1_create(g_alloc)); },                             \
            [](void* t) { return nj2k::ns::enc_t1_data(static_cast<nj2k::ns::EncT1*>(t)); },                    \
            [](void* t, EncCblk& c, const Block& b) {                                                            \
                return nj2k::ns::enc_t1_encode(static_cast<nj2k::ns::EncT1*>(t), c, b.orient, b.compno, b.level, \
                                               b.reversible != 0, b.stepsize, norms(b), b.mct_numcomps);         \
            },                                                                                                   \
            [](void* t, Arena& a, EncCblk& c) {                                                                  \
                return nj2k::ns::enc_t1_commit(static_cast<nj2k::ns::EncT1*>(t), a, c);                          \
            },                                                                                                   \
            [](void* t) { nj2k::ns::enc_t1_destroy(static_cast<nj2k::ns::EncT1*>(t), g_alloc); }                 \
    }

Impl split_impl(int variant)
{
    Impl im;
    im.name = std::string("split ") + nj2k::kMqVariants[variant].name;
    im.create = [] { return static_cast<void*>(nj2k::study_split::enc_t1_create(g_alloc)); };
    im.data = [](void* t) { return nj2k::study_split::enc_t1_data(static_cast<nj2k::study_split::EncT1*>(t)); };
    const nj2k::MqFn fn = nj2k::kMqVariants[variant].fn;
    im.encode = [fn](void* t, EncCblk& c, const Block& b) {
        return nj2k::study_split::enc_t1_encode(static_cast<nj2k::study_split::EncT1*>(t), c, b.orient, b.compno,
                                                b.level, b.reversible != 0, b.stepsize, norms(b), b.mct_numcomps, 3,
                                                fn);
    };
    im.commit = [](void* t, Arena& a, EncCblk& c) {
        return nj2k::study_split::enc_t1_commit(static_cast<nj2k::study_split::EncT1*>(t), a, c);
    };
    im.destroy = [](void* t) { nj2k::study_split::enc_t1_destroy(static_cast<nj2k::study_split::EncT1*>(t), g_alloc); };
    im.scratch = nj2k::study_split::enc_t1_size();
    return im;
}

std::vector<Result> run(const Impl& im, const std::vector<Block>& blocks)
{
    std::vector<Result> out(blocks.size());
    void* t = im.create();
    Arena arena;
    arena.init(&g_alloc);
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const Block& b = blocks[i];
        std::memcpy(im.data(t), b.data.data(), b.data.size() * 4);
        EncCblk c = cblk_of(b);
        Result& r = out[i];
        r.ok = im.encode(t, c, b);
        r.numbps = c.numbps;
        r.totalpasses = r.ok ? c.totalpasses : 0;
        if (r.ok && im.commit(t, arena, c) && c.totalpasses) {
            r.passes.assign(c.passes, c.passes + c.totalpasses);
            const std::uint32_t nbytes = c.passes[c.totalpasses - 1].rate;
            (void)nbytes;
            // the committed bytes: numbytes() of the coder, which the last pass's rate may undercut by the
            // 0xFF rule; compare everything the arena got
            r.bytes.assign(c.data, c.data + (c.passes[c.totalpasses - 1].rate));
        }
        arena.release_all();
    }
    im.destroy(t);
    return out;
}

bool same(const Result& a, const Result& b)
{
    if (a.ok != b.ok || a.numbps != b.numbps || a.totalpasses != b.totalpasses || a.bytes != b.bytes ||
        a.passes.size() != b.passes.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.passes.size(); ++i) {
        const EncPass& x = a.passes[i];
        const EncPass& y = b.passes[i];
        if (x.rate != y.rate || x.len != y.len || x.term != y.term ||
            std::memcmp(&x.distortiondec, &y.distortiondec, sizeof(double)) != 0) {
            return false;
        }
    }
    return true;
}

std::uint64_t time_once(const Impl& im, void* t, const std::vector<Block>& blocks)
{
    std::uint64_t sum = 0;
    for (const Block& b : blocks) {
        std::memcpy(im.data(t), b.data.data(), b.data.size() * 4);
        EncCblk c = cblk_of(b);
        const std::uint64_t t0 = __rdtsc();
        (void)im.encode(t, c, b);
        sum += __rdtsc() - t0;
    }
    return sum;
}

std::uint64_t median(std::vector<std::uint64_t> v)
{
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

}  // namespace

int main(int argc, char** argv)
{
    const char* dump = nullptr;
    int rounds = 15, pin = -1;
    std::string only;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--dump") {
            dump = argv[i + 1];
        } else if (a == "--rounds") {
            rounds = std::atoi(argv[i + 1]);
        } else if (a == "--pin") {
            pin = std::atoi(argv[i + 1]);
        } else if (a == "--only") {
            only = argv[i + 1];
        }
    }
#if defined(_WIN32)
    if (pin >= 0) {
        SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << pin);
    }
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#endif
    const auto blocks = load(dump ? dump : "");
    if (blocks.empty()) {
        std::fprintf(stderr, "no blocks in %s\n", dump ? dump : "(none)");
        return 1;
    }
    std::uint64_t samples = 0;
    for (const auto& b : blocks) {
        samples += b.data.size();
    }

    // the implementations
    std::vector<Impl> impls;
    {
        Impl head = IMPL_OF(detail, "head (library)");
        head.scratch = 0;
        impls.push_back(head);
        Impl copy = IMPL_OF(study_copy, "copy (same source)");
        copy.scratch = nj2k::study_copy::enc_t1_size();
        impls.push_back(copy);
        for (int v = 0; v < nj2k::kMqVariantCount; ++v) {
            impls.push_back(split_impl(v));
        }
        {
            Impl f = IMPL_OF(study_f0, "f0 control: members, pointers, bit loop, branches");
            f.scratch = nj2k::study_f0::enc_t1_size();
            impls.push_back(f);
        }
        {
            Impl f = IMPL_OF(study_f1, "f1 locals");
            f.scratch = nj2k::study_f1::enc_t1_size();
            impls.push_back(f);
        }
        {
            Impl f = IMPL_OF(study_f2, "f2 locals+clz");
            f.scratch = nj2k::study_f2::enc_t1_size();
            impls.push_back(f);
        }
        {
            Impl f = IMPL_OF(study_f3, "f3 locals+clz+idx");
            f.scratch = nj2k::study_f3::enc_t1_size();
            impls.push_back(f);
        }
        {
            Impl f = IMPL_OF(study_f4, "f4 locals+clz+idx+select");
            f.scratch = nj2k::study_f4::enc_t1_size();
            impls.push_back(f);
        }
        {
            Impl f = IMPL_OF(study_f5, "f5 clz+idx+select (members)");
            f.scratch = nj2k::study_f5::enc_t1_size();
            impls.push_back(f);
        }
        {
            Impl f = IMPL_OF(study_f6, "f6 locals+idx+select (bit loop)");
            f.scratch = nj2k::study_f6::enc_t1_size();
            impls.push_back(f);
        }
        {
            Impl f = IMPL_OF(study_f7, "f7 select only (pointers)");
            f.scratch = nj2k::study_f7::enc_t1_size();
            impls.push_back(f);
        }
        {
            Impl f = IMPL_OF(study_f8, "f8 idx+select (members, bit loop)");
            f.scratch = nj2k::study_f8::enc_t1_size();
            impls.push_back(f);
        }
        {
            Impl f = IMPL_OF(study_f9, "f9 clz (members)");
            f.scratch = nj2k::study_f9::enc_t1_size();
            impls.push_back(f);
        }
        {
            Impl f = IMPL_OF(study_f10, "f10 idx (members)");
            f.scratch = nj2k::study_f10::enc_t1_size();
            impls.push_back(f);
        }
        {
            Impl f = IMPL_OF(study_f11, "f11 clz+idx (members)");
            f.scratch = nj2k::study_f11::enc_t1_size();
            impls.push_back(f);
        }
    }
    if (!only.empty()) {
        std::vector<Impl> keep;
        for (auto& im : impls) {
            if (im.name.rfind("head", 0) == 0 || only.find(im.name.substr(0, im.name.find(' '))) != std::string::npos) {
                keep.push_back(im);
            }
        }
        impls = keep;
    }

    // identity: every implementation against the library
    const auto ref = run(impls[0], blocks);
    bool all_same = true;
    for (std::size_t k = 1; k < impls.size(); ++k) {
        const auto res = run(impls[k], blocks);
        std::size_t bad = 0;
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            bad += same(ref[i], res[i]) ? 0 : 1;
        }
        std::printf("identity %-26s %s (%zu of %zu blocks differ)\n", impls[k].name.c_str(), bad ? "DIFFERS" : "identical",
                    bad, blocks.size());
        all_same = all_same && bad == 0;
    }

    // statistics: the counting copy
    {
        Impl cnt = IMPL_OF(study_count, "count");
        nj2k::study_count::g_stats = {};
        (void)run(cnt, blocks);
        const auto& s = nj2k::study_count::g_stats;
        const std::uint64_t dec = s.decisions[0] + s.decisions[1] + s.decisions[2];
        std::printf("blocks %llu samples %llu bitplanes/block %.2f passes sig/ref/cln %llu/%llu/%llu\n",
                    (unsigned long long)s.blocks, (unsigned long long)s.samples, double(s.bitplanes) / double(s.blocks),
                    (unsigned long long)s.passes[0], (unsigned long long)s.passes[1], (unsigned long long)s.passes[2]);
        std::printf("decisions %llu (%.3f a sample; sig %.1f%% ref %.1f%% cln %.1f%%)\n", (unsigned long long)dec,
                    double(dec) / double(samples), 100.0 * s.decisions[0] / dec, 100.0 * s.decisions[1] / dec,
                    100.0 * s.decisions[2] / dec);
        std::printf("MPS %.1f%% (MPS with renorm %.1f%% of decisions) LPS %.1f%%; renorms %.1f%% of decisions, "
                    "%.2f shifts a renorm; byteouts %.3f a decision (%llu bytes)\n",
                    100.0 * s.mps / dec, 100.0 * s.mps_renorm / dec, 100.0 * s.lps / dec, 100.0 * s.renorms / dec,
                    double(s.renorm_shifts) / double(s.renorms), double(s.byteouts) / double(dec),
                    (unsigned long long)s.byteouts);
        std::printf("context share:");
        for (int c = 0; c < 19; ++c) {
            std::printf(" %d:%.1f", c, 100.0 * s.ctx_hist[c] / dec);
        }
        std::printf("\n");
        std::printf("STAT decisions=%llu samples=%llu renorms=%llu shifts=%llu byteouts=%llu lps=%llu mpsr=%llu\n",
                    (unsigned long long)dec, (unsigned long long)samples, (unsigned long long)s.renorms,
                    (unsigned long long)s.renorm_shifts, (unsigned long long)s.byteouts, (unsigned long long)s.lps,
                    (unsigned long long)s.mps_renorm);
    }

    // the (context, bit) traces: the MQ variants alone, and the pass-end invariant with a short buffer
    std::vector<std::vector<std::uint8_t>> traces(blocks.size());
    std::vector<std::vector<std::uint32_t>> ends(blocks.size());
    std::uint64_t max_trace = 0;
    {
        Impl sp = split_impl(0);
        void* t = sp.create();
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            const Block& b = blocks[i];
            std::memcpy(sp.data(t), b.data.data(), b.data.size() * 4);
            EncCblk c = cblk_of(b);
            (void)nj2k::study_split::enc_t1_encode(static_cast<nj2k::study_split::EncT1*>(t), c, b.orient, b.compno,
                                                   b.level, b.reversible != 0, b.stepsize, norms(b), b.mct_numcomps, 1,
                                                   nullptr);
            const std::uint32_t* pe = nullptr;
            const std::uint8_t* sy = nj2k::study_split::enc_t1_syms(static_cast<nj2k::study_split::EncT1*>(t), &pe);
            const std::uint32_t np = c.totalpasses;
            ends[i].assign(pe, pe + np);
            traces[i].assign(sy, sy + (np ? pe[np - 1] : 0));
            max_trace = std::max<std::uint64_t>(max_trace, traces[i].size());
        }
        sp.destroy(t);
    }
    std::printf("trace bytes: max %llu a block (scratch of the split copy %zu bytes; library copy %zu)\n",
                (unsigned long long)max_trace, nj2k::study_split::enc_t1_size(), nj2k::study_copy::enc_t1_size());
    const std::uint32_t cap_full = 74 + 3U * (3U * 25U - 2U) + 4096U * 4U;
    std::vector<std::uint8_t> outbuf(1 + cap_full), refbuf(1 + cap_full);
    {
        std::size_t bad = 0, overflow_cases = 0;
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            const std::uint32_t np = static_cast<std::uint32_t>(ends[i].size());
            if (!np) {
                continue;
            }
            nj2k::MqResult r0;
            refbuf[0] = 0;
            nj2k::kMqVariants[0].fn(traces[i].data(), ends[i].data(), np, refbuf.data() + 1, cap_full, r0);
            // short buffers: the clamp at the output limit (OD-4) must happen at the same byte
            const std::uint32_t caps[3] = {cap_full, r0.nbytes / 2 + 2, 3};
            for (int v = 1; v < nj2k::kMqVariantCount; ++v) {
                for (std::uint32_t cap : caps) {
                    nj2k::MqResult ra, rb;
                    refbuf[0] = 0;
                    outbuf[0] = 0;
                    std::memset(refbuf.data() + 1, 0xAA, cap_full);
                    std::memset(outbuf.data() + 1, 0xAA, cap_full);
                    nj2k::kMqVariants[0].fn(traces[i].data(), ends[i].data(), np, refbuf.data() + 1, cap, ra);
                    nj2k::kMqVariants[v].fn(traces[i].data(), ends[i].data(), np, outbuf.data() + 1, cap, rb);
                    overflow_cases += (ra.overflow && v == 1) ? 1 : 0;
                    bool ok = ra.overflow == rb.overflow && ra.nbytes == rb.nbytes &&
                              std::memcmp(ra.rates, rb.rates, np * 4) == 0 &&
                              std::memcmp(refbuf.data(), outbuf.data(), 1 + cap_full) == 0;
                    bad += ok ? 0 : 1;
                }
            }
        }
        std::printf("MQ variants vs M0 on every trace, full and short buffers (pass-end rates, bytes, overflow): "
                    "%s (%zu mismatches; %zu short-buffer overflows exercised)\n",
                    bad ? "DIFFER" : "identical", bad, overflow_cases);
        all_same = all_same && bad == 0;
    }

    // timing: interleaved rounds, the order rotating
    std::vector<std::vector<std::uint64_t>> times(impls.size());
    std::vector<std::vector<std::uint64_t>> mq_times(nj2k::kMqVariantCount);
    std::vector<std::uint64_t> model_times, passtime[3], passtime4[3];
    Impl f4t = IMPL_OF(study_f4t, "f4t");
    void* f4tt = f4t.create();
    {
        std::vector<void*> ts;
        for (auto& im : impls) {
            ts.push_back(im.create());
        }
        Impl pt = IMPL_OF(study_passtime, "passtime");
        void* ptt = pt.create();
        Impl sp = split_impl(0);
        void* spt = sp.create();
        for (int r = -1; r < rounds; ++r) {
            for (std::size_t k0 = 0; k0 < impls.size(); ++k0) {
                const std::size_t k = (k0 + static_cast<std::size_t>(r + 1)) % impls.size();
                const std::uint64_t c = time_once(impls[k], ts[k], blocks);
                if (r >= 0) {
                    times[k].push_back(c);
                }
            }
            for (int v0 = 0; v0 < nj2k::kMqVariantCount; ++v0) {
                const int v = (v0 + r + 1) % nj2k::kMqVariantCount;
                std::uint64_t sum = 0;
                for (std::size_t i = 0; i < blocks.size(); ++i) {
                    const std::uint32_t np = static_cast<std::uint32_t>(ends[i].size());
                    if (!np) {
                        continue;
                    }
                    nj2k::MqResult rr;
                    outbuf[0] = 0;
                    const std::uint64_t t0 = __rdtsc();
                    nj2k::kMqVariants[v].fn(traces[i].data(), ends[i].data(), np, outbuf.data() + 1, cap_full, rr);
                    sum += __rdtsc() - t0;
                }
                if (r >= 0) {
                    mq_times[v].push_back(sum);
                }
            }
            {
                // the modelling half alone (the split copy's passes writing the trace)
                std::uint64_t sum = 0;
                for (const Block& b : blocks) {
                    std::memcpy(sp.data(spt), b.data.data(), b.data.size() * 4);
                    EncCblk c = cblk_of(b);
                    const std::uint64_t t0 = __rdtsc();
                    (void)nj2k::study_split::enc_t1_encode(static_cast<nj2k::study_split::EncT1*>(spt), c, b.orient,
                                                           b.compno, b.level, b.reversible != 0, b.stepsize, norms(b),
                                                           b.mct_numcomps, 1, nullptr);
                    sum += __rdtsc() - t0;
                }
                if (r >= 0) {
                    model_times.push_back(sum);
                }
                nj2k::study_passtime::g_stats = {};
                (void)time_once(pt, ptt, blocks);
                if (r >= 0) {
                    for (int p = 0; p < 3; ++p) {
                        passtime[p].push_back(nj2k::study_passtime::g_stats.pass_cycles[p]);
                    }
                }
                for (auto& x : nj2k::study_f4t::g_pass_cycles) {
                    x = 0;
                }
                (void)time_once(f4t, f4tt, blocks);
                if (r >= 0) {
                    for (int p = 0; p < 3; ++p) {
                        passtime4[p].push_back(nj2k::study_f4t::g_pass_cycles[p]);
                    }
                }
            }
        }
        for (std::size_t k = 0; k < impls.size(); ++k) {
            impls[k].destroy(ts[k]);
        }
        pt.destroy(ptt);
        sp.destroy(spt);
    }
    const double head = double(median(times[0]));
    std::printf("\nkernel (enc_t1_encode over %zu blocks, %d rounds, median; min..max spread)\n", blocks.size(), rounds);
    for (std::size_t k = 0; k < impls.size(); ++k) {
        auto v = times[k];
        std::sort(v.begin(), v.end());
        std::printf("  %-28s %10.3f Mcyc  %.4f x head  spread %.1f%%  scratch %zu B\n", impls[k].name.c_str(),
                    double(median(times[k])) / 1e6, double(median(times[k])) / head,
                    100.0 * double(v.back() - v.front()) / double(v[v.size() / 2]), impls[k].scratch);
    }
    std::printf("  %-28s %10.3f Mcyc  %.4f x head\n", "model half alone (split)", double(median(model_times)) / 1e6,
                double(median(model_times)) / head);
    const double m0 = double(median(mq_times[0]));
    for (int v = 0; v < nj2k::kMqVariantCount; ++v) {
        std::printf("  MQ alone %-19s %10.3f Mcyc  %.4f x head  %.4f x M0\n", nj2k::kMqVariants[v].name,
                    double(median(mq_times[v])) / 1e6, double(median(mq_times[v])) / head,
                    double(median(mq_times[v])) / m0);
    }
    const double pts = double(median(passtime[0]) + median(passtime[1]) + median(passtime[2]));
    std::printf("  pass shares (timed copy): sig %.1f%% ref %.1f%% cln %.1f%%\n", 100.0 * median(passtime[0]) / pts,
                100.0 * median(passtime[1]) / pts, 100.0 * median(passtime[2]) / pts);
    f4t.destroy(f4tt);
    std::printf("  pass Mcyc, f4: sig %.3f ref %.3f cln %.3f; today's (timed copy): sig %.3f ref %.3f cln %.3f\n",
                double(median(passtime4[0])) / 1e6, double(median(passtime4[1])) / 1e6,
                double(median(passtime4[2])) / 1e6, double(median(passtime[0])) / 1e6,
                double(median(passtime[1])) / 1e6, double(median(passtime[2])) / 1e6);
    std::printf("\n%s\n", all_same ? "ALL IDENTICAL" : "MISMATCH");
    return all_same ? 0 : 3;
}

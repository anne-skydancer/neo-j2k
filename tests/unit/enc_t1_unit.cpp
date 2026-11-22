// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The encoder's MQ coder and Tier-1 against the frozen reference (the text of src/enc_t1.cpp as it was
// before the MQ redesign: enc_mq_frozen.h, enc_t1_frozen.cpp). The redesign must leave every byte, every
// pass end and the overflow flag as they were, so each test runs both coders on the same input:
//   1. streams: random decision streams over the 19 contexts, pass ends every few hundred decisions;
//      after every decision the coders' a, c, ct, bytes written (their count), overflow flag and context
//      state agree; at every pass end numbytes() and the overflow flag agree; after the flush all the
//      bytes of the buffer agree; at full capacity and at
//      short ones (the output clamp at every byte position of a short buffer, and around the length of
//      the whole stream);
//   2. single decisions: one decision from a chosen state over every interval a in [0x8000, 0xffff],
//      every state (94), both bits, every bit counter ct in [1, 12], the low word c, the previous byte
//      (0x00, 0xFE, 0xFF: plain, carry into 0xFF, stuffing) and the output limit at the previous byte,
//      1, 2 or 3 bytes after it (inside a renormalisation with several byte-outs) or out of reach. The
//      default run takes one pseudo-random combination of c, previous byte and limit for each (a, state,
//      bit, ct); --exhaustive-full takes all 60 (4.4 billion decisions, a few minutes);
//   3. enc_t1_encode on generated code-blocks: every size 1..64 a side, heights that are no multiple of
//      four, every orientation, 5/3 and 9/7, samples from nothing (numbps 0) to the full range: the
//      passes (rate, len, term, distortion bit for bit), numbps and the committed bytes. The output
//      clamp is not expected to be reachable through it (a code-block's buffer holds four bytes a
//      sample and the coder writes about one and a half at most): the run counts the blocks refused
//      for overflow and asserts none; the clamp is tested in 1 and 2.
// Usage: nj2k_enc_t1_unit [--exhaustive-full] [--no-exhaustive]
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <vector>

#include "common.h"
#include "enc_t1_frozen.h"
#include "enc_mq_frozen.h"

// the library's internals (by their path: src/ is no include directory)
#include "../../src/arena.h"
#include "../../src/enc_mq.h"
#include "../../src/enc_tile.h"
#include "../../src/t1_flags.h"
#include "../../src/t1_tables.h"

namespace {

namespace d = nj2k::detail;
namespace fz = nj2k::frozen;
using u8 = std::uint8_t;
using u32 = std::uint32_t;

int g_failures = 0;
#define CHECK_MSG(cond, ...)                                          \
    do {                                                              \
        if (!(cond)) {                                                \
            ++g_failures;                                             \
            std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            std::fprintf(stderr, __VA_ARGS__);                        \
            std::fprintf(stderr, "\n");                               \
        }                                                             \
    } while (0)

struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed) {}
    std::uint64_t next() noexcept
    {
        s += 0x9e3779b97f4a7c15ull;
        std::uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    u32 u(u32 n) noexcept { return static_cast<u32>((next() >> 11) % n); }  // n > 0
};

// The state index (state * 2 + mps) of a context, whatever the coder keeps it as (a pointer into the
// 94 states, or the index itself).
template <class Mq>
u32 state_of(const Mq& m, u32 ctx) noexcept
{
    if constexpr (std::is_pointer_v<std::remove_cvref_t<decltype(m.ctxs[0])>>) {
        return static_cast<u32>(m.ctxs[ctx] - d::kMqcStates);
    } else {
        return static_cast<u32>(m.ctxs[ctx]);
    }
}

// ---- 1. streams ----

template <class Mq>
struct Run {
    Mq mq{};
    std::vector<u8> buf;
    explicit Run(u32 cap) : buf(cap + 2, 0)
    {
        mq.resetstates();
        mq.setstate(T1_CTXNO_UNI, 0, 46);
        mq.setstate(T1_CTXNO_AGG, 0, 3);
        mq.setstate(T1_CTXNO_ZC, 0, 4);
        mq.init_enc(buf.data() + 1, buf.data() + cap);
    }
};

template <class A, class B>
bool same_coder(const A& x, const B& y) noexcept
{
    if (x.a != y.a || x.c != y.c || x.ct != y.ct || x.overflow != y.overflow || x.numbytes() != y.numbytes()) {
        return false;
    }
    for (u32 i = 0; i < 19; ++i) {
        if (state_of(x, i) != state_of(y, i)) {
            return false;
        }
    }
    return true;
}

// Codes `n` decisions of stream `seed` in both coders (pass ends every `pass_len` decisions); false at the
// first difference. total: the stream's length in bytes after the flush (of the first coder).
bool run_stream(u32 seed, u32 n, u32 cap, u32* total)
{
    Rng rng(seed * 7919u + 13u);
    // the per-context probability of a 1: skewed, uniform or alternating, by stream
    const u32 kind = seed % 4;
    u32 thr[19];
    for (u32 i = 0; i < 19; ++i) {
        thr[i] = kind == 0 ? 32768u : kind == 1 ? 200u + rng.u(65000u) : kind == 2 ? (rng.u(2) ? 65000u : 500u) : rng.u(65536u);
    }
    Run<fz::MqEnc> ref(cap);
    Run<d::MqEnc> neo(cap);
    u32 until_pass_end = 1 + rng.u(300);
    u32 prev_ctx = 0;
    bool ok = true;
    for (u32 k = 0; k < n && ok; ++k) {
        // contexts in runs, as the passes use them
        const u32 ctx = rng.u(4) ? prev_ctx : rng.u(19);
        prev_ctx = ctx;
        const u32 bit = rng.u(65536) < thr[ctx] ? 1u : 0u;
        ref.mq.setcurctx(ctx);
        neo.mq.setcurctx(ctx);
        ref.mq.encode(bit);
        neo.mq.encode(bit);
        if (!same_coder(ref.mq, neo.mq)) {
            std::fprintf(stderr, "stream %u cap %u: coders differ after decision %u (ctx %u bit %u)\n", seed, cap, k, ctx, bit);
            ok = false;
        }
        if (--until_pass_end == 0) {
            until_pass_end = 1 + rng.u(300);
            if (ref.mq.numbytes() != neo.mq.numbytes() || ref.mq.overflow != neo.mq.overflow) {
                std::fprintf(stderr, "stream %u cap %u: pass end at decision %u: %u / %u bytes\n", seed, cap, k,
                             ref.mq.numbytes(), neo.mq.numbytes());
                ok = false;
            }
        }
    }
    ref.mq.flush();
    neo.mq.flush();
    if (ok && (ref.mq.numbytes() != neo.mq.numbytes() || ref.mq.overflow != neo.mq.overflow ||
               std::memcmp(ref.buf.data(), neo.buf.data(), ref.buf.size()) != 0)) {
        std::fprintf(stderr, "stream %u cap %u: after the flush: %u / %u bytes, overflow %d / %d\n", seed, cap,
                     ref.mq.numbytes(), neo.mq.numbytes(), ref.mq.overflow, neo.mq.overflow);
        ok = false;
    }
    if (total) {
        *total = ref.mq.numbytes();
    }
    return ok;
}

void test_streams()
{
    u32 runs = 0;
    for (u32 seed = 0; seed < 240; ++seed) {
        const u32 n = 200 + (seed * 37u) % 4000u;
        u32 len = 0;
        if (!run_stream(seed, n, 1u << 16, &len)) {
            ++g_failures;
            return;
        }
        ++runs;
        // short capacities: every byte position up to 40, around the length, and a spread
        u32 caps[80];
        u32 nc = 0;
        for (u32 c = 1; c <= 40; ++c) {
            caps[nc++] = c;
        }
        for (u32 c = len > 4 ? len - 4 : 1; c <= len + 4; ++c) {
            caps[nc++] = c;
        }
        caps[nc++] = len / 2 + 1;
        caps[nc++] = len / 3 + 1;
        caps[nc++] = len / 5 + 1;
        for (u32 i = 0; i < nc; ++i) {
            if (!run_stream(seed, n, caps[i], nullptr)) {
                ++g_failures;
                return;
            }
            ++runs;
        }
    }
    std::printf("streams: %u runs identical\n", runs);
}

// ---- 2. single decisions ----

struct One {
    bool ok = true;
    std::uint64_t n = 0;
};

// One decision of both coders from the given state; false when they differ.
bool one_decision(u32 a, u32 state, u32 bit, u32 ct, u32 c, u8 prev, u32 limit_off)
{
    u8 rb[8], nb[8];
    std::memset(rb, prev, sizeof rb);
    std::memset(nb, prev, sizeof nb);
    fz::MqEnc ref{};
    d::MqEnc neo{};
    ref.resetstates();
    neo.resetstates();
    ref.setstate(0, state & 1u, static_cast<std::int32_t>(state >> 1));
    neo.setstate(0, state & 1u, static_cast<std::int32_t>(state >> 1));
    ref.setcurctx(0);
    neo.setcurctx(0);
    ref.a = neo.a = a;
    ref.c = neo.c = c;
    ref.ct = neo.ct = ct;
    ref.bp = rb + 2;
    neo.bp = nb + 2;
    ref.start = rb + 3;
    neo.start = nb + 3;
    ref.limit = rb + 2 + limit_off;
    neo.limit = nb + 2 + limit_off;
    ref.overflow = neo.overflow = false;
    ref.encode(bit);
    neo.encode(bit);
    return same_coder(ref, neo) && ref.bp - rb == neo.bp - nb && std::memcmp(rb, nb, sizeof rb) == 0;
}

// the low words: 0, under the carry bit (27), at it, and mixed; one is taken at a time. A combination is
// (c, previous byte, limit offset) = (combo % 4, (combo / 4) % 3, combo / 12): 4 x 3 x 5 (limit at the
// previous byte, 1, 2, 3 bytes after it, and out of reach: offset 4 writes at most to rb[6])
constexpr u32 kC[4] = {0u, 0x7fffff0u, 0x8000000u, 0x35a5a5au};
constexpr u8 kPrev[3] = {0x00, 0xfe, 0xff};

bool test_single(bool full)
{
    std::uint64_t n = 0;
    for (u32 a = 0x8000; a <= 0xffff; ++a) {
        for (u32 state = 0; state < 94; ++state) {
            for (u32 bit = 0; bit < 2; ++bit) {
                // a pseudo-random combination (c, previous byte, limit) for the default run
                const u32 pick = (a * 2654435761u + state * 40503u + bit * 977u) % 60u;
                for (u32 ct = 1; ct <= 12; ++ct) {
                    if (full) {
                        for (u32 combo = 0; combo < 60; ++combo) {
                            if (!one_decision(a, state, bit, ct, kC[combo & 3], kPrev[(combo >> 2) % 3], combo / 12)) {
                                std::fprintf(stderr, "single decision differs: a=%04x state=%u bit=%u ct=%u combo=%u\n", a, state,
                                             bit, ct, combo);
                                ++g_failures;
                                return false;
                            }
                            ++n;
                        }
                    } else {
                        const u32 combo = (pick + ct * 5u) % 60u;
                        if (!one_decision(a, state, bit, ct, kC[combo & 3], kPrev[(combo >> 2) % 3], combo / 12)) {
                            std::fprintf(stderr, "single decision differs: a=%04x state=%u bit=%u ct=%u combo=%u\n", a, state, bit,
                                         ct, combo);
                            ++g_failures;
                            return false;
                        }
                        ++n;
                    }
                }
            }
        }
    }
    std::printf("single decisions: %llu identical (%s)\n", static_cast<unsigned long long>(n), full ? "all combinations" : "sampled combinations");
    return true;
}

// ---- 3. enc_t1_encode ----

void fill_block(std::int32_t* data, u32 w, u32 h, Rng& rng)
{
    const u32 kind = rng.u(7);
    const u32 bits = rng.u(31);  // magnitudes up to 2^bits
    const std::int32_t mag = bits ? static_cast<std::int32_t>((1u << bits) - 1u) : 0;
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            std::int32_t v = 0;
            switch (kind) {
            case 0:  // nothing
                v = 0;
                break;
            case 1:  // noise
                v = mag ? static_cast<std::int32_t>(rng.next() % (static_cast<std::uint64_t>(mag) + 1u)) : 0;
                if (rng.u(2)) {
                    v = -v;
                }
                break;
            case 2:  // gradient
                v = static_cast<std::int32_t>((static_cast<std::int64_t>(x + y) * mag) / (w + h));
                break;
            case 3:  // sparse spikes
                v = rng.u(16) == 0 ? (mag ? static_cast<std::int32_t>(rng.next() % (static_cast<std::uint64_t>(mag) + 1u)) : 0) : 0;
                if (rng.u(2)) {
                    v = -v;
                }
                break;
            case 4:  // extremes
                v = rng.u(8) == 0 ? (rng.u(2) ? INT32_MIN : INT32_MAX) : static_cast<std::int32_t>(rng.u(1000)) - 500;
                break;
            case 5:  // constant
                v = mag / 2 + 1;
                break;
            default:  // smooth with noise, as an image's high band
                v = static_cast<std::int32_t>(((x * 7 + y * 3) % 61) * (mag / 61 + 1)) + static_cast<std::int32_t>(rng.u(64));
                if ((x ^ y) & 1) {
                    v = -v;
                }
                break;
            }
            data[y * w + x] = v;
        }
    }
}

u32 g_driver_false = 0;  // blocks both coders refused (the output clamp through the driver)

bool encode_block(fz::EncT1* ref, d::EncT1* neo, u32 w, u32 h, u32 orient, bool reversible, Rng& rng, const nj2k::detail::Allocator& alloc)
{
    std::vector<std::int32_t> samples(w * h);
    fill_block(samples.data(), w, h, rng);
    std::memcpy(fz::enc_t1_data(ref), samples.data(), samples.size() * 4);
    std::memcpy(d::enc_t1_data(neo), samples.data(), samples.size() * 4);
    const u32 compno = rng.u(4);
    const u32 level = rng.u(6);
    const double stepsize = 0.0123 + rng.u(2000) / 1000.0;
    const double* norms = reversible ? d::kEncMctNorms : d::kEncMctNormsReal;
    d::EncCblk cr{}, cn{};
    cr.x1 = cn.x1 = static_cast<std::int32_t>(w);
    cr.y1 = cn.y1 = static_cast<std::int32_t>(h);
    const bool okr = fz::enc_t1_encode(ref, cr, orient, compno, level, reversible, stepsize, norms, 3);
    const bool okn = d::enc_t1_encode(neo, cn, orient, compno, level, reversible, stepsize, norms, 3);
    if (okr != okn || cr.numbps != cn.numbps || cr.totalpasses != cn.totalpasses) {
        return false;
    }
    if (!okr) {
        ++g_driver_false;
        return true;
    }
    if (cr.totalpasses == 0) {
        return true;
    }
    nj2k::detail::Arena arena;
    arena.init(&alloc);
    // (the committed passes are the encoder's scratch passes: compare them there, bit for bit)
    if (!fz::enc_t1_commit(ref, arena, cr) || !d::enc_t1_commit(neo, arena, cn)) {
        return false;
    }
    for (u32 i = 0; i < cr.totalpasses; ++i) {
        if (cr.passes[i].rate != cn.passes[i].rate || cr.passes[i].len != cn.passes[i].len ||
            cr.passes[i].term != cn.passes[i].term ||
            std::memcmp(&cr.passes[i].distortiondec, &cn.passes[i].distortiondec, sizeof(double)) != 0) {
            return false;
        }
    }
    const u32 bytes = cr.passes[cr.totalpasses - 1].rate;
    return std::memcmp(cr.data, cn.data, bytes) == 0;
}

void test_encode()
{
    const nj2k::detail::Allocator alloc = nj2k::detail::Allocator::system();
    fz::EncT1* ref = fz::enc_t1_create(alloc);
    d::EncT1* neo = d::enc_t1_create(alloc);
    CHECK_MSG(ref && neo, "no memory");
    if (!ref || !neo) {
        return;
    }
    Rng rng(20261122);
    std::uint64_t blocks = 0;
    // every size, one orientation and mode by rotation; small and edge sizes in every orientation and mode
    const u32 edge[] = {1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64};
    for (u32 h = 1; h <= 64; ++h) {
        for (u32 w = 1; w <= 64; ++w) {
            bool is_edge = false;
            bool is_edge_w = false;
            for (u32 e : edge) {
                is_edge |= e == h;
                is_edge_w |= e == w;
            }
            const u32 reps = (is_edge && is_edge_w) ? 8u : 1u;
            for (u32 r = 0; r < reps; ++r) {
                const u32 combo = reps == 8 ? r : (w * 3u + h) % 8u;
                if (!encode_block(ref, neo, w, h, combo & 3u, (combo >> 2) & 1u, rng, alloc)) {
                    std::fprintf(stderr, "enc_t1_encode differs: %u x %u orient %u %s\n", w, h, combo & 3u, ((combo >> 2) & 1u) ? "5/3" : "9/7");
                    ++g_failures;
                    goto done;
                }
                ++blocks;
            }
        }
    }
    // the full-size block over many contents
    for (u32 i = 0; i < 400; ++i) {
        if (!encode_block(ref, neo, 64, 64, i & 3u, (i >> 2) & 1u, rng, alloc)) {
            std::fprintf(stderr, "enc_t1_encode differs: 64 x 64, content %u\n", i);
            ++g_failures;
            goto done;
        }
        ++blocks;
    }
    std::printf("enc_t1_encode: %llu code-blocks identical, %u refused for overflow (expected 0: unreachable here)\n",
                static_cast<unsigned long long>(blocks), g_driver_false);
    CHECK_MSG(g_driver_false == 0, "%u blocks overflowed the output buffer; the clamp is now reachable through the driver", g_driver_false);
done:
    fz::enc_t1_destroy(ref, alloc);
    d::enc_t1_destroy(neo, alloc);
}

}  // namespace

int main(int argc, char** argv)
{
    nj2k::test::console_reports();
    bool full = false;
    bool exhaustive = true;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--exhaustive-full") == 0) {
            full = true;
        } else if (std::strcmp(argv[i], "--no-exhaustive") == 0) {
            exhaustive = false;
        } else {
            std::fprintf(stderr, "usage: nj2k_enc_t1_unit [--exhaustive-full] [--no-exhaustive]\n");
            return 2;
        }
    }
    if (full) {
        // the nightly run: only the exhaustive comparison
        test_single(true);
    } else {
        test_streams();
        if (exhaustive) {
            test_single(false);
        }
        test_encode();
    }
    if (g_failures) {
        std::fprintf(stderr, "enc_t1_unit: %d failures\n", g_failures);
        return 1;
    }
    std::printf("enc_t1_unit: ok\n");
    return 0;
}

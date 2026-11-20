// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// nj2k_mq_lanes: the stage 4 prototype of a lane-per-code-block MQ decoder. The files are decoded
// once by a copy of the library with Tier-1's trace (nj2k_t1_trace), which records every MQ segment
// (its bytes and its contexts' states) and every decision (its context and symbol). The decisions
// are then decoded again from the bytes, with the contexts as recorded, by the library's scalar MQ
// decoder and by eight AVX2 lanes, each lane a code-block of its own, taking the next segment of
// any image when its own ends (cross-image batching). Both are checked against the recorded
// symbols. Context modelling is left out of both, so the lanes' time is a lower bound of what a
// lane-per-code-block Tier-1 could reach: it pays only if the lanes beat the scalar decoder here.
//
//   nj2k_mq_lanes --files LIST [--rounds 7] [--small 128]
//
// --small: also the segments of images up to this many pixels a side alone (the viewer's small
// textures).
#include <immintrin.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <neoj2k/neoj2k.h>

#include "../src/t1.h"

namespace {

using clk = std::chrono::steady_clock;

struct Stream {
    std::size_t bytes;  // offset of its bytes (with the decoder's 0xFF 0xFF sentinel) in Corpus::bytes
    std::uint32_t len;  // its data bytes
    std::size_t first;  // its first decision in Corpus::ctx and ::sym
    std::size_t count;
    std::uint8_t states[19];
    bool usable;  // no context reset between passes (the replay starts each segment from its states)
};

struct Corpus {
    std::vector<std::uint8_t> bytes;
    std::vector<std::int32_t> ctx;
    std::vector<std::uint8_t> sym;
    std::vector<Stream> streams;
};

void on_segment(void* user, const std::uint8_t* data, std::uint32_t len, const std::uint8_t* states, bool reset)
{
    auto& c = *static_cast<Corpus*>(user);
    Stream s{};
    s.bytes = c.bytes.size();
    s.len = len;
    s.first = c.ctx.size();
    std::memcpy(s.states, states, sizeof s.states);
    s.usable = !reset;
    c.bytes.insert(c.bytes.end(), data, data + len + 2);  // the sentinel the decoder wrote
    c.bytes.insert(c.bytes.end(), 6, 0);                   // room for the lanes' 4-byte reads
    c.streams.push_back(s);
}

void on_decision(void* user, std::uint32_t ctx, std::uint32_t d)
{
    auto& c = *static_cast<Corpus*>(user);
    if (c.streams.empty()) {
        return;
    }
    c.ctx.push_back(static_cast<std::int32_t>(ctx));
    c.sym.push_back(static_cast<std::uint8_t>(d));
    ++c.streams.back().count;
}

std::vector<std::uint8_t> read_file(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Decodes a file with the traced library; its streams are those added. false when it does not decode.
bool trace_file(const std::string& path, std::uint32_t& side)
{
    const auto data = read_file(path);
    nj2k_codec_t* codec = nj2k_create_decompress(NJ2K_CODEC_J2K);
    if (!codec) {
        return false;
    }
    nj2k_dparameters_t p;
    nj2k_set_default_decoder_parameters(&p);
    nj2k_setup_decoder(codec, &p);
    nj2k_image_t image{};
    bool ok = nj2k_read_header_memory(codec, data.data(), data.size(), &image) != NJ2K_FALSE;
    std::vector<std::vector<std::int32_t>> planes;
    std::vector<nj2k_plane_t> pl;
    side = 0;
    for (NJ2K_UINT32 c = 0; ok && c < image.numcomps; ++c) {
        planes.emplace_back(std::size_t(image.comps[c].w) * image.comps[c].h);
        side = std::max({side, image.comps[c].w, image.comps[c].h});
    }
    for (NJ2K_UINT32 c = 0; ok && c < image.numcomps; ++c) {
        pl.push_back(nj2k_plane_t{planes[c].data(), static_cast<NJ2K_INT64>(image.comps[c].w) * 4, 4, c});
    }
    if (ok) {
        const nj2k_output_t out{sizeof(nj2k_output_t), NJ2K_SAMPLE_I32, image.numcomps, pl.data()};
        ok = nj2k_decode_memory(codec, data.data(), data.size(), &out) != NJ2K_FALSE;
    }
    nj2k_destroy_codec(codec);
    return ok;
}

// ---- the scalar replay: the library's MQ decoder (mqc_inl.h, with stage 4's renormalisation) ----

struct Tables {
    std::int32_t qe[94], nmps[94], nlps[94];
};

Tables make_tables()
{
    Tables t{};
    const nj2k::detail::MqState* s = nj2k::detail::t1_trace_states();
    for (int i = 0; i < 94; ++i) {
        t.qe[i] = static_cast<std::int32_t>(s[i].qeval);
        t.nmps[i] = static_cast<std::int32_t>(s[i].nmps - s);
        t.nlps[i] = static_cast<std::int32_t>(s[i].nlps - s);
    }
    return t;
}

struct Scalar {
    std::uint32_t c, a, ct;
    const std::uint8_t* bp;
};

inline void bytein(Scalar& m) noexcept
{
    const std::uint32_t l_c = m.bp[1];
    if (m.bp[0] == 0xff) {
        if (l_c > 0x8f) {
            m.c += 0xff00;
            m.ct = 8;
        } else {
            m.bp++;
            m.c += l_c << 9;
            m.ct = 7;
        }
    } else {
        m.bp++;
        m.c += l_c << 8;
        m.ct = 8;
    }
}

inline void init(Scalar& m, const std::uint8_t* bp, std::uint32_t len) noexcept
{
    m.bp = bp;
    m.c = len == 0 ? 0xffu << 16 : std::uint32_t(*bp) << 16;
    bytein(m);
    m.c <<= 7;
    m.ct -= 7;
    m.a = 0x8000;
}

inline void renorm(Scalar& m) noexcept
{
    std::uint32_t shifts = static_cast<std::uint32_t>(std::countl_zero(m.a)) - 16U;
    do {
        if (m.ct == 0) {
            bytein(m);
        }
        const std::uint32_t k = shifts < m.ct ? shifts : m.ct;
        m.a <<= k;
        m.c <<= k;
        m.ct -= k;
        shifts -= k;
    } while (shifts != 0);
}

// opj_mqc_decode_macro on state indices
inline std::uint32_t decide(Scalar& m, std::int32_t& st, const Tables& t) noexcept
{
    const std::uint32_t qe = static_cast<std::uint32_t>(t.qe[st]);
    const std::uint32_t mps = static_cast<std::uint32_t>(st & 1);
    std::uint32_t d;
    m.a -= qe;
    if ((m.c >> 16) < qe) {
        if (m.a < qe) {
            d = mps;
            st = t.nmps[st];
        } else {
            d = !mps;
            st = t.nlps[st];
        }
        m.a = qe;
        renorm(m);
    } else {
        m.c -= qe << 16;
        if ((m.a & 0x8000) == 0) {
            if (m.a < qe) {
                d = !mps;
                st = t.nlps[st];
            } else {
                d = mps;
                st = t.nmps[st];
            }
            renorm(m);
        } else {
            d = mps;
        }
    }
    return d;
}

std::size_t replay_scalar(const Corpus& c, const std::vector<std::uint32_t>& order, const Tables& t,
                          std::size_t& decisions) noexcept
{
    std::size_t bad = 0;
    decisions = 0;
    for (const std::uint32_t si : order) {
        const Stream& s = c.streams[si];
        std::int32_t st[19];
        for (int k = 0; k < 19; ++k) {
            st[k] = s.states[k];
        }
        Scalar m;
        init(m, c.bytes.data() + s.bytes, s.len);
        const std::int32_t* ctx = c.ctx.data() + s.first;
        const std::uint8_t* sym = c.sym.data() + s.first;
        for (std::size_t i = 0; i < s.count; ++i) {
            bad += decide(m, st[ctx[i]], t) != sym[i];
        }
        decisions += s.count;
    }
    return bad;
}

// ---- eight lanes, one segment each ----

std::size_t replay_lanes(const Corpus& c, const std::vector<std::uint32_t>& order, const Tables& t,
                         std::size_t& decisions) noexcept
{
    alignas(32) std::int32_t A[8], C[8], CT[8], BP[8], POS[8], END[8], states[8 * 19];
    std::size_t next = 0, bad = 0;
    decisions = 0;
    int active = 0;
    bool live[8] = {};
    auto refill = [&](int l) {
        while (next < order.size()) {
            const Stream& s = c.streams[order[next++]];
            if (s.count == 0) {
                continue;
            }
            Scalar m;
            init(m, c.bytes.data() + s.bytes, s.len);
            A[l] = static_cast<std::int32_t>(m.a);
            C[l] = static_cast<std::int32_t>(m.c);
            CT[l] = static_cast<std::int32_t>(m.ct);
            BP[l] = static_cast<std::int32_t>(m.bp - c.bytes.data());
            POS[l] = static_cast<std::int32_t>(s.first);
            END[l] = static_cast<std::int32_t>(s.first + s.count);
            for (int k = 0; k < 19; ++k) {
                states[l * 19 + k] = s.states[k];
            }
            decisions += s.count;
            live[l] = true;
            return;
        }
        live[l] = false;
        // an idle lane decodes nothing: a context of its own that is never compared
        POS[l] = 0;
        END[l] = 0;
    };
    for (int l = 0; l < 8; ++l) {
        refill(l);
        active += live[l];
    }
    const std::int32_t* ctxs = c.ctx.data();
    const std::uint8_t* syms = c.sym.data();
    const auto* bytes = reinterpret_cast<const int*>(c.bytes.data());
    const __m256i lane19 = _mm256_setr_epi32(0, 19, 38, 57, 76, 95, 114, 133);
    const __m256i one = _mm256_set1_epi32(1), x8000 = _mm256_set1_epi32(0x8000), xff = _mm256_set1_epi32(0xff);
    const __m256i x8f = _mm256_set1_epi32(0x8f), eight = _mm256_set1_epi32(8), seven = _mm256_set1_epi32(7);
    const __m256i fifteen = _mm256_set1_epi32(15), x7f = _mm256_set1_epi32(127), ff00 = _mm256_set1_epi32(0xff00);
    __m256i a = _mm256_load_si256(reinterpret_cast<const __m256i*>(A));
    __m256i cr = _mm256_load_si256(reinterpret_cast<const __m256i*>(C));
    __m256i ct = _mm256_load_si256(reinterpret_cast<const __m256i*>(CT));
    __m256i bp = _mm256_load_si256(reinterpret_cast<const __m256i*>(BP));
    __m256i pos = _mm256_load_si256(reinterpret_cast<const __m256i*>(POS));
    __m256i end = _mm256_load_si256(reinterpret_cast<const __m256i*>(END));
    while (active) {
        __m256i livem = _mm256_cmpgt_epi32(end, pos);
        const __m256i ctx = _mm256_mask_i32gather_epi32(_mm256_setzero_si256(), ctxs, pos, livem, 4);
        const __m256i slot = _mm256_add_epi32(lane19, ctx);
        const __m256i st = _mm256_i32gather_epi32(states, slot, 4);
        const __m256i qe = _mm256_i32gather_epi32(t.qe, st, 4);
        const __m256i nmps = _mm256_i32gather_epi32(t.nmps, st, 4);
        const __m256i nlps = _mm256_i32gather_epi32(t.nlps, st, 4);
        const __m256i mps = _mm256_and_si256(st, one);
        const __m256i an = _mm256_sub_epi32(a, qe);
        const __m256i lps = _mm256_cmpgt_epi32(qe, _mm256_srli_epi32(cr, 16));
        const __m256i small = _mm256_cmpgt_epi32(qe, an);  // a < qe
        const __m256i renorm_mps = _mm256_cmpeq_epi32(_mm256_and_si256(an, x8000), _mm256_setzero_si256());
        // LPS: d = small ? mps : !mps, state small ? nmps : nlps; MPS needing renormalisation: the
        // other way round; MPS without: d = mps, the state kept
        const __m256i flip_lps = _mm256_andnot_si256(small, lps);
        const __m256i flip_mps = _mm256_andnot_si256(lps, _mm256_and_si256(renorm_mps, small));
        const __m256i flip = _mm256_or_si256(flip_lps, flip_mps);
        const __m256i d = _mm256_xor_si256(mps, _mm256_and_si256(flip, one));
        const __m256i change = _mm256_or_si256(lps, renorm_mps);
        __m256i nst = _mm256_blendv_epi8(nmps, nlps, flip);
        nst = _mm256_blendv_epi8(st, nst, change);
        cr = _mm256_blendv_epi8(_mm256_sub_epi32(cr, _mm256_slli_epi32(qe, 16)), cr, lps);
        a = _mm256_blendv_epi8(an, qe, lps);
        // the contexts' new states, lane by lane (AVX2 has no scatter)
        alignas(32) std::int32_t slot_v[8], nst_v[8], d_v[8], pos_v[8];
        _mm256_store_si256(reinterpret_cast<__m256i*>(slot_v), slot);
        _mm256_store_si256(reinterpret_cast<__m256i*>(nst_v), nst);
        _mm256_store_si256(reinterpret_cast<__m256i*>(d_v), d);
        _mm256_store_si256(reinterpret_cast<__m256i*>(pos_v), pos);
        const int livebits = _mm256_movemask_ps(_mm256_castsi256_ps(livem));
        for (int l = 0; l < 8; ++l) {
            if (livebits & (1 << l)) {
                states[slot_v[l]] = nst_v[l];
                bad += static_cast<std::uint32_t>(d_v[l]) != syms[pos_v[l]];
            }
        }
        // renormalisation: 15 - floor(log2 a) shifts where needed, taken as c has bits, a byte read
        // where none is left
        const __m256i need = _mm256_and_si256(change, livem);
        const __m256i e = _mm256_sub_epi32(_mm256_srli_epi32(_mm256_castps_si256(_mm256_cvtepi32_ps(a)), 23), x7f);
        __m256i shifts = _mm256_and_si256(_mm256_sub_epi32(fifteen, e), need);
        while (!_mm256_testz_si256(shifts, shifts)) {
            const __m256i in = _mm256_and_si256(_mm256_cmpeq_epi32(ct, _mm256_setzero_si256()),
                                                _mm256_cmpgt_epi32(shifts, _mm256_setzero_si256()));
            if (!_mm256_testz_si256(in, in)) {
                const __m256i w = _mm256_mask_i32gather_epi32(_mm256_setzero_si256(), bytes, bp, in, 1);
                const __m256i b0 = _mm256_and_si256(w, xff);
                const __m256i b1 = _mm256_and_si256(_mm256_srli_epi32(w, 8), xff);
                const __m256i isff = _mm256_cmpeq_epi32(b0, xff);
                const __m256i marker = _mm256_and_si256(isff, _mm256_cmpgt_epi32(b1, x8f));
                const __m256i stuffed = _mm256_andnot_si256(marker, isff);
                // marker: c += 0xff00, ct 8; stuffed: bp + 1, c += b1 << 9, ct 7; else bp + 1, c += b1 << 8, ct 8
                const __m256i add = _mm256_blendv_epi8(_mm256_sllv_epi32(b1, _mm256_blendv_epi8(eight, _mm256_set1_epi32(9), stuffed)),
                                                       ff00, marker);
                cr = _mm256_blendv_epi8(cr, _mm256_add_epi32(cr, add), in);
                ct = _mm256_blendv_epi8(ct, _mm256_blendv_epi8(eight, seven, stuffed), in);
                bp = _mm256_blendv_epi8(bp, _mm256_add_epi32(bp, _mm256_andnot_si256(marker, one)), in);
            }
            const __m256i k = _mm256_min_epi32(shifts, ct);
            a = _mm256_sllv_epi32(a, k);
            cr = _mm256_sllv_epi32(cr, k);
            ct = _mm256_sub_epi32(ct, k);
            shifts = _mm256_sub_epi32(shifts, k);
        }
        pos = _mm256_add_epi32(pos, _mm256_and_si256(livem, one));
        // lanes whose segment ended take the next one
        const __m256i done = _mm256_and_si256(livem, _mm256_cmpeq_epi32(pos, end));
        if (!_mm256_testz_si256(done, done)) {
            const int donebits = _mm256_movemask_ps(_mm256_castsi256_ps(done));
            _mm256_store_si256(reinterpret_cast<__m256i*>(A), a);
            _mm256_store_si256(reinterpret_cast<__m256i*>(C), cr);
            _mm256_store_si256(reinterpret_cast<__m256i*>(CT), ct);
            _mm256_store_si256(reinterpret_cast<__m256i*>(BP), bp);
            _mm256_store_si256(reinterpret_cast<__m256i*>(POS), pos);
            _mm256_store_si256(reinterpret_cast<__m256i*>(END), end);
            for (int l = 0; l < 8; ++l) {
                if (donebits & (1 << l)) {
                    refill(l);
                    active -= !live[l];
                }
            }
            a = _mm256_load_si256(reinterpret_cast<const __m256i*>(A));
            cr = _mm256_load_si256(reinterpret_cast<const __m256i*>(C));
            ct = _mm256_load_si256(reinterpret_cast<const __m256i*>(CT));
            bp = _mm256_load_si256(reinterpret_cast<const __m256i*>(BP));
            pos = _mm256_load_si256(reinterpret_cast<const __m256i*>(POS));
            end = _mm256_load_si256(reinterpret_cast<const __m256i*>(END));
        }
    }
    return bad;
}

double median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    return v.empty() ? 0 : v[v.size() / 2];
}

void compare(const char* name, const Corpus& c, const std::vector<std::uint32_t>& order, const Tables& t, int rounds,
             int& failures)
{
    std::vector<double> ts, tl;
    std::size_t ns = 0, nl = 0, bs = 0, bl = 0;
    for (int r = 0; r < rounds; ++r) {
        for (int k = 0; k < 2; ++k) {
            const bool scalar = ((r + k) & 1) == 0;
            const auto t0 = clk::now();
            if (scalar) {
                bs = replay_scalar(c, order, t, ns);
            } else {
                bl = replay_lanes(c, order, t, nl);
            }
            const double us = std::chrono::duration<double, std::micro>(clk::now() - t0).count();
            (scalar ? ts : tl).push_back(us);
        }
    }
    failures += (bs != 0) + (bl != 0) + (ns != nl);
    const double ms = median(ts), ml = median(tl);
    std::printf("%-6s %8zu segments %10zu decisions: scalar %8.2f ns/decision, 8 lanes %8.2f ns/decision (%.2fx)%s%s\n",
                name, order.size(), ns, ns ? ms * 1000.0 / double(ns) : 0.0, nl ? ml * 1000.0 / double(nl) : 0.0,
                ml > 0 ? ms / ml : 0.0, bs ? " SCALAR MISMATCH" : "", bl ? " LANES MISMATCH" : "");
}

}  // namespace

int main(int argc, char** argv)
{
    std::string list;
    int rounds = 7;
    std::uint32_t small_side = 128;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--files" && i + 1 < argc) {
            list = argv[++i];
        } else if (a == "--rounds" && i + 1 < argc) {
            rounds = std::atoi(argv[++i]);
        } else if (a == "--small" && i + 1 < argc) {
            small_side = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else {
            std::fprintf(stderr, "usage: nj2k_mq_lanes --files LIST [--rounds 7] [--small 128]\n");
            return 2;
        }
    }
    if (list.empty()) {
        std::fprintf(stderr, "usage: nj2k_mq_lanes --files LIST [--rounds 7] [--small 128]\n");
        return 2;
    }
    Corpus corpus;
    nj2k::detail::g_t1_trace = nj2k::detail::T1Trace{&corpus, on_segment, on_decision};
    std::ifstream in(list);
    std::string path;
    std::vector<std::uint32_t> all, small;
    std::size_t files = 0, refused = 0;
    while (std::getline(in, path)) {
        if (!path.empty() && path.back() == '\r') {
            path.pop_back();
        }
        if (path.empty()) {
            continue;
        }
        const std::size_t before = corpus.streams.size();
        std::uint32_t side = 0;
        if (!trace_file(path, side)) {
            ++refused;
        }
        ++files;
        for (std::size_t s = before; s < corpus.streams.size(); ++s) {
            if (!corpus.streams[s].usable || corpus.streams[s].count == 0) {
                continue;
            }
            all.push_back(static_cast<std::uint32_t>(s));
            if (side <= small_side) {
                small.push_back(static_cast<std::uint32_t>(s));
            }
        }
    }
    std::printf("traced %zu files (%zu refused): %zu MQ segments, %zu decisions\n", files, refused, corpus.streams.size(),
                corpus.ctx.size());
    const Tables t = make_tables();
    int failures = 0;
    compare("all", corpus, all, t, rounds, failures);
    compare("small", corpus, small, t, rounds, failures);
    return failures ? 1 : 0;
}

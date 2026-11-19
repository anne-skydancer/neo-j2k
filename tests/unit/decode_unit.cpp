// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// Stage 1c: the decode core. Departure DP-3 (a tile whose tile-parts hold no data bytes decodes, as
// ISO 15444-1 has it, to the flat mid-value), the 9/7 quirk OpenJPEG calls BUG_WEIRD_TWO_INVK,
// departure DP-2 (the DC shift's edges), the wavelet and colour kernels' scalar references against
// their AVX2 versions, the memory decode into the caller's planes (each sample type, stride and
// orientation) against the oracle's image, and the zero-heap gate: once warmed up, a decoder asks
// its allocator for nothing (and calls no operator new) to decode images up to 256 x 256 again.
// The oracle (OpenJPEG at the pinned fork commit) is linked beside neo-j2k; the kernels are reached
// through the static library's internals.
#include <neoj2k/neoj2k.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <random>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#endif

#include "common.h"
#include "decoder.h"
#include "j2k_scan.h"

// the library's internals (by their path: src/ is no include directory)
#include "../../src/dwt.h"
#include "../../src/header.h"
#include "../../src/mct.h"
#include "../../src/t1.h"

using namespace nj2k::test;

// ---- operator new, counted while a decode runs ------------------------------------------------------
// The library has no operator new of its own on the decode path (everything goes through the
// codec's allocator); counting the global one keeps it so. Sanitizer runtimes replace these
// functions themselves, so sanitized builds count the allocator hook only.
#if defined(__SANITIZE_ADDRESS__)
#define NJ2K_TEST_COUNT_NEW 0
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(memory_sanitizer) || __has_feature(thread_sanitizer)
#define NJ2K_TEST_COUNT_NEW 0
#endif
#endif
#ifndef NJ2K_TEST_COUNT_NEW
#define NJ2K_TEST_COUNT_NEW 1
#endif

namespace {
std::atomic<bool> g_count_new{false};
std::atomic<std::size_t> g_news{0};
}  // namespace

#if NJ2K_TEST_COUNT_NEW
void* operator new(std::size_t size)
{
    if (g_count_new.load(std::memory_order_relaxed)) {
        g_news.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* p = std::malloc(size ? size : 1)) {
        return p;
    }
    throw std::bad_alloc();
}
void* operator new(std::size_t size, std::align_val_t al)
{
    if (g_count_new.load(std::memory_order_relaxed)) {
        g_news.fetch_add(1, std::memory_order_relaxed);
    }
    const std::size_t a = static_cast<std::size_t>(al);
#if defined(_WIN32)
    void* p = _aligned_malloc(size ? size : 1, a);
#else
    void* p = nullptr;
    if (posix_memalign(&p, a < sizeof(void*) ? sizeof(void*) : a, size ? size : 1) != 0) {
        p = nullptr;
    }
#endif
    if (!p) {
        throw std::bad_alloc();
    }
    return p;
}
void operator delete(void* p) noexcept
{
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept
{
    std::free(p);
}
void operator delete(void* p, std::align_val_t) noexcept
{
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
void operator delete(void* p, std::size_t, std::align_val_t al) noexcept
{
    operator delete(p, al);
}
#endif

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
        }                                                                                          \
    } while (0)

#define CHECK_MSG(cond, ...)                                                                       \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                            \
            std::printf(__VA_ARGS__);                                                              \
            std::printf("\n");                                                                     \
        }                                                                                          \
    } while (0)

std::string g_corpus;

Bytes corpus_file(const char* name)
{
    auto data = read_file(join_path(g_corpus, name));
    if (!data) {
        std::printf("FAIL: cannot read %s\n", join_path(g_corpus, name).c_str());
        ++g_failures;
        return {};
    }
    return *data;
}

void put_be32(std::uint8_t* p, std::uint32_t v)
{
    p[0] = std::uint8_t(v >> 24);
    p[1] = std::uint8_t(v >> 16);
    p[2] = std::uint8_t(v >> 8);
    p[3] = std::uint8_t(v);
}

float bits_float(std::uint32_t b)
{
    float f;
    std::memcpy(&f, &b, sizeof f);
    return f;
}

std::uint32_t float_bits(float f)
{
    std::uint32_t b;
    std::memcpy(&b, &f, sizeof b);
    return b;
}

// the same float, or both NaN (the payload a NaN keeps depends on operand order, which a compiler
// may swap in scalar code; every NaN leaves the decoder as the same clamped value)
bool same_float_bits(std::int32_t a, std::int32_t b)
{
    const float fa = bits_float(static_cast<std::uint32_t>(a)), fb = bits_float(static_cast<std::uint32_t>(b));
    return a == b || (std::isnan(fa) && std::isnan(fb));
}

// ---- a counting allocator for the hook --------------------------------------------------------------

struct Counting {
    std::size_t allocs = 0, frees = 0, live = 0, peak = 0;

    static void* alloc(void* user, NJ2K_SIZE_T size, NJ2K_SIZE_T alignment)
    {
        auto* c = static_cast<Counting*>(user);
        ++c->allocs;
#if defined(_WIN32)
        void* p = _aligned_malloc(size, alignment < 16 ? 16 : alignment);
#else
        void* p = nullptr;
        if (posix_memalign(&p, alignment < sizeof(void*) ? sizeof(void*) : alignment, size) != 0) {
            p = nullptr;
        }
#endif
        if (p) {
            c->live += size;
            c->peak = c->live > c->peak ? c->live : c->peak;
        }
        return p;
    }
    static void release(void* user, void* ptr, NJ2K_SIZE_T size)
    {
        auto* c = static_cast<Counting*>(user);
        ++c->frees;
        c->live -= size;
#if defined(_WIN32)
        _aligned_free(ptr);
#else
        std::free(ptr);
#endif
    }
    nj2k_allocator_t hook() { return nj2k_allocator_t{sizeof(nj2k_allocator_t), alloc, release, this}; }
};

// ---- the memory decode --------------------------------------------------------------------------------

// A codec for the memory decode: non-strict (partial codestreams decode, as the viewer asks), the
// reduce factor in the parameters.
nj2k_codec_t* memory_codec(int reduce, const nj2k_allocator_t* hook = nullptr)
{
    nj2k_codec_t* codec = hook ? nj2k_create_decompress_with_allocator(NJ2K_CODEC_J2K, hook)
                               : nj2k_create_decompress(NJ2K_CODEC_J2K);
    if (!codec) {
        return nullptr;
    }
    nj2k_dparameters_t p;
    nj2k_set_default_decoder_parameters(&p);
    p.cp_reduce = static_cast<NJ2K_UINT32>(reduce);
    if (!nj2k_setup_decoder(codec, &p) || !nj2k_decoder_set_strict_mode(codec, NJ2K_FALSE)) {
        nj2k_destroy_codec(codec);
        return nullptr;
    }
    return codec;
}

enum class Layout { Planar, BottomUpInterleaved, PaddedRows };

struct MemoryImage {
    bool ok = false;
    nj2k_status_t status = NJ2K_OK;
    std::uint32_t numcomps = 0;
    std::vector<std::uint32_t> w, h;
    std::vector<std::uint8_t> buffer;
    std::vector<nj2k_plane_t> planes;
    std::size_t sample_size = 4;
    // sample c, x, y as written
    std::int64_t sample(std::uint32_t c, std::uint32_t x, std::uint32_t y) const
    {
        const nj2k_plane_t& p = planes[c];
        const auto* at = static_cast<const std::uint8_t*>(p.data) + std::int64_t(y) * p.row_stride +
                         std::int64_t(x) * p.sample_stride;
        if (sample_size == 1) {
            return *at;
        }
        if (sample_size == 2) {
            std::uint16_t v;
            std::memcpy(&v, at, 2);
            return v;
        }
        std::int32_t v;
        std::memcpy(&v, at, 4);
        return v;
    }
};

// Reads the header, lays the planes out, decodes. Every component must share one size for the
// interleaved layout (as the viewer's copy assumes); others fall back to planar.
MemoryImage memory_decode(nj2k_codec_t* codec, const Bytes& data, std::size_t size, std::uint32_t sample_type,
                          Layout layout)
{
    MemoryImage m;
    m.sample_size = sample_type == NJ2K_SAMPLE_U8 ? 1 : sample_type == NJ2K_SAMPLE_U16 ? 2 : 4;
    nj2k_image_t image;
    if (!nj2k_read_header_memory(codec, data.data(), size, &image)) {
        m.status = nj2k_get_status(codec);
        return m;
    }
    m.numcomps = image.numcomps;
    bool same = true;
    for (std::uint32_t c = 0; c < image.numcomps; ++c) {
        m.w.push_back(image.comps[c].w);
        m.h.push_back(image.comps[c].h);
        same = same && image.comps[c].w == image.comps[0].w && image.comps[c].h == image.comps[0].h;
    }
    if (layout == Layout::BottomUpInterleaved && !same) {
        layout = Layout::Planar;
    }
    const std::size_t ss = m.sample_size;
    m.planes.resize(image.numcomps);
    if (layout == Layout::BottomUpInterleaved) {
        const std::size_t w = m.w[0], h = m.h[0], nc = image.numcomps;
        m.buffer.assign(w * h * nc * ss + 1, 0xA5);
        const std::int64_t row = std::int64_t(w * nc * ss);
        for (std::uint32_t c = 0; c < image.numcomps; ++c) {
            std::uint8_t* last_row = m.buffer.data() + (h ? (h - 1) * w * nc * ss : 0) + c * ss;
            m.planes[c] = nj2k_plane_t{last_row, -row, static_cast<NJ2K_UINT32>(nc * ss), c};
        }
    } else {
        std::vector<std::size_t> offset(image.numcomps);
        std::size_t total = 0;
        const std::size_t pad = layout == Layout::PaddedRows ? 3 : 0;
        for (std::uint32_t c = 0; c < image.numcomps; ++c) {
            offset[c] = total;
            total += (std::size_t(m.w[c]) + pad) * m.h[c] * ss;
        }
        m.buffer.assign(total + 1, 0xA5);
        for (std::uint32_t c = 0; c < image.numcomps; ++c) {
            m.planes[c] = nj2k_plane_t{m.buffer.data() + offset[c], std::int64_t((m.w[c] + pad) * ss),
                                       static_cast<NJ2K_UINT32>(ss), c};
        }
    }
    nj2k_output_t out{sizeof(nj2k_output_t), sample_type, image.numcomps, m.planes.data()};
    m.ok = nj2k_decode_memory(codec, data.data(), size, &out) != NJ2K_FALSE;
    m.status = nj2k_get_status(codec);
    return m;
}

// Every sample the memory decode wrote equals the oracle's, converted to the sample type.
bool equal_to_oracle(const MemoryImage& m, const Outcome& o, std::uint32_t sample_type, std::string& why)
{
    if (!o.ok || !m.ok) {
        why = !o.ok ? "the oracle refused" : "neo-j2k refused";
        return o.ok == m.ok;
    }
    if (m.numcomps != o.numcomps || o.planes.size() != o.numcomps) {
        why = "component count";
        return false;
    }
    for (std::uint32_t c = 0; c < m.numcomps; ++c) {
        if (m.w[c] != o.comps[c].w || m.h[c] != o.comps[c].h) {
            why = "component " + std::to_string(c) + " size";
            return false;
        }
        for (std::uint32_t y = 0; y < m.h[c]; ++y) {
            for (std::uint32_t x = 0; x < m.w[c]; ++x) {
                const std::int32_t v = o.planes[c][std::size_t(y) * m.w[c] + x];
                const std::int64_t want = sample_type == NJ2K_SAMPLE_U8    ? std::int64_t(std::uint8_t(v))
                                          : sample_type == NJ2K_SAMPLE_U16 ? std::int64_t(std::uint16_t(v))
                                                                           : std::int64_t(v);
                if (m.sample(c, x, y) != want) {
                    why = "component " + std::to_string(c) + " sample (" + std::to_string(x) + ", " +
                          std::to_string(y) + ")";
                    return false;
                }
            }
        }
    }
    return true;
}

// ---- DP-3 ------------------------------------------------------------------------------------------

// The DP-3 file with one byte after SOD in every tile-part that holds no data bytes, Psot one more:
// the oracle then decodes those tiles (their packet headers are in PPT, every packet empty, so the
// byte is never read). inserted[i] is where the original's i-th padded tile-part ends.
Bytes pad_empty_tile_parts(const Bytes& in, std::vector<std::size_t>& inserted, std::vector<std::uint16_t>& empty_tiles)
{
    std::string why;
    const auto mh = parse_main_header(in, &why);
    if (!mh) {
        std::printf("FAIL: DP-3 file: %s\n", why.c_str());
        ++g_failures;
        return {};
    }
    CHECK(!mh->has_tlm && !mh->has_ppm);  // the padding moves no marker a TLM or PPM would describe
    const auto parts = scan_tile_parts(in, *mh);
    std::vector<int> data_bytes(std::size_t(mh->tiles_x() * mh->tiles_y()), 0);
    Bytes out;
    std::size_t at = 0;
    for (const auto& tp : parts) {
        CHECK(tp.has_ppt);
        if (tp.tile < data_bytes.size()) {
            data_bytes[tp.tile] += int(tp.end - tp.data);
        }
        if (tp.end != tp.data || tp.psot == 0) {
            continue;
        }
        out.insert(out.end(), in.begin() + std::ptrdiff_t(at), in.begin() + std::ptrdiff_t(tp.data));
        put_be32(out.data() + (out.size() - (tp.data - tp.sot)) + 6, tp.psot + 1);
        out.push_back(0);
        inserted.push_back(tp.data);
        at = tp.data;
    }
    out.insert(out.end(), in.begin() + std::ptrdiff_t(at), in.end());
    for (std::size_t t = 0; t < data_bytes.size(); ++t) {
        if (data_bytes[t] == 0) {
            empty_tiles.push_back(static_cast<std::uint16_t>(t));
        }
    }
    return out;
}

void test_dp3()
{
    const Bytes in = corpus_file("edge_ppt_empty_tiles_97.j2k");
    if (in.empty()) {
        return;
    }
    std::vector<std::size_t> inserted;
    std::vector<std::uint16_t> empty_tiles;
    const Bytes padded = pad_empty_tile_parts(in, inserted, empty_tiles);
    CHECK(!inserted.empty() && !empty_tiles.empty());
    auto oracle = make_oracle_backend();
    auto neo = make_neoj2k_backend();

    // OpenJPEG refuses the file (if that changes, the departure goes); it decodes the padded one
    Request rq;
    rq.data = in.data();
    rq.size = in.size();
    rq.capture = true;
    CHECK(!oracle->decode(rq).ok);
    rq.data = padded.data();
    rq.size = padded.size();
    CHECK(oracle->decode(rq).ok);

    // the standard's output: an empty tile's coefficients are all zero, so after the inverse ICT and
    // the DC shift every sample is the mid-value; the other tiles are the oracle's
    rq.data = in.data();
    rq.size = in.size();
    const Outcome whole = neo->decode(rq);
    CHECK(whole.ok);
    const auto mh = parse_main_header(in);
    if (whole.ok && mh) {
        for (std::uint16_t t : empty_tiles) {
            const std::uint64_t p = t % mh->tiles_x(), q = t / mh->tiles_x();
            const std::uint64_t tx0 = std::max<std::uint64_t>(mh->xtosiz + p * mh->xtsiz, mh->xosiz);
            const std::uint64_t ty0 = std::max<std::uint64_t>(mh->ytosiz + q * mh->ytsiz, mh->yosiz);
            const std::uint64_t tx1 = std::min<std::uint64_t>(mh->xtosiz + (p + 1) * mh->xtsiz, mh->xsiz);
            const std::uint64_t ty1 = std::min<std::uint64_t>(mh->ytosiz + (q + 1) * mh->ytsiz, mh->ysiz);
            std::size_t off_mid = 0;
            for (std::uint32_t c = 0; c < whole.numcomps; ++c) {
                const std::int32_t mid = whole.comps[c].sgnd ? 0 : std::int32_t(1) << (whole.comps[c].prec - 1);
                for (std::uint64_t y = ty0; y < ty1; ++y) {
                    for (std::uint64_t x = tx0; x < tx1; ++x) {
                        const std::size_t i = std::size_t((y - whole.y0) * whole.comps[c].w + (x - whole.x0));
                        off_mid += whole.planes[c][i] != mid;
                    }
                }
            }
            CHECK_MSG(off_mid == 0, "tile %u: %zu samples off the mid-value", unsigned(t), off_mid);
        }
    }

    // every request of the reference tables (plain and viewer at reduce 0-5; strict too), on the
    // whole file and on every prefix, equals the oracle's on the padded file cut at the same place:
    // a cut just after an empty tile-part's SOD keeps the padding byte (that tile-part is whole)
    std::size_t decodes = 0, differences = 0;
    for (Mode mode : {Mode::Plain, Mode::Strict, Mode::Viewer}) {
        for (int r = 0; r <= 5; ++r) {
            for (std::size_t n = 1; n <= in.size(); ++n) {
                if (mode == Mode::Strict && n != in.size()) {
                    continue;
                }
                std::size_t m = n;
                for (std::size_t ins : inserted) {
                    m += ins <= n;
                }
                Request a, b;
                a.data = in.data();
                a.size = n;
                b.data = padded.data();
                b.size = m;
                a.reduce = b.reduce = r;
                a.mode = b.mode = mode;
                const Outcome x = neo->decode(a), y = oracle->decode(b);
                ++decodes;
                if (x.ok != y.ok || x.hash != y.hash || x.u8_hash != y.u8_hash) {
                    if (++differences <= 5) {
                        std::printf("FAIL DP-3 %s reduce %d at %zu bytes: neo-j2k %s %016llx, oracle (padded, %zu) %s "
                                    "%016llx\n",
                                    mode_name(mode), r, n, x.ok ? "decoded" : "refused",
                                    static_cast<unsigned long long>(x.hash), m, y.ok ? "decoded" : "refused",
                                    static_cast<unsigned long long>(y.hash));
                    }
                }
            }
        }
    }
    CHECK_MSG(differences == 0, "%zu of %zu decodes differ", differences, decodes);
    std::printf("DP-3: %zu empty tiles, %zu tile-parts padded; %zu decodes equal to the oracle's on the padded file\n",
                empty_tiles.size(), inserted.size(), decodes - differences);
}

// ---- BUG_WEIRD_TWO_INVK ---------------------------------------------------------------------------

// The 9/7 lifting of one low and one high sample (sn = dn = 1, cas = 0) as OpenJPEG's
// opj_v8dwt_decode does it, with the high band's scale as a parameter.
void lift_pair(float& l, float& h, float high_scale)
{
    const float alpha = -1.586134342f, beta = -0.052980118f, gamma = 0.882911075f, delta = 0.443506852f;
    const float k = 1.230174105f;
    l = l * k;
    h = h * high_scale;
    l = l + ((h + h) * -delta);
    h = h + ((-gamma + -gamma) * l);
    l = l + ((h + h) * -beta);
    h = h + ((-alpha + -alpha) * l);
}

// OpenJPEG scales the 9/7 high band by two_invK = 1.625732422f (dwt.c, "BUG_WEIRD_TWO_INVK") and
// compensates in tcd.c with a band gain of 0 for every irreversible band: neither is the standard's
// arithmetic, and both are ported as they are (D2). The kernels take the constant (an impulse in
// the high band gives OpenJPEG's value, not that of 2 / K); the pairing shows end to end, as 9/7
// codestreams decode to the oracle's samples at every reduce level.
void test_two_invk()
{
    for (int avx2 = 0; avx2 < 2; ++avx2) {
#if !defined(__AVX2__)
        if (avx2) {
            continue;
        }
#endif
        float w[2 * 8 + 32] = {};
        for (int k = 0; k < 8; ++k) {
            w[k] = 0.25f * float(k);  // low
            w[8 + k] = 1.0f + float(k);  // high
        }
#if defined(__AVX2__)
        if (avx2) {
            nj2k::detail::kernels::v8dwt_decode_avx2(w, 1, 1, 0);
        } else
#endif
        {
            nj2k::detail::kernels::v8dwt_decode_scalar(w, 1, 1, 0);
        }
        for (int k = 0; k < 8; ++k) {
            float l = 0.25f * float(k), h = 1.0f + float(k);
            lift_pair(l, h, 1.625732422f);
            CHECK_MSG(float_bits(w[k]) == float_bits(l) && float_bits(w[8 + k]) == float_bits(h),
                      "%s lane %d: %.9g %.9g, OpenJPEG's %.9g %.9g", avx2 ? "AVX2" : "scalar", k, double(w[k]),
                      double(w[8 + k]), double(l), double(h));
            float l2 = 0.25f * float(k), h2 = 1.0f + float(k);
            lift_pair(l2, h2, 2.0f / 1.230174105f);
            CHECK(float_bits(w[8 + k]) != float_bits(h2));  // the quirk shows at this impulse
        }
    }
    auto oracle = make_oracle_backend();
    auto neo = make_neoj2k_backend();
    for (const char* name : {"size_17x37_rgb_97.j2k", "layers_8_97.j2k", "prec16_g_97.j2k"}) {
        const Bytes in = corpus_file(name);
        for (int r = 0; r <= 5 && !in.empty(); ++r) {
            Request rq;
            rq.data = in.data();
            rq.size = in.size();
            rq.reduce = r;
            const Outcome a = neo->decode(rq), b = oracle->decode(rq);
            CHECK_MSG(a.ok == b.ok && a.hash == b.hash, "%s reduce %d", name, r);
        }
    }
}

// ---- DP-2 ------------------------------------------------------------------------------------------

// The DC shift at its edges: a 9/7 sample of 2^31 or more gives the maximum on every platform (the
// MSVC build of OpenJPEG gives the minimum), NaN and anything below -2^31 the minimum, rounding is
// to nearest with ties to even (opj_lrintf); the 5/3 shift adds in 64 bits and clamps (OpenJPEG's
// int32 sum is undefined on overflow).
void test_dp2()
{
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    struct Case {
        float v;
        std::int32_t u8, u31;  // prec 8 unsigned (shift 128), prec 31 unsigned (shift 2^30)
    };
    const std::int32_t max31 = std::numeric_limits<std::int32_t>::max();
    const Case cases[] = {
        {2147483648.0f, 255, max31},     {4294967296.0f, 255, max31},     {inf, 255, max31},
        {2147483520.0f, 255, max31},     {-2147483648.0f, 0, 0},          {-2147483904.0f, 0, 0},
        {-inf, 0, 0},                    {nan, 0, 0},                     {0.5f, 128, 1073741824},
        {1.5f, 130, 1073741826},         {2.5f, 130, 1073741826},         {-0.5f, 128, 1073741824},
        {-1.5f, 126, 1073741822},        {-2.5f, 126, 1073741822},        {127.5f, 255, 1073741952},
        {-128.5f, 0, 1073741696},        {1073741823.5f, 255, max31},     {-1073741824.0f, 0, 0},
    };
    for (int avx2 = 0; avx2 < 3; ++avx2) {
#if !defined(__AVX2__)
        if (avx2 == 1) {
            continue;
        }
#endif
        for (int prec : {8, 31}) {
            std::vector<std::int32_t> p;
            for (const Case& c : cases) {
                p.push_back(static_cast<std::int32_t>(float_bits(c.v)));
            }
            const std::int32_t shift = std::int32_t(1) << (prec - 1);
            const std::int32_t hi = prec == 31 ? max31 : (std::int32_t(1) << prec) - 1;
            if (avx2 == 2) {
                // the tile-component function, rows of 5 with a stride of 7
                std::vector<std::int32_t> rows(7 * 4, 0);
                for (std::size_t i = 0; i < p.size(); ++i) {
                    rows[(i / 5) * 7 + i % 5] = p[i];
                }
                nj2k::detail::dc_shift_97(rows.data(), 5, 4, 7, shift, 0, hi);
                for (std::size_t i = 0; i < p.size(); ++i) {
                    p[i] = rows[(i / 5) * 7 + i % 5];
                }
            }
#if defined(__AVX2__)
            else if (avx2 == 1) {
                nj2k::detail::kernels::dc_shift_97_avx2(p.data(), p.size(), shift, 0, hi);
            }
#endif
            else {
                nj2k::detail::kernels::dc_shift_97_scalar(p.data(), p.size(), shift, 0, hi);
            }
            for (std::size_t i = 0; i < p.size(); ++i) {
                const std::int32_t want = prec == 8 ? cases[i].u8 : cases[i].u31;
                CHECK_MSG(p[i] == want, "%s prec %d: %.9g gives %d, not %d",
                          avx2 == 2 ? "dc_shift_97" : avx2 ? "AVX2" : "scalar", prec, double(cases[i].v), p[i], want);
            }
        }
    }
    std::int32_t v[4] = {std::numeric_limits<std::int32_t>::max(), std::numeric_limits<std::int32_t>::min(),
                         std::numeric_limits<std::int32_t>::max() - 100, -5};
    nj2k::detail::dc_shift_53(v, 4, 1, 4, std::int32_t(1) << 30, 0, std::numeric_limits<std::int32_t>::max());
    CHECK(v[0] == std::numeric_limits<std::int32_t>::max() && v[1] == 0 &&
          v[2] == std::numeric_limits<std::int32_t>::max() && v[3] == (std::int32_t(1) << 30) - 5);
}

// ---- the kernels: scalar references against AVX2 ------------------------------------------------

#if defined(__AVX2__)
std::mt19937_64 g_rng(0x6e656f6a326bull);

std::int32_t random_sample(bool wide)
{
    if (wide) {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(g_rng()));
    }
    return static_cast<std::int32_t>(g_rng() % (1u << 21)) - (1 << 20);
}

// a float sample: mostly moderate values, some extremes and specials
std::int32_t random_float_sample()
{
    const unsigned pick = static_cast<unsigned>(g_rng() % 64);
    float f;
    if (pick == 0) {
        static const float specials[] = {0.0f, -0.0f, std::numeric_limits<float>::infinity(),
                                         -std::numeric_limits<float>::infinity(),
                                         std::numeric_limits<float>::quiet_NaN(), 1e-40f, -1e-40f,
                                         2147483648.0f, -2147483648.0f, 2147483520.0f, 0.5f, 1.5f, -2.5f};
        f = specials[g_rng() % (sizeof specials / sizeof specials[0])];
    } else if (pick == 1) {
        f = bits_float(static_cast<std::uint32_t>(g_rng()));
    } else {
        std::uniform_real_distribution<float> d(-70000.0f, 70000.0f);
        f = d(g_rng);
        if (pick < 16) {
            f = std::nearbyint(f) + 0.5f;  // ties
        }
    }
    return static_cast<std::int32_t>(float_bits(f));
}

void test_kernels_colour()
{
    constexpr std::size_t n = (std::size_t(1) << 20) + 5;  // an odd tail after the 8-wide steps
    std::vector<std::int32_t> a0(n), a1(n), a2(n);
    for (int wide = 0; wide < 2; ++wide) {
        for (std::size_t i = 0; i < n; ++i) {
            a0[i] = random_sample(wide);
            a1[i] = random_sample(wide);
            a2[i] = random_sample(wide);
        }
        auto b0 = a0, b1 = a1, b2 = a2;
        nj2k::detail::kernels::rct_scalar(a0.data(), a1.data(), a2.data(), n);
        nj2k::detail::kernels::rct_avx2(b0.data(), b1.data(), b2.data(), n);
        CHECK(a0 == b0 && a1 == b1 && a2 == b2);
    }
    for (std::size_t i = 0; i < n; ++i) {
        a0[i] = random_float_sample();
        a1[i] = random_float_sample();
        a2[i] = random_float_sample();
    }
    {
        auto b0 = a0, b1 = a1, b2 = a2;
        nj2k::detail::kernels::ict_scalar(a0.data(), a1.data(), a2.data(), n);
        nj2k::detail::kernels::ict_avx2(b0.data(), b1.data(), b2.data(), n);
        std::size_t bad = 0;
        for (std::size_t i = 0; i < n; ++i) {
            bad += !same_float_bits(a0[i], b0[i]) || !same_float_bits(a1[i], b1[i]) || !same_float_bits(a2[i], b2[i]);
        }
        CHECK_MSG(bad == 0, "ict: %zu of %zu samples differ", bad, n);
    }
    struct Range {
        std::int32_t shift, lo, hi;
    };
    const Range ranges[] = {{128, 0, 255}, {0, -128, 127}, {2048, 0, 4095}, {32768, 0, 65535}, {0, -32768, 32767},
                            {1 << 30, 0, std::numeric_limits<std::int32_t>::max()}, {0, -(1 << 30), (1 << 30) - 1}};
    for (const Range& r : ranges) {
        for (std::size_t i = 0; i < n; ++i) {
            a0[i] = random_float_sample();
        }
        auto b0 = a0;
        nj2k::detail::kernels::dc_shift_97_scalar(a0.data(), n, r.shift, r.lo, r.hi);
        nj2k::detail::kernels::dc_shift_97_avx2(b0.data(), n, r.shift, r.lo, r.hi);
        CHECK_MSG(a0 == b0, "dc_shift_97 shift %d [%d, %d]", r.shift, r.lo, r.hi);
    }
}

// n int32 at a 64-byte boundary in buf, as the decoder's wavelet scratch is (the AVX2 5/3 kernels
// load and store it aligned, as OpenJPEG's do)
std::int32_t* aligned_ints(std::vector<std::int32_t>& buf, std::size_t n)
{
    buf.assign(n + 16, 0);
    return reinterpret_cast<std::int32_t*>((reinterpret_cast<std::uintptr_t>(buf.data()) + 63) & ~std::uintptr_t(63));
}

// sn and dn of a line of len samples whose first sample has parity cas (OpenJPEG's rw_prev)
void split_line(std::int32_t len, std::int32_t cas, std::int32_t& sn, std::int32_t& dn)
{
    sn = cas == 0 ? (len + 1) / 2 : len / 2;
    dn = len - sn;
}

// Tier-1's store into the tile buffer: the 5/3 halving and the 9/7 dequantisation
void test_kernels_store()
{
    std::size_t samples = 0, bad = 0;
    while (samples < 2000000) {
        const std::uint32_t w = 1 + static_cast<std::uint32_t>(g_rng() % 64), h = 1 + static_cast<std::uint32_t>(g_rng() % 64);
        const std::size_t stride = w + g_rng() % 9;
        const bool wide = (g_rng() & 3) == 0;
        std::vector<std::int32_t> src(std::size_t(w) * h);
        for (auto& v : src) {
            v = random_sample(wide);
        }
        const float step = std::ldexp(1.0f + float(g_rng() % 2048) / 2048.0f, int(g_rng() % 24) - 16);
        for (int reversible = 0; reversible < 2; ++reversible) {
            std::vector<std::int32_t> a(stride * h + 8, 0x5A5A5A5A), b = a;
            if (reversible) {
                nj2k::detail::kernels::store53_scalar(src.data(), w, h, a.data(), stride);
                nj2k::detail::kernels::store53_avx2(src.data(), w, h, b.data(), stride);
            } else {
                nj2k::detail::kernels::store97_scalar(src.data(), w, h, 0.5f * step, a.data(), stride);
                nj2k::detail::kernels::store97_avx2(src.data(), w, h, 0.5f * step, b.data(), stride);
            }
            bad += a != b;
        }
        samples += std::size_t(w) * h;
    }
    CHECK_MSG(bad == 0, "Tier-1 store: %zu blocks differ", bad);
    std::printf("kernels: Tier-1 store %zu samples each\n", samples);
}

void test_kernels_wavelet()
{
    // 5/3 rows
    std::size_t samples = 0, bad = 0;
    while (samples < 2000000) {
        const std::int32_t len = 1 + static_cast<std::int32_t>(g_rng() % 97);
        const std::int32_t cas = static_cast<std::int32_t>(g_rng() & 1);
        const bool wide = (g_rng() & 3) == 0;
        std::int32_t sn, dn;
        split_line(len, cas, sn, dn);
        std::vector<std::int32_t> a(std::size_t(len) + 8), tmp_a, tmp_b;
        for (auto& v : a) {
            v = random_sample(wide);
        }
        auto b = a;
        nj2k::detail::kernels::idwt53_h_scalar(a.data(), sn, dn, cas, aligned_ints(tmp_a, std::size_t(len) + 64));
        nj2k::detail::kernels::idwt53_h_avx2(b.data(), sn, dn, cas, aligned_ints(tmp_b, std::size_t(len) + 64));
        bad += a != b;
        samples += std::size_t(len);
    }
    CHECK_MSG(bad == 0, "idwt53_h: %zu lines differ", bad);
    std::printf("kernels: idwt53_h %zu samples\n", samples);

    // 5/3 columns: groups of 16 (OpenJPEG's SIMD formulas in both) and narrower remainders
    samples = 0;
    bad = 0;
    while (samples < 4000000) {
        const std::int32_t len = 1 + static_cast<std::int32_t>(g_rng() % 97);
        const std::int32_t cas = static_cast<std::int32_t>(g_rng() & 1);
        const std::int32_t cols = (g_rng() & 1) ? 16 : 1 + static_cast<std::int32_t>(g_rng() % 16);
        const std::size_t stride = std::size_t(cols) + g_rng() % 9;
        const bool wide = (g_rng() & 3) == 0;
        std::int32_t sn, dn;
        split_line(len, cas, sn, dn);
        std::vector<std::int32_t> a(stride * std::size_t(len) + 16);
        std::vector<std::int32_t> tmp_a, tmp_b;
        for (auto& v : a) {
            v = random_sample(wide);
        }
        auto b = a;
        nj2k::detail::kernels::idwt53_v_scalar(a.data(), stride, sn, dn, cas, cols,
                                               aligned_ints(tmp_a, std::size_t(len) * 16 + 64));
        nj2k::detail::kernels::idwt53_v_avx2(b.data(), stride, sn, dn, cas, cols,
                                             aligned_ints(tmp_b, std::size_t(len) * 16 + 64));
        bad += a != b;
        samples += std::size_t(len) * std::size_t(cols);
    }
    CHECK_MSG(bad == 0, "idwt53_v: %zu blocks differ", bad);
    std::printf("kernels: idwt53_v %zu samples\n", samples);

    // 9/7, eight lines at once
    samples = 0;
    bad = 0;
    while (samples < 4000000) {
        const std::int32_t len = 1 + static_cast<std::int32_t>(g_rng() % 97);
        const std::int32_t cas = static_cast<std::int32_t>(g_rng() & 1);
        std::int32_t sn, dn;
        split_line(len, cas, sn, dn);
        std::vector<std::int32_t> a(std::size_t(len) * 8 + 32);
        for (auto& v : a) {
            v = random_float_sample();
        }
        auto b = a;
        nj2k::detail::kernels::v8dwt_decode_scalar(reinterpret_cast<float*>(a.data()), sn, dn, cas);
        nj2k::detail::kernels::v8dwt_decode_avx2(reinterpret_cast<float*>(b.data()), sn, dn, cas);
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (!same_float_bits(a[i], b[i])) {
                ++bad;
                break;
            }
        }
        samples += std::size_t(len) * 8;
    }
    CHECK_MSG(bad == 0, "v8dwt: %zu blocks differ", bad);
    std::printf("kernels: v8dwt %zu samples\n", samples);
}
#endif

// ---- the memory decode against the oracle -----------------------------------------------------------

void test_memory_decode()
{
    auto oracle = make_oracle_backend();
    std::size_t decodes = 0, cut_decodes = 0;
    for (const auto& path : expand_inputs(g_corpus)) {
        const std::string name = base_name(path);
        if (name == "edge_ppt_empty_tiles_97.j2k") {
            continue;  // DP-3, above
        }
        const auto data = read_file(path);
        if (!data) {
            continue;
        }
        const bool has_eoc = data->size() >= 2 && (*data)[data->size() - 2] == 0xFF && (*data)[data->size() - 1] == 0xD9;
        for (int r = 0; r <= 5; ++r) {
            Request rq;
            rq.data = data->data();
            rq.size = data->size();
            rq.reduce = r;
            rq.capture = true;
            const Outcome o = oracle->decode(rq);
            nj2k_codec_t* codec = memory_codec(r);
            CHECK(codec != nullptr);
            if (!codec) {
                return;
            }
            const struct {
                std::uint32_t type;
                Layout layout;
            } variants[] = {{NJ2K_SAMPLE_I32, Layout::Planar},
                            {NJ2K_SAMPLE_U8, Layout::BottomUpInterleaved},
                            {NJ2K_SAMPLE_U16, Layout::PaddedRows},
                            {NJ2K_SAMPLE_I32, Layout::BottomUpInterleaved}};
            for (const auto& v : variants) {
                const MemoryImage m = memory_decode(codec, *data, data->size(), v.type, v.layout);
                std::string why;
                CHECK_MSG(equal_to_oracle(m, o, v.type, why), "%s reduce %d type %u layout %d: %s", name.c_str(), r,
                          v.type, int(v.layout), why.c_str());
                // a whole file decodes to NJ2K_OK, or to NJ2K_TRUNCATED when it has no EOC (the
                // codestream goes on past its bytes)
                CHECK_MSG(!m.ok || m.status == (has_eoc ? NJ2K_OK : NJ2K_TRUNCATED), "%s reduce %d: status %s",
                          name.c_str(), r, nj2k_status_string(m.status));
                ++decodes;
            }
            nj2k_destroy_codec(codec);
        }
        // prefixes of the small files: what arrived, as the oracle decodes it, and NJ2K_TRUNCATED
        if (data->size() <= 4096) {
            nj2k_codec_t* codec = memory_codec(0);
            for (std::size_t n = 1; n < data->size(); n += 7) {
                Request rq;
                rq.data = data->data();
                rq.size = n;
                rq.capture = true;
                const Outcome o = oracle->decode(rq);
                const MemoryImage m = memory_decode(codec, *data, n, NJ2K_SAMPLE_I32, Layout::Planar);
                std::string why;
                CHECK_MSG(equal_to_oracle(m, o, NJ2K_SAMPLE_I32, why), "%s cut at %zu: %s", name.c_str(), n,
                          why.c_str());
                CHECK_MSG(!m.ok || m.status == NJ2K_TRUNCATED, "%s cut at %zu: status %s", name.c_str(), n,
                          nj2k_status_string(m.status));
                ++cut_decodes;
            }
            nj2k_destroy_codec(codec);
        }
    }
    std::printf("memory decode: %zu whole-file and %zu prefix decodes equal to the oracle's\n", decodes, cut_decodes);
}

// ---- DP-13 ------------------------------------------------------------------------------------------

// A JP2 file with a box after the codestream box whose length is broken: OpenJPEG's
// opj_end_decompress reads the boxes after the codestream and fails on it; neo-j2k's reads nothing
// more and the decode stands, with the codestream's samples.
void test_dp13()
{
    const Bytes cs = corpus_file("size_16x16_rgb_r5.j2k");
    const auto mh = cs.empty() ? std::nullopt : parse_main_header(cs);
    if (!mh) {
        return;
    }
    Bytes jp2;
    auto u32 = [&](std::uint32_t v) {
        for (int k = 3; k >= 0; --k) {
            jp2.push_back(std::uint8_t(v >> (8 * k)));
        }
    };
    auto bytes = [&](std::initializer_list<std::uint8_t> b) { jp2.insert(jp2.end(), b); };
    u32(12), u32(0x6A502020), u32(0x0D0A870A);                       // signature
    u32(20), u32(0x66747970), u32(0x6A703220), u32(0), u32(0x6A703220);  // ftyp: jp2
    u32(8 + 22 + 15), u32(0x6A703268);                               // jp2h
    u32(22), u32(0x69686472), u32(std::uint32_t(mh->height())), u32(std::uint32_t(mh->width()));
    bytes({0, 3, 7, 7, 0, 0});                                       // ihdr: 3 components, 8 bits
    u32(15), u32(0x636F6C72), bytes({1, 0, 0}), u32(16);             // colr: sRGB
    u32(std::uint32_t(8 + cs.size())), u32(0x6A703263);              // jp2c
    jp2.insert(jp2.end(), cs.begin(), cs.end());
    u32(4), u32(0x78787878);                                         // a box of length 4: broken
    auto oracle = make_oracle_backend();
    auto neo = make_neoj2k_backend();
    Request rq;
    rq.data = jp2.data();
    rq.size = jp2.size();
    rq.jp2 = true;
    rq.capture = true;
    const Outcome a = oracle->decode(rq), b = neo->decode(rq);
    Request bare;
    bare.data = cs.data();
    bare.size = cs.size();
    bare.capture = true;
    const Outcome c = neo->decode(bare);
    CHECK_MSG(!a.ok, "the oracle's opj_end_decompress took the broken box");
    CHECK(b.ok && c.ok && b.planes == c.planes);
}

// ---- the memory API's states ------------------------------------------------------------------------

// A codec decoding from memory recovers from a failure at its next header, and takes the reduce
// factor for it in between (nj2k_set_decoded_resolution_factor() then keeps it, as before a header).
void test_memory_states()
{
    const Bytes a = corpus_file("sl_128_rgba_97.j2k");
    if (a.empty()) {
        return;
    }
    nj2k_codec_t* codec = memory_codec(0);
    CHECK(codec != nullptr);
    if (!codec) {
        return;
    }
    nj2k_image_t image;
    CHECK(nj2k_read_header_memory(codec, a.data(), a.size(), &image) && image.comps[0].w == 128);
    const std::uint8_t junk[16] = {0xFF, 0x4F, 0xFF, 0x51, 0, 2};
    CHECK(!nj2k_read_header_memory(codec, junk, sizeof junk, &image));
    CHECK(!nj2k_set_decoded_resolution_factor(codec, 2) && nj2k_get_status(codec) == NJ2K_E_STATE);
    CHECK(nj2k_read_header_memory(codec, a.data(), a.size(), &image) && image.comps[0].w == 32);
    CHECK(nj2k_set_decoded_resolution_factor(codec, 1) && nj2k_get_status(codec) == NJ2K_OK);
    std::vector<std::int32_t> plane(64 * 64 * 4);
    nj2k_plane_t planes[4];
    for (std::uint32_t c = 0; c < 4; ++c) {
        planes[c] = nj2k_plane_t{plane.data() + c * 64 * 64, 64 * 4, 4, c};
    }
    const nj2k_output_t out{sizeof(nj2k_output_t), NJ2K_SAMPLE_I32, 4, planes};
    CHECK(nj2k_decode_memory(codec, a.data(), a.size(), &out) && nj2k_get_status(codec) == NJ2K_OK);
    // a factor the next codestream does not have: refused at its header under NJ2K_REDUCE_EXACT
    const Bytes one = corpus_file("size_1x1_g.j2k");
    CHECK(!nj2k_set_decoded_resolution_factor(codec, 7));  // refused against the held header
    nj2k_destroy_codec(codec);
    codec = memory_codec(3);
    CHECK(codec && !one.empty() && !nj2k_read_header_memory(codec, one.data(), one.size(), &image) &&
          nj2k_get_status(codec) == NJ2K_E_LIMIT);
    if (codec) {
        nj2k_destroy_codec(codec);
    }
}

// ---- the working-memory bound -------------------------------------------------------------------------

// What a memory decode takes from the codec's allocator beyond the header stays within the working
// memory the limits check computes from the header before anything is allocated (max_memory's
// promise), for every corpus file at every reduce level.
void test_memory_bound()
{
    std::size_t checked = 0;
    double worst = 0;
    std::string worst_name;
    for (const auto& path : expand_inputs(g_corpus)) {
        const auto data = read_file(path);
        if (!data) {
            continue;
        }
        const std::string name = base_name(path);
        for (int r = 0; r <= 5; ++r) {
            // the estimate, from the library's own header model
            nj2k::detail::Allocator sys = nj2k::detail::Allocator::system();
            nj2k::detail::Arena arena;
            arena.init(&sys);
            nj2k::detail::Messages msg;
            nj2k::detail::HeaderSettings hs{};
            nj2k::detail::Source src(data->data(), data->size());
            nj2k::detail::MainHeader* mh = nullptr;
            std::uint64_t need = 0;
            const bool header = nj2k::detail::read_main_header(src, arena, msg, hs, mh) == NJ2K_OK &&
                                std::uint32_t(r) < mh->min_resolutions;
            if (header) {
                need = nj2k::detail::decode_working_memory(*mh, std::uint32_t(r), nullptr);
            }
            arena.release_all();
            if (!header) {
                continue;
            }
            Counting counting;
            const nj2k_allocator_t hook = counting.hook();
            nj2k_codec_t* codec = memory_codec(r, &hook);
            nj2k_image_t image;
            if (!codec || !nj2k_read_header_memory(codec, data->data(), data->size(), &image)) {
                CHECK_MSG(false, "%s reduce %d: no header", name.c_str(), r);
                if (codec) {
                    nj2k_destroy_codec(codec);
                }
                continue;
            }
            const std::size_t header_bytes = counting.live;
            counting.peak = counting.live;
            const MemoryImage m = memory_decode(codec, *data, data->size(), NJ2K_SAMPLE_I32, Layout::Planar);
            const std::size_t used = counting.peak - header_bytes;
            CHECK_MSG(!m.ok || used <= need, "%s reduce %d: %zu bytes beyond the header, estimate %llu", name.c_str(),
                      r, used, static_cast<unsigned long long>(need));
            if (m.ok && need && double(used) / double(need) > worst) {
                worst = double(used) / double(need);
                worst_name = name + " r" + std::to_string(r);
            }
            checked += m.ok;
            nj2k_destroy_codec(codec);
        }
    }
    std::printf("working memory: %zu decodes within the estimate; the closest %.0f %% of it (%s)\n", checked,
                worst * 100, worst_name.c_str());
}

// ---- the zero-heap gate ------------------------------------------------------------------------------

// One U8 plane per component, one after another, in raw (sized here, before anything is counted).
nj2k_output_t u8_planes(const nj2k_image_t& image, std::vector<std::uint8_t>& raw, std::vector<nj2k_plane_t>& planes)
{
    std::size_t total = 0;
    for (std::uint32_t c = 0; c < image.numcomps; ++c) {
        total += std::size_t(image.comps[c].w) * image.comps[c].h;
    }
    raw.assign(total + 1, 0);
    planes.resize(image.numcomps);
    std::size_t at = 0;
    for (std::uint32_t c = 0; c < image.numcomps; ++c) {
        planes[c] = nj2k_plane_t{raw.data() + at, std::int64_t(image.comps[c].w), 1, c};
        at += std::size_t(image.comps[c].w) * image.comps[c].h;
    }
    return nj2k_output_t{sizeof(nj2k_output_t), NJ2K_SAMPLE_U8, image.numcomps, planes.data()};
}

// After a warm-up, decoding an image up to 256 x 256 again (the same one, or another of the set)
// asks the codec's allocator for nothing, and nothing calls operator new.
void test_zero_heap()
{
    std::vector<std::pair<std::string, Bytes>> files;
    for (const auto& path : expand_inputs(g_corpus)) {
        auto data = read_file(path);
        const auto mh = data ? parse_main_header(*data) : std::nullopt;
        if (mh && mh->width() <= 256 && mh->height() <= 256 && base_name(path) != "edge_ppt_empty_tiles_97.j2k") {
            files.emplace_back(base_name(path), std::move(*data));
        }
    }
    CHECK(files.size() >= 20);
    for (int r = 0; r <= 1; ++r) {
        Counting counting;
        const nj2k_allocator_t hook = counting.hook();
        nj2k_codec_t* codec = memory_codec(r, &hook);
        CHECK(codec != nullptr);
        if (!codec) {
            return;
        }
        // warm-up: every file twice
        for (int pass = 0; pass < 2; ++pass) {
            for (const auto& [name, data] : files) {
                (void)memory_decode(codec, data, data.size(), NJ2K_SAMPLE_U8, Layout::BottomUpInterleaved);
            }
        }
        // each file again, three times in a row, then the whole set in turn
        std::size_t same_calls = 0, set_calls = 0, news = 0, decoded = 0;
        for (const auto& [name, data] : files) {
            nj2k_image_t image;
            if (!nj2k_read_header_memory(codec, data.data(), data.size(), &image)) {
                continue;
            }
            std::vector<std::uint8_t> raw;
            std::vector<nj2k_plane_t> planes;
            const nj2k_output_t out = u8_planes(image, raw, planes);
            const std::size_t before = counting.allocs + counting.frees;
            g_news = 0;
            g_count_new = true;
            bool ok = true;
            for (int k = 0; k < 3; ++k) {
                ok = nj2k_decode_memory(codec, data.data(), data.size(), &out) && ok;
            }
            g_count_new = false;
            news += g_news;
            const std::size_t calls = counting.allocs + counting.frees - before;
            CHECK_MSG(calls == 0, "%s reduce %d: %zu allocator calls on a repeat decode", name.c_str(), r, calls);
            same_calls += calls;
            decoded += ok;
        }
        const std::size_t before = counting.allocs + counting.frees;
        for (const auto& [name, data] : files) {
            nj2k_image_t image;
            g_count_new = true;
            const bool header = nj2k_read_header_memory(codec, data.data(), data.size(), &image) != NJ2K_FALSE;
            g_count_new = false;
            if (!header) {
                continue;
            }
            std::vector<std::uint8_t> raw;
            std::vector<nj2k_plane_t> planes;
            const nj2k_output_t out = u8_planes(image, raw, planes);
            g_news = 0;
            g_count_new = true;
            (void)nj2k_decode_memory(codec, data.data(), data.size(), &out);
            g_count_new = false;
            news += g_news;
        }
        set_calls = counting.allocs + counting.frees - before;
        CHECK_MSG(set_calls == 0, "reduce %d: %zu allocator calls over the set after the warm-up", r, set_calls);
#if NJ2K_TEST_COUNT_NEW
        CHECK_MSG(news == 0, "reduce %d: %zu operator new calls while decoding", r, news);
#endif
        std::printf("zero heap, reduce %d: %zu files up to 256 x 256 (%zu decoded), %zu allocator calls on repeats, "
                    "%zu over the set, %zu operator new; codec peak %zu KiB\n",
                    r, files.size(), decoded, same_calls, set_calls, news, counting.peak / 1024);
        nj2k_destroy_codec(codec);
        CHECK(counting.live == 0 && counting.allocs == counting.frees);
    }
}

}  // namespace

int main(int argc, char** argv)
{
    console_reports();
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // every line out before a crash
    if (argc < 2) {
        std::fprintf(stderr, "usage: nj2k_decode_unit <corpus folder>\n");
        return 2;
    }
    g_corpus = argv[1];
    test_dp3();
    test_two_invk();
    test_dp2();
#if defined(__AVX2__)
    test_kernels_colour();
    test_kernels_store();
    test_kernels_wavelet();
#else
    std::printf("kernels: scalar build, no AVX2 versions to compare\n");
#endif
    test_dp13();
    test_memory_decode();
    test_memory_states();
    test_memory_bound();
    test_zero_heap();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

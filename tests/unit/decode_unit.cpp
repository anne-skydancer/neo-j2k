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
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#endif

#include <neoj2k/neoj2k.hpp>

#include "common.h"
#include "decoder.h"
#include "j2k_scan.h"
#include "thread_pool.h"

// the library's internals (by their path: src/ is no include directory)
#include "../../src/dwt.h"
#include "../../src/header.h"
#include "../../src/mct.h"
#include "../../src/output.h"
#include "../../src/parallel.h"
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

// Guarded: planar, with guard bytes before, between and after the planes, checked after the decode
enum class Layout { Planar, BottomUpInterleaved, PaddedRows, Guarded };
constexpr std::size_t kGuard = 256;
constexpr std::uint8_t kGuardByte = 0xA5;

struct MemoryImage {
    bool ok = false;
    bool guards_ok = true;  // Layout::Guarded: no byte outside the planes was written
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
    }
    std::vector<std::size_t> offset(image.numcomps);
    const std::size_t guard = layout == Layout::Guarded ? kGuard : 0;
    if (layout != Layout::BottomUpInterleaved) {
        std::size_t total = guard;
        const std::size_t pad = layout == Layout::PaddedRows ? 3 : 0;
        for (std::uint32_t c = 0; c < image.numcomps; ++c) {
            offset[c] = total;
            total += (std::size_t(m.w[c]) + pad) * m.h[c] * ss + guard;
        }
        m.buffer.assign(total + 1, kGuardByte);
        for (std::uint32_t c = 0; c < image.numcomps; ++c) {
            m.planes[c] = nj2k_plane_t{m.buffer.data() + offset[c], std::int64_t((m.w[c] + pad) * ss),
                                       static_cast<NJ2K_UINT32>(ss), c};
        }
    }
    nj2k_output_t out{sizeof(nj2k_output_t), sample_type, image.numcomps, m.planes.data()};
    m.ok = nj2k_decode_memory(codec, data.data(), size, &out) != NJ2K_FALSE;
    m.status = nj2k_get_status(codec);
    if (guard) {
        auto intact = [&](std::size_t from, std::size_t n) {
            for (std::size_t i = from; i < from + n; ++i) {
                if (m.buffer[i] != kGuardByte) {
                    return false;
                }
            }
            return true;
        };
        m.guards_ok = intact(0, guard);
        for (std::uint32_t c = 0; c < image.numcomps; ++c) {
            m.guards_ok = intact(offset[c] + std::size_t(m.w[c]) * m.h[c] * ss, guard) && m.guards_ok;
        }
    }
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

// ---- codestreams made from corpus files ---------------------------------------------------------------

// The offset of a file's first SOT marker (the end of its main header); 0 (a failure counted) when it
// has no main header.
std::size_t first_sot(const Bytes& in)
{
    const auto mh = parse_main_header(in);
    if (!mh) {
        std::printf("FAIL: no main header\n");
        ++g_failures;
        return 0;
    }
    return mh->end;
}

// The main header of in, then one tile-part of tile 0 (TPsot 0, TNsot 1) holding the marker segments
// markers and, after SOD, the bytes data; then EOC.
Bytes one_tile_part(const Bytes& in, const Bytes& markers, const Bytes& data)
{
    const std::size_t sot = first_sot(in);
    if (!sot) {
        return {};
    }
    Bytes out(in.begin(), in.begin() + std::ptrdiff_t(sot));
    const std::uint8_t segment[12] = {0xFF, 0x90, 0x00, 0x0A, 0x00, 0x00, 0, 0, 0, 0, 0x00, 0x01};
    out.insert(out.end(), segment, segment + 12);
    put_be32(out.data() + sot + 6, std::uint32_t(12 + markers.size() + 2 + data.size()));
    out.insert(out.end(), markers.begin(), markers.end());
    out.push_back(0xFF);
    out.push_back(0x93);
    out.insert(out.end(), data.begin(), data.end());
    out.push_back(0xFF);
    out.push_back(0xD9);
    return out;
}

// in with bytes inserted after its first SOT marker segment (at the start of the first tile-part's
// header), that tile-part's Psot grown by as many.
Bytes into_first_tile_part(const Bytes& in, const Bytes& bytes)
{
    const std::size_t sot = first_sot(in);
    if (!sot) {
        return {};
    }
    Bytes out(in);
    const std::uint32_t psot = std::uint32_t(in[sot + 6]) << 24 | std::uint32_t(in[sot + 7]) << 16 |
                               std::uint32_t(in[sot + 8]) << 8 | in[sot + 9];
    if (psot) {
        put_be32(out.data() + sot + 6, psot + std::uint32_t(bytes.size()));
    }
    out.insert(out.begin() + std::ptrdiff_t(sot + 12), bytes.begin(), bytes.end());
    return out;
}

// A JP2 file around a codestream of w x h with three 8-bit components (sRGB), then the bytes after
// (boxes after the codestream box).
Bytes jp2_file(const Bytes& cs, std::uint64_t w, std::uint64_t h, const Bytes& after)
{
    Bytes jp2;
    auto u32 = [&](std::uint32_t v) {
        for (int k = 3; k >= 0; --k) {
            jp2.push_back(std::uint8_t(v >> (8 * k)));
        }
    };
    auto bytes = [&](std::initializer_list<std::uint8_t> b) { jp2.insert(jp2.end(), b); };
    u32(12), u32(0x6A502020), u32(0x0D0A870A);                           // signature
    u32(20), u32(0x66747970), u32(0x6A703220), u32(0), u32(0x6A703220);  // ftyp: jp2
    u32(8 + 22 + 15), u32(0x6A703268);                                   // jp2h
    u32(22), u32(0x69686472), u32(std::uint32_t(h)), u32(std::uint32_t(w));
    bytes({0, 3, 7, 7, 0, 0});                                           // ihdr: 3 components, 8 bits
    u32(15), u32(0x636F6C72), bytes({1, 0, 0}), u32(16);                 // colr: sRGB
    u32(std::uint32_t(8 + cs.size())), u32(0x6A703263);                  // jp2c
    jp2.insert(jp2.end(), cs.begin(), cs.end());
    jp2.insert(jp2.end(), after.begin(), after.end());
    return jp2;
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

    // DP-3 needs packed packet headers: a tile-part with no data bytes and no PPM or PPT has lost its
    // packets, and both refuse it, in strict mode and not
    const Bytes no_packets = one_tile_part(corpus_file("sl_64_rgb_97.j2k"), {}, {});
    if (!no_packets.empty()) {
        for (Mode mode : {Mode::Plain, Mode::Strict}) {
            Request r;
            r.data = no_packets.data();
            r.size = no_packets.size();
            r.mode = mode;
            CHECK_MSG(!oracle->decode(r).ok && !neo->decode(r).ok, "a tile-part without data or packed headers (%s)",
                      mode_name(mode));
        }
        nj2k_codec_t* codec = memory_codec(0);
        const MemoryImage m = memory_decode(codec, no_packets, no_packets.size(), NJ2K_SAMPLE_U8, Layout::Guarded);
        CHECK_MSG(!m.ok && m.status == NJ2K_E_DATA && m.guards_ok, "memory decode: %s", nj2k_status_string(m.status));
        nj2k_destroy_codec(codec);
    }
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

// Stage 2's fused output pass against the unfused steps it replaces (mct.h's transforms in place
// over the whole buffers, the DC shift per row, then the copy of the low bits), in both builds:
// every layout of the planes (the viewer's interleaved bottom-up raw image of 1 to 4 components,
// planar, interleaved with a gap; 8, 16 and 32 bits), the colour transforms, random and edge
// samples, odd widths; guard bytes around the planes stay untouched (the packed 3-byte store
// overlaps). The second set aims at the viewer's 8-bit image: components of 1 to 8 unsigned bits
// (the fused 8-bit pass, sixteen pixels a step), every component 5/3 or every one 9/7 or mixed,
// widths around sixteen, tiny regions (a pixel at a time up to four pixels) and small contiguous
// ones (finished as one run), rows padded or not.
struct OutputCase {
    std::uint32_t w, h, k;
    nj2k::detail::Mct mct;
    std::uint32_t type;
    int layout;  // 0 interleaved, 1 planar, 2 interleaved with a gap
    bool bottom_up;
    std::size_t src_stride;
    nj2k::detail::Finish f[4];
};

// the case's planes written by write_group against the unfused steps; bad: the planes differ,
// guard: a byte outside them was written
void output_case(const OutputCase& oc, bool& bad, bool& guard)
{
    using nj2k::detail::CopyComp;
    using nj2k::detail::Mct;
    const std::uint32_t w = oc.w, h = oc.h, k = oc.k;
    const std::size_t ss = oc.type == NJ2K_SAMPLE_U8 ? 1 : oc.type == NJ2K_SAMPLE_U16 ? 2 : 4;
    std::vector<std::vector<std::int32_t>> src(k), ref(k);
    for (std::uint32_t c = 0; c < k; ++c) {
        src[c].resize(oc.src_stride * h);
        for (auto& v : src[c]) {
            v = oc.f[c].reversible ? random_sample((g_rng() & 3) == 0) : random_float_sample();
        }
        ref[c] = src[c];
    }
    // the unfused reference: the transform over the whole buffers, then the shift per row
    if (oc.mct == Mct::kRct) {
        nj2k::detail::kernels::rct_scalar(ref[0].data(), ref[1].data(), ref[2].data(), ref[0].data(), ref[1].data(),
                                          ref[2].data(), ref[0].size());
    } else if (oc.mct == Mct::kIct) {
        nj2k::detail::kernels::ict_scalar(ref[0].data(), ref[1].data(), ref[2].data(), ref[0].data(), ref[1].data(),
                                          ref[2].data(), ref[0].size());
    }
    for (std::uint32_t c = 0; c < k; ++c) {
        if (oc.f[c].reversible) {
            nj2k::detail::dc_shift_53(ref[c].data(), w, h, oc.src_stride, oc.f[c].shift, oc.f[c].lo, oc.f[c].hi);
        } else {
            for (std::uint32_t j = 0; j < h; ++j) {
                nj2k::detail::kernels::dc_shift_97_scalar(ref[c].data() + std::size_t(j) * oc.src_stride, w, oc.f[c].shift,
                                                          oc.f[c].lo, oc.f[c].hi);
            }
        }
    }
    // the planes, with 64 guard bytes before and after
    const std::uint32_t pixel = oc.layout == 1 ? 1 : (oc.layout == 2 ? k + 1 : k);
    const std::size_t row = std::size_t(w) * pixel * ss + (oc.layout == 1 ? 3 : 0);
    const std::size_t plane_bytes = row * h * (oc.layout == 1 ? k : 1);
    std::vector<std::uint8_t> buf(plane_bytes + 128, 0xA5);
    std::vector<nj2k_plane_t> planes(k);
    for (std::uint32_t c = 0; c < k; ++c) {
        std::uint8_t* base = buf.data() + 64 + (oc.layout == 1 ? c * row * h : c * ss);
        std::int64_t rs = static_cast<std::int64_t>(row);
        if (oc.bottom_up) {
            base += (h - 1) * row;
            rs = -rs;
        }
        planes[c] = nj2k_plane_t{base, rs, static_cast<NJ2K_UINT32>(pixel * ss), c};
    }
    auto expect = buf;
    for (std::uint32_t c = 0; c < k; ++c) {
        const std::ptrdiff_t off = static_cast<std::uint8_t*>(planes[c].data) - buf.data();
        for (std::uint32_t j = 0; j < h; ++j) {
            for (std::uint32_t i = 0; i < w; ++i) {
                const std::int32_t v = ref[c][std::size_t(j) * oc.src_stride + i];
                std::uint8_t* at =
                    expect.data() + off + std::int64_t(j) * planes[c].row_stride + std::size_t(i) * planes[c].sample_stride;
                if (ss == 1) {
                    *at = static_cast<std::uint8_t>(v);
                } else if (ss == 2) {
                    const std::uint16_t u = static_cast<std::uint16_t>(v);
                    std::memcpy(at, &u, 2);
                } else {
                    std::memcpy(at, &v, 4);
                }
            }
        }
    }
    CopyComp cc[4];
    for (std::uint32_t c = 0; c < k; ++c) {
        cc[c] = CopyComp{src[c].data(), oc.src_stride, 0, 0, w, h, oc.f[c], c, true};
    }
    nj2k::detail::write_group(cc, k, oc.mct, oc.type, planes.data(), k);
    bad = std::memcmp(buf.data() + 64, expect.data() + 64, plane_bytes) != 0;
    guard = std::memcmp(buf.data(), expect.data(), 64) != 0 ||
            std::memcmp(buf.data() + 64 + plane_bytes, expect.data() + 64 + plane_bytes, 64) != 0;
}

void test_output_fused()
{
    using nj2k::detail::Mct;
    std::size_t cases = 0, bad = 0, guard_bad = 0;
    auto run = [&](const OutputCase& oc) {
        bool b = false, g = false;
        output_case(oc, b, g);
        ++cases;
        bad += b;
        guard_bad += g;
    };
    // any layout, type and precision
    const std::uint32_t widths[] = {1, 2, 7, 8, 9, 10, 11, 17, 255, 256, 257, 300, 513};
    for (int round = 0; round < 600; ++round) {
        OutputCase oc{};
        oc.w = widths[g_rng() % std::size(widths)];
        oc.h = 1 + static_cast<std::uint32_t>(g_rng() % 5);
        oc.k = 1 + static_cast<std::uint32_t>(g_rng() % 4);
        oc.mct = Mct::kNone;
        if (oc.k >= 3 && (g_rng() & 1)) {
            oc.mct = (g_rng() & 1) ? Mct::kRct : Mct::kIct;
        }
        const std::uint32_t types[] = {NJ2K_SAMPLE_U8, NJ2K_SAMPLE_U8, NJ2K_SAMPLE_U16, NJ2K_SAMPLE_I32};
        oc.type = types[g_rng() % 4];
        oc.layout = static_cast<int>(g_rng() % 3);
        oc.bottom_up = (g_rng() & 1) != 0;
        oc.src_stride = oc.w + g_rng() % 5;
        for (std::uint32_t c = 0; c < oc.k; ++c) {
            bool reversible = (g_rng() & 1) != 0;
            if (oc.mct == Mct::kRct) {
                reversible = true;
            } else if (oc.mct == Mct::kIct) {
                reversible = false;
            }
            const std::uint32_t prec = 1 + static_cast<std::uint32_t>(g_rng() % 31);
            const bool sgnd = (g_rng() & 3) == 0;
            oc.f[c].reversible = reversible;
            oc.f[c].shift = sgnd ? 0 : static_cast<std::int32_t>(1u << (prec - 1));
            oc.f[c].lo = sgnd ? -static_cast<std::int32_t>(1u << (prec - 1)) : 0;
            oc.f[c].hi = sgnd ? static_cast<std::int32_t>((1u << (prec - 1)) - 1) : static_cast<std::int32_t>((1u << prec) - 1);
        }
        run(oc);
    }
    const std::size_t general = cases;
    // the viewer's 8-bit image: unsigned components of up to 8 bits, interleaved (now and then
    // planar or with a gap), tiny, small and wide regions
    const std::uint32_t u8_widths[] = {1, 2, 3, 4, 5, 15, 16, 17, 31, 32, 33, 47, 48, 64, 100, 1000};
    for (int round = 0; round < 1500; ++round) {
        OutputCase oc{};
        oc.w = u8_widths[g_rng() % std::size(u8_widths)];
        oc.h = 1 + static_cast<std::uint32_t>(g_rng() % (oc.w <= 5 ? 6 : 4));
        oc.k = 1 + static_cast<std::uint32_t>(g_rng() % 4);
        oc.mct = Mct::kNone;
        if (oc.k >= 3 && (g_rng() % 4) != 0) {
            oc.mct = (g_rng() & 1) ? Mct::kRct : Mct::kIct;
        }
        oc.type = NJ2K_SAMPLE_U8;
        oc.layout = (g_rng() % 8) == 0 ? static_cast<int>(1 + g_rng() % 2) : 0;
        oc.bottom_up = (g_rng() % 4) != 0;
        oc.src_stride = (g_rng() & 1) ? oc.w : oc.w + 1 + g_rng() % 4;
        // every component 5/3, every one 9/7 (each with its transform), or mixed now and then
        const int kind = static_cast<int>(g_rng() % 8);
        // no ICT with mixed components: a 5/3 component's integers taken as floats by the ICT can
        // be NaN, and a NaN's payload depends on the compiler's operand order (doc/departures.md)
        if (kind == 0 && oc.mct == Mct::kIct) {
            oc.mct = Mct::kNone;
        }
        const bool all_rev = oc.mct == Mct::kRct || (oc.mct == Mct::kNone && (g_rng() & 1));
        for (std::uint32_t c = 0; c < oc.k; ++c) {
            const bool reversible = kind == 0 ? (g_rng() & 1) != 0 : all_rev;
            const std::uint32_t prec = kind == 1 ? 9 + static_cast<std::uint32_t>(g_rng() % 4)
                                                 : 1 + static_cast<std::uint32_t>(g_rng() % 8);
            oc.f[c].reversible = reversible;
            oc.f[c].shift = static_cast<std::int32_t>(1u << (prec - 1));
            oc.f[c].lo = 0;
            oc.f[c].hi = static_cast<std::int32_t>((1u << prec) - 1);
        }
        run(oc);
    }
    CHECK_MSG(bad == 0, "fused output: %zu of %zu cases differ from the unfused steps", bad, cases);
    CHECK_MSG(guard_bad == 0, "fused output: %zu of %zu cases wrote outside their planes", guard_bad, cases);
    std::printf("fused output: %zu cases against the unfused steps (%zu of them 8-bit images)\n", cases, cases - general);
}

// n int32 at a 64-byte boundary in buf, as the decoder's wavelet scratch is (the AVX2 5/3 kernels
// load and store it aligned, as OpenJPEG's do)
std::int32_t* aligned_ints(std::vector<std::int32_t>& buf, std::size_t n)
{
    buf.assign(n + 16, 0);
    return reinterpret_cast<std::int32_t*>((reinterpret_cast<std::uintptr_t>(buf.data()) + 63) & ~std::uintptr_t(63));
}

// Stage 3's inverse wavelet against stage 1c's (OpenJPEG's structure), whole tile-components of
// random geometry: 1 to 7 resolutions, odd and even origins and sizes, 1 to 300 samples a side,
// random and special samples (9/7: extremes, NaN, infinities; 5/3: int32 wrap-around), in both
// builds: the transform as decodes take it, its scalar kernels, and its row-wise path on every level
// (decodes take it on large levels only). Some buffers have rows longer than the level (a padded
// stride) and some decode fewer levels than the buffer holds (a cut stream, or a reduce factor
// below the tile's own); the whole buffer is compared, so a write outside the decoded region shows.
void test_dwt_whole()
{
    namespace dk = nj2k::detail;
    std::size_t cases = 0, bad = 0, samples = 0, padded = 0, cut = 0;
    for (int round = 0; round < 1500; ++round) {
        const bool reversible = (round & 1) != 0;
        const std::uint32_t numres_buf = 1 + static_cast<std::uint32_t>(g_rng() % 7);
        const std::uint32_t numres = (g_rng() % 4) == 0 ? 1 + static_cast<std::uint32_t>(g_rng() % numres_buf) : numres_buf;
        const std::int32_t x0 = static_cast<std::int32_t>(g_rng() % 9), y0 = static_cast<std::int32_t>(g_rng() % 9);
        const std::int32_t big = (g_rng() % 8) == 0 ? 300 : 70;
        const std::int32_t x1 = x0 + 1 + static_cast<std::int32_t>(g_rng() % big);
        const std::int32_t y1 = y0 + 1 + static_cast<std::int32_t>(g_rng() % big);
        dk::ResRect res[8];
        for (std::uint32_t r = 0; r < numres_buf; ++r) {
            const std::int32_t l = static_cast<std::int32_t>(numres_buf - 1 - r);
            auto cdp = [l](std::int32_t v) { return static_cast<std::int32_t>((std::int64_t(v) + (std::int64_t(1) << l) - 1) >> l); };
            res[r] = dk::ResRect{cdp(x0), cdp(y0), cdp(x1), cdp(y1)};
        }
        // the buffer: the tile's full level, its rows padded now and then
        const std::uint32_t pad = (g_rng() % 4) == 0 ? 1 + static_cast<std::uint32_t>(g_rng() % 37) : 0;
        const std::uint32_t w = static_cast<std::uint32_t>(res[numres_buf - 1].x1 - res[numres_buf - 1].x0) + pad;
        const std::uint32_t h = static_cast<std::uint32_t>(res[numres_buf - 1].y1 - res[numres_buf - 1].y0);
        padded += pad != 0;
        cut += numres != numres_buf;
        std::vector<std::int32_t> src(std::size_t(w) * h);
        const bool wide = (g_rng() & 3) == 0;
        for (auto& v : src) {
            v = reversible ? random_sample(wide) : random_float_sample();
        }
        const std::size_t sa = dk::kernels::dwt_scratch_bytes_reference(res, numres, reversible);
        const std::size_t sb = dk::dwt_scratch_bytes(res, numres, reversible);
        std::vector<std::int32_t> buf_a, buf_b;
        void* scratch_a = aligned_ints(buf_a, sa / 4 + 16);
        void* scratch_b = aligned_ints(buf_b, sb / 4 + 16);
        auto a = src, b = src;
        dk::kernels::dwt_decode_reference(a.data(), w, res, numres, reversible, scratch_a);
        dk::dwt_decode(b.data(), w, res, numres, reversible, scratch_b);
        std::size_t diff = 0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            diff += reversible ? a[i] != b[i] : !same_float_bits(a[i], b[i]);
        }
        // the scalar kernels, and the row-wise transform on every level (scalar and the build's)
        for (int variant = 0; variant < 3; ++variant) {
            auto c = src;
            if (variant == 0) {
                dk::kernels::dwt_decode_scalar(c.data(), w, res, numres, reversible, scratch_b);
            } else {
                dk::kernels::dwt_decode_row_wise(c.data(), w, res, numres, reversible, variant == 1, scratch_b);
            }
            for (std::size_t i = 0; i < a.size(); ++i) {
                diff += reversible ? a[i] != c[i] : !same_float_bits(a[i], c[i]);
            }
        }
        bad += diff != 0;
        if (diff && bad <= 5) {
            std::printf("dwt %s: %u resolutions, (%d, %d)-(%d, %d): %zu samples differ\n", reversible ? "5/3" : "9/7",
                        numres, x0, y0, x1, y1, diff);
        }
        ++cases;
        samples += src.size();
    }
    CHECK_MSG(bad == 0, "inverse wavelet: %zu of %zu tile-components differ from stage 1c's transform", bad, cases);
    CHECK_MSG(padded > 100 && cut > 100, "inverse wavelet: too few padded (%zu) or cut (%zu) cases", padded, cut);
    std::printf("inverse wavelet: %zu tile-components (%zu padded, %zu cut), %zu samples, against stage 1c's transform\n",
                cases, padded, cut, samples);
}

#if defined(__AVX2__)
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
        nj2k::detail::kernels::rct_scalar(a0.data(), a1.data(), a2.data(), a0.data(), a1.data(), a2.data(), n);
        nj2k::detail::kernels::rct_avx2(b0.data(), b1.data(), b2.data(), b0.data(), b1.data(), b2.data(), n);
        CHECK(a0 == b0 && a1 == b1 && a2 == b2);
    }
    for (std::size_t i = 0; i < n; ++i) {
        a0[i] = random_float_sample();
        a1[i] = random_float_sample();
        a2[i] = random_float_sample();
    }
    {
        auto b0 = a0, b1 = a1, b2 = a2;
        nj2k::detail::kernels::ict_scalar(a0.data(), a1.data(), a2.data(), a0.data(), a1.data(), a2.data(), n);
        nj2k::detail::kernels::ict_avx2(b0.data(), b1.data(), b2.data(), b0.data(), b1.data(), b2.data(), n);
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

// The fused pass's row kernels, scalar against AVX2
void test_kernels_output()
{
    using nj2k::detail::Finish;
    using nj2k::detail::Mct;
    namespace kn = nj2k::detail::kernels;
    std::size_t bad = 0, samples = 0;
    const std::size_t lens[] = {0, 1, 7, 8, 9, 10, 15, 16, 17, 31, 255, 256, 1000, 4099};
    for (int round = 0; round < 400; ++round) {
        const std::size_t n = lens[g_rng() % std::size(lens)];
        const bool fl = (g_rng() & 1) != 0;
        std::vector<std::int32_t> a(n), b(n), c(n);
        for (std::size_t i = 0; i < n; ++i) {
            a[i] = fl ? random_float_sample() : random_sample((g_rng() & 3) == 0);
            b[i] = fl ? random_float_sample() : random_sample((g_rng() & 3) == 0);
            c[i] = fl ? random_float_sample() : random_sample((g_rng() & 3) == 0);
        }
        const Mct m = fl ? Mct::kIct : Mct::kRct;
        std::vector<std::int32_t> s0(n), s1(n), s2(n), v0(n), v1(n), v2(n);
        kn::mct_rows_scalar(m, a.data(), b.data(), c.data(), s0.data(), s1.data(), s2.data(), n);
        kn::mct_rows_avx2(m, a.data(), b.data(), c.data(), v0.data(), v1.data(), v2.data(), n);
        bad += s0 != v0 || s1 != v1 || s2 != v2;
        const std::uint32_t prec = 1 + static_cast<std::uint32_t>(g_rng() % 31);
        const bool sgnd = (g_rng() & 3) == 0;
        Finish f{};
        f.shift = sgnd ? 0 : static_cast<std::int32_t>(1u << (prec - 1));
        f.lo = sgnd ? -static_cast<std::int32_t>(1u << (prec - 1)) : 0;
        f.hi = sgnd ? static_cast<std::int32_t>((1u << (prec - 1)) - 1) : static_cast<std::int32_t>((1u << prec) - 1);
        f.reversible = !fl;
        kn::finish_row_scalar(s0.data(), n, f);
        kn::finish_row_avx2(v0.data(), n, f);
        bad += s0 != v0;
        const std::uint32_t types[] = {NJ2K_SAMPLE_U8, NJ2K_SAMPLE_U16, NJ2K_SAMPLE_I32};
        const std::uint32_t type = types[g_rng() % 3];
        const std::uint32_t ss =
            (type == NJ2K_SAMPLE_U8 ? 1u : type == NJ2K_SAMPLE_U16 ? 2u : 4u) * (1 + static_cast<std::uint32_t>(g_rng() % 2));
        std::vector<std::uint8_t> o1(n * ss + 32, 0x5A), o2 = o1;
        kn::store_scalar(s0.data(), n, type, o1.data(), ss);
        kn::store_avx2(s0.data(), n, type, o2.data(), ss);
        bad += o1 != o2;
        for (std::uint32_t kk = 1; kk <= 4; ++kk) {
            const std::int32_t* rows[4] = {s0.data(), s1.data(), s2.data(), v1.data()};
            std::vector<std::uint8_t> p1(n * kk + 32, 0x5A), p2 = p1;
            kn::store_u8_packed_scalar(rows, kk, n, p1.data());
            kn::store_u8_packed_avx2(rows, kk, n, p2.data());
            bad += p1 != p2;
        }
        samples += n;
    }
    CHECK_MSG(bad == 0, "output kernels: %zu rows differ between scalar and AVX2", bad);
    std::printf("kernels: output rows %zu samples\n", samples);
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
                // int32 planes (a single tile decodes into them in place) and the viewer's 8-bit
                // image (a single tile is finished on the way, or in place first when the stream
                // ends before its last resolution and the decoded region is smaller than its buffer)
                for (const std::uint32_t type : {NJ2K_SAMPLE_I32, NJ2K_SAMPLE_U8}) {
                    const Layout layout = type == NJ2K_SAMPLE_I32 ? Layout::Planar : Layout::BottomUpInterleaved;
                    const MemoryImage m = memory_decode(codec, *data, n, type, layout);
                    std::string why;
                    CHECK_MSG(equal_to_oracle(m, o, type, why), "%s cut at %zu type %u: %s", name.c_str(), n, type,
                              why.c_str());
                    // a prefix of a valid file is never damaged: it decodes what arrived, or needs more
                    CHECK_MSG(m.ok ? m.status == NJ2K_TRUNCATED : m.status == NJ2K_E_NEED_DATA,
                              "%s cut at %zu: status %s", name.c_str(), n, nj2k_status_string(m.status));
                    ++cut_decodes;
                }
            }
            nj2k_destroy_codec(codec);
        }
    }
    std::printf("memory decode: %zu whole-file and %zu prefix decodes equal to the oracle's\n", decodes, cut_decodes);
}

// Large single-tile images, made by the oracle's encoder (the viewer's upload: one tile, RLCP, 5/3
// lossless or 9/7 lossy), whose levels reach the row-wise wavelet (5/3 from 512 x 512 samples a level,
// 9/7 from 128 x 128; the corpus's 5/3 files stay below it), square and not: neo-j2k's memory decode
// in every layout and its compatibility API's decode, each against the oracle's at reduce 0-2, and
// cuts of each (the first resolutions only) into the viewer's 8-bit image and 16-bit planes.
void test_large_generated()
{
    auto oracle = make_oracle_backend();
    auto compat = make_neoj2k_backend();
    const struct {
        int w, h, comps;
        bool reversible;
    } images[] = {{1024, 1024, 3, true}, {2048, 600, 4, true}, {600, 2048, 1, true}, {2048, 600, 3, false}};
    std::size_t decodes = 0;
    for (const auto& im : images) {
        // a texture: gradients, a few edges and noise, 8-bit interleaved, bottom-up as the viewer's
        std::vector<std::uint8_t> raw(std::size_t(im.w) * im.h * im.comps);
        for (int y = 0; y < im.h; ++y) {
            for (int x = 0; x < im.w; ++x) {
                for (int c = 0; c < im.comps; ++c) {
                    const int edge = ((x / 97 + y / 61 + c) & 1) * 60;
                    const int v = (x * (c + 1) / 9 + y / (c + 2) + edge + static_cast<int>(g_rng() % 23)) & 0xFF;
                    raw[(std::size_t(y) * im.w + x) * im.comps + c] = static_cast<std::uint8_t>(v);
                }
            }
        }
        Bytes data;
        CHECK_MSG(oracle->encode_upload(raw.data(), im.w, im.h, im.comps, im.reversible, data),
                  "the oracle's encoder refused %dx%d", im.w, im.h);
        for (int r = 0; r <= 2 && !data.empty(); ++r) {
            Request rq;
            rq.data = data.data();
            rq.size = data.size();
            rq.reduce = r;
            rq.capture = true;
            const Outcome o = oracle->decode(rq);
            const Outcome n = compat->decode(rq);
            CHECK_MSG(o.ok && n.ok && o.hash == n.hash, "%dx%d %s reduce %d: the compatibility API's decode differs",
                      im.w, im.h, im.reversible ? "5/3" : "9/7", r);
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
                            {NJ2K_SAMPLE_I32, Layout::Guarded}};
            for (const auto& v : variants) {
                const MemoryImage m = memory_decode(codec, data, data.size(), v.type, v.layout);
                std::string why;
                CHECK_MSG(equal_to_oracle(m, o, v.type, why) && m.guards_ok, "%dx%d %s reduce %d type %u layout %d: %s",
                          im.w, im.h, im.reversible ? "5/3" : "9/7", r, v.type, int(v.layout), why.c_str());
                ++decodes;
            }
            nj2k_destroy_codec(codec);
        }
        // cuts (RLCP: the first resolutions only, a decoded region smaller than the tile's buffer),
        // the viewer's first request among them
        nj2k_codec_t* codec = memory_codec(0);
        CHECK(codec != nullptr);
        for (const std::size_t n : {std::size_t(600), std::size_t(2000), data.size() / 20, data.size() / 3}) {
            if (!codec || n >= data.size()) {
                continue;
            }
            Request rq;
            rq.data = data.data();
            rq.size = n;
            rq.capture = true;
            const Outcome o = oracle->decode(rq);
            for (const std::uint32_t type : {NJ2K_SAMPLE_U8, NJ2K_SAMPLE_U16}) {
                const Layout layout = type == NJ2K_SAMPLE_U8 ? Layout::BottomUpInterleaved : Layout::PaddedRows;
                const MemoryImage m = memory_decode(codec, data, n, type, layout);
                std::string why;
                CHECK_MSG(equal_to_oracle(m, o, type, why), "%dx%d %s cut at %zu type %u: %s", im.w, im.h,
                          im.reversible ? "5/3" : "9/7", n, type, why.c_str());
                ++decodes;
            }
        }
        if (codec) {
            nj2k_destroy_codec(codec);
        }
    }
    std::printf("large generated images: %zu memory decodes and the compatibility API's equal to the oracle's\n",
                decodes);
}

// The colour transform over three components whose decoded regions differ in shape but not in
// size (component 0 subsampled across, 1 down, 2 across, so OpenJPEG's sample-count check passes):
// OpenJPEG pairs the samples of the three buffers as flat arrays, so the fused pass cannot pair them
// by place, and write_tile finishes the tile in place first and copies it. OpenJPEG's encoder keeps
// the transform off for such components; the test sets it in the COD. One tile and a grid of tiles
// (clipped at the image's edge), 5/3 and 9/7, reduce 0-3 (at 3 the counts differ and both refuse),
// the memory decode in every layout and the compatibility API's, against the oracle.
void test_mct_unpaired()
{
    auto oracle = make_oracle_backend();
    auto compat = make_neoj2k_backend();
    std::size_t decodes = 0;
    for (int variant = 0; variant < 4; ++variant) {
        EncodeSpec e;
        e.width = 232;
        e.height = 136;
        e.dx = {2, 1, 2};
        e.dy = {1, 2, 1};
        e.tile = (variant & 1) ? 64 : 0;
        e.reversible = (variant & 2) == 0;
        e.numres = 4;
        for (std::size_t c = 0; c < 3; ++c) {
            const std::uint32_t w = (e.width + e.dx[c] - 1) / e.dx[c], h = (e.height + e.dy[c] - 1) / e.dy[c];
            std::vector<std::uint8_t> plane(std::size_t(w) * h);
            for (std::uint32_t y = 0; y < h; ++y) {
                for (std::uint32_t x = 0; x < w; ++x) {
                    plane[std::size_t(y) * w + x] =
                        static_cast<std::uint8_t>(x * (c + 2) + y * (3 - c) + ((x / 13 + y / 7) & 1) * 90 + g_rng() % 17);
                }
            }
            e.planes.push_back(std::move(plane));
        }
        Bytes data;
        CHECK_MSG(oracle_encode(e, data), "the oracle's encoder refused variant %d", variant);
        // the COD's multiple component transform byte (Lcod, Scod, progression, layers, then MCT)
        std::size_t cod = 0;
        for (std::size_t i = 2; i + 9 < data.size(); ++i) {
            if (data[i] == 0xFF && data[i + 1] == 0x52) {
                cod = i;
                break;
            }
        }
        CHECK_MSG(cod && data[cod + 8] == 0, "variant %d: no COD with the transform off", variant);
        if (!cod) {
            continue;
        }
        data[cod + 8] = 1;
        for (int r = 0; r <= 3; ++r) {
            Request rq;
            rq.data = data.data();
            rq.size = data.size();
            rq.reduce = r;
            rq.capture = true;
            const Outcome o = oracle->decode(rq);
            // at reduce 3 the three regions' sample counts differ (rounding), and both refuse the tile
            CHECK_MSG(o.ok == (r < 3), "variant %d reduce %d: the oracle's outcome is not the expected one", variant, r);
            const Outcome n = compat->decode(rq);
            CHECK_MSG(n.ok == o.ok && (!o.ok || o.hash == n.hash),
                      "variant %d reduce %d: the compatibility API's decode differs", variant, r);
            nj2k_codec_t* codec = memory_codec(r);
            CHECK(codec != nullptr);
            if (!codec) {
                return;
            }
            for (const Layout layout : {Layout::Planar, Layout::PaddedRows, Layout::Guarded}) {
                for (const std::uint32_t type : {NJ2K_SAMPLE_I32, NJ2K_SAMPLE_U8, NJ2K_SAMPLE_U16}) {
                    const MemoryImage m = memory_decode(codec, data, data.size(), type, layout);
                    std::string why;
                    CHECK_MSG(equal_to_oracle(m, o, type, why) && m.guards_ok, "variant %d reduce %d type %u layout %d: %s",
                              variant, r, type, int(layout), why.c_str());
                    ++decodes;
                }
            }
            nj2k_destroy_codec(codec);
        }
    }
    std::printf("colour transform over unpaired regions: %zu memory decodes and the compatibility API's equal to "
                "the oracle's\n", decodes);
}

// A single tile whose last resolutions are never decoded: the viewer's upload (RLCP, one tile) with
// a POC after the COD that ends the progression at resolution k, so the decoded region is the
// level k - 1 of a buffer of the full level. OpenJPEG finishes the tile in place and hands the whole
// buffer over; neo-j2k does the same (hand_over_whole). Every k, 5/3 and 9/7, three components and
// four, into the viewer's 8-bit image, 16-bit planes, int32 planes (decoded into in place) and
// through the compatibility API, against the oracle.
void test_poc_fewer_resolutions()
{
    auto oracle = make_oracle_backend();
    auto compat = make_neoj2k_backend();
    std::size_t decodes = 0;
    for (int variant = 0; variant < 4; ++variant) {
        const int w = 160, h = 96, comps = (variant & 1) ? 4 : 3;
        const bool reversible = (variant & 2) == 0;
        std::vector<std::uint8_t> raw(std::size_t(w) * h * comps);
        for (std::size_t i = 0; i < raw.size(); ++i) {
            raw[i] = static_cast<std::uint8_t>((i * 7 / 5 + (i / 977) * 31 + g_rng() % 29) & 0xFF);
        }
        Bytes data;
        CHECK_MSG(oracle->encode_upload(raw.data(), w, h, comps, reversible, data), "the oracle's encoder refused");
        std::size_t sot = 0;
        for (std::size_t i = 2; i + 1 < data.size(); ++i) {
            if (data[i] == 0xFF && data[i + 1] == 0x90) {
                sot = i;
                break;
            }
        }
        CHECK_MSG(sot != 0, "variant %d: no SOT", variant);
        if (!sot) {
            continue;
        }
        for (std::uint8_t k = 1; k <= 5; ++k) {
            // POC: RSpoc 0, CSpoc 0, LYEpoc 65535, REpoc k, CEpoc comps, RLCP
            const std::uint8_t poc[] = {0xFF, 0x5F, 0x00, 0x09, 0x00, 0x00, 0xFF, 0xFF, k, std::uint8_t(comps), 0x01};
            Bytes file(data);
            file.insert(file.begin() + std::ptrdiff_t(sot), std::begin(poc), std::end(poc));
            for (int r = 0; r <= 1; ++r) {
                Request rq;
                rq.data = file.data();
                rq.size = file.size();
                rq.reduce = r;
                rq.capture = true;
                const Outcome o = oracle->decode(rq);
                const Outcome n = compat->decode(rq);
                CHECK_MSG(n.ok == o.ok && (!o.ok || o.hash == n.hash),
                          "variant %d POC to resolution %u reduce %d: the compatibility API's decode differs", variant,
                          unsigned(k), r);
                nj2k_codec_t* codec = memory_codec(r);
                CHECK(codec != nullptr);
                if (!codec) {
                    return;
                }
                const struct {
                    std::uint32_t type;
                    Layout layout;
                } variants[] = {{NJ2K_SAMPLE_U8, Layout::BottomUpInterleaved},
                                {NJ2K_SAMPLE_U16, Layout::PaddedRows},
                                {NJ2K_SAMPLE_I32, Layout::Planar}};
                for (const auto& v : variants) {
                    const MemoryImage m = memory_decode(codec, file, file.size(), v.type, v.layout);
                    std::string why;
                    CHECK_MSG(equal_to_oracle(m, o, v.type, why), "variant %d POC to resolution %u reduce %d type %u: %s",
                              variant, unsigned(k), r, v.type, why.c_str());
                    ++decodes;
                }
                nj2k_destroy_codec(codec);
            }
        }
    }
    std::printf("a single tile's last resolutions not decoded (POC): %zu memory decodes and the compatibility "
                "API's equal to the oracle's\n", decodes);
}

// ---- DP-12 ------------------------------------------------------------------------------------------

// poc_lrcp_rlcp.j2k with the POC of its first tile-part header moved into the main header: right
// after SIZ, before the COD (before_cod), or at the main header's end, after it. Empty (a failure
// counted) when the file is not as expected.
Bytes poc_into_main_header(const Bytes& in, bool before_cod)
{
    const auto mh = in.size() > 6 ? parse_main_header(in) : std::nullopt;
    const auto tps = mh ? scan_tile_parts(in, *mh) : std::vector<TilePart>{};
    std::size_t poc = 0, poc_len = 0;
    if (!tps.empty()) {
        for (std::size_t pos = tps[0].sot + 12; pos + 4 <= tps[0].data - 2;) {
            const std::uint32_t m = std::uint32_t(in[pos]) << 8 | in[pos + 1];
            const std::size_t length = std::size_t(in[pos + 2]) << 8 | in[pos + 3];
            if (m == 0xFF5F) {
                poc = pos;
                poc_len = 2 + length;
                break;
            }
            pos += 2 + length;
        }
    }
    if (!poc || tps[0].psot <= poc_len) {
        std::printf("FAIL: no POC in the first tile-part header\n");
        ++g_failures;
        return {};
    }
    const Bytes segment(in.begin() + std::ptrdiff_t(poc), in.begin() + std::ptrdiff_t(poc + poc_len));
    Bytes out(in);
    out.erase(out.begin() + std::ptrdiff_t(poc), out.begin() + std::ptrdiff_t(poc + poc_len));
    put_be32(out.data() + tps[0].sot + 6, tps[0].psot - std::uint32_t(poc_len));
    const std::size_t at = before_cod ? 4 + (std::size_t(in[4]) << 8 | in[5]) : mh->end;  // after SIZ, or the end
    out.insert(out.begin() + std::ptrdiff_t(at), segment.begin(), segment.end());
    return out;
}

// DP-12: OpenJPEG bounds a POC's layer end by the layers known when it reads the POC, so a main
// header POC read before the COD gives no packet (every sample the mid-value); neo-j2k bounds it by
// the tile's layers when it decodes, as the standard has it, whatever the marker order. After the
// COD the two decode alike, and as the file with the POC in its tile-part header.
void test_dp12()
{
    const Bytes in = corpus_file("poc_lrcp_rlcp.j2k");
    if (in.empty()) {
        return;
    }
    const Bytes before = poc_into_main_header(in, true), after = poc_into_main_header(in, false);
    if (before.empty() || after.empty()) {
        return;
    }
    auto oracle = make_oracle_backend();
    auto neo = make_neoj2k_backend();
    for (Mode mode : {Mode::Plain, Mode::Strict}) {
        auto decode = [mode](Backend& b, const Bytes& data) {
            Request rq;
            rq.data = data.data();
            rq.size = data.size();
            rq.mode = mode;
            return b.decode(rq);
        };
        const Outcome file = decode(*neo, in), na = decode(*neo, after), oa = decode(*oracle, after);
        const Outcome nb = decode(*neo, before), ob = decode(*oracle, before);
        CHECK_MSG(file.ok && na.ok && oa.ok && oa.hash == na.hash && na.hash == file.hash,
                  "%s: the POC after the COD", mode_name(mode));
        CHECK_MSG(nb.ok && nb.hash == file.hash, "%s: neo-j2k, the POC before the COD", mode_name(mode));
        CHECK_MSG(ob.ok && ob.hash != nb.hash, "%s: the oracle's layer end before the COD", mode_name(mode));
    }
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
    // a box of length 4 after the codestream box: broken
    const Bytes jp2 = jp2_file(cs, mh->width(), mh->height(), {0, 0, 0, 4, 0x78, 0x78, 0x78, 0x78});
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
    std::map<std::string, std::array<std::size_t, 6>> per_request;  // bytes beyond the header, by reduce level
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
            if (m.ok) {
                per_request[name][std::size_t(r)] = used;
            }
            checked += m.ok;
            nj2k_destroy_codec(codec);
        }
    }
    std::printf("working memory: %zu decodes within the estimate; the closest %.0f %% of it (%s)\n", checked,
                worst * 100, worst_name.c_str());
    // the record per request: what the decode takes at each reduce level of the file that takes most at
    // reduce 0 (the dropped resolutions keep only their packet headers' state)
    const auto largest = std::max_element(per_request.begin(), per_request.end(),
                                          [](const auto& x, const auto& y) { return x.second[0] < y.second[0]; });
    if (largest != per_request.end()) {
        std::printf("working memory by reduce level, %s:", largest->first.c_str());
        for (std::size_t r = 0; r < 6; ++r) {
            std::printf(" r%zu %.1f KiB", r, double(largest->second[r]) / 1024.0);
        }
        std::printf("\n");
    }
}

// ---- the review round's cases (stage 1c) ------------------------------------------------------------

// A memory codec as the viewer will set one up: non-strict, the Second Life limits, the reduce policy.
nj2k_codec_t* viewer_codec(int reduce, std::uint32_t policy)
{
    nj2k_codec_t* codec = memory_codec(reduce);
    nj2k_decoder_limits_t limits;
    if (!codec || !nj2k_decoder_limits_init(&limits, NJ2K_DECODER_LIMITS_SL_TEXTURE) ||
        !nj2k_decoder_set_limits(codec, &limits) || !nj2k_decoder_set_reduce_policy(codec, policy)) {
        std::printf("FAIL: no viewer codec\n");
        ++g_failures;
    }
    return codec;
}

// A tile-part COD whose component has fewer resolutions than the reduce factor in effect: the tile
// has no level of the image's size, so it is refused under NJ2K_REDUCE_EXACT and NJ2K_REDUCE_CLAMP
// alike, before anything is written (it overran the caller's planes under CLAMP); at reduce 0 it
// decodes as the oracle decodes it.
void test_clamp_tile_styles()
{
    const Bytes in = corpus_file("sl_64_rgb_97.j2k");
    const auto mh = in.empty() ? std::nullopt : parse_main_header(in);
    if (!mh) {
        return;
    }
    // COD: one layer, the main header's transform and colour transform, no decomposition level
    const Bytes cod = {0xFF, 0x52, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x01, mh->mct, 0x00, 0x04, 0x04, 0x00, mh->transform};
    const Bytes cs = one_tile_part(in, cod, {0x00, 0x00, 0x00, 0x00});
    for (std::uint32_t policy : {NJ2K_REDUCE_EXACT, NJ2K_REDUCE_CLAMP}) {
        for (int r = 1; r <= 5; ++r) {
            nj2k_codec_t* codec = viewer_codec(r, policy);
            const MemoryImage m = memory_decode(codec, cs, cs.size(), NJ2K_SAMPLE_U8, Layout::Guarded);
            CHECK_MSG(!m.ok && m.status == NJ2K_E_LIMIT && m.guards_ok, "policy %u reduce %d: %s, guards %s", policy, r,
                      nj2k_status_string(m.status), m.guards_ok ? "intact" : "WRITTEN");
            nj2k_destroy_codec(codec);
        }
    }
    auto oracle = make_oracle_backend();
    Request rq;
    rq.data = cs.data();
    rq.size = cs.size();
    rq.capture = true;
    const Outcome o = oracle->decode(rq);
    nj2k_codec_t* codec = viewer_codec(0, NJ2K_REDUCE_CLAMP);
    const MemoryImage m = memory_decode(codec, cs, cs.size(), NJ2K_SAMPLE_U8, Layout::Guarded);
    std::string why;
    CHECK_MSG(o.ok && equal_to_oracle(m, o, NJ2K_SAMPLE_U8, why) && m.guards_ok, "reduce 0: %s", why.c_str());
    nj2k_destroy_codec(codec);
}

// Markers in and between tile-part headers. SOC, EOC and EPH have no place in a tile-part header and
// a marker without a segment none where a SOT is expected: both decoders refuse them. A marker
// without a segment inside a tile-part header is skipped (DP-7), where OpenJPEG fails.
void test_tile_part_markers()
{
    const Bytes in = corpus_file("sl_128_rgba_97.j2k");
    const std::size_t sot = in.empty() ? 0 : first_sot(in);
    if (!sot) {
        return;
    }
    auto oracle = make_oracle_backend();
    auto neo = make_neoj2k_backend();
    auto decode = [&](Backend& b, const Bytes& cs) {
        Request rq;
        rq.data = cs.data();
        rq.size = cs.size();
        return b.decode(rq);
    };
    const Outcome original = decode(*neo, in);
    CHECK(original.ok);
    for (std::uint8_t m : {std::uint8_t(0xD9), std::uint8_t(0x92), std::uint8_t(0x4F)}) {
        const Bytes cs = into_first_tile_part(in, {0xFF, m, 0x00, 0x04, 0x00, 0x00});
        CHECK_MSG(!decode(*oracle, cs).ok && !decode(*neo, cs).ok, "0xFF%02X in a tile-part header", m);
        nj2k_codec_t* codec = memory_codec(0);
        const MemoryImage mi = memory_decode(codec, cs, cs.size(), NJ2K_SAMPLE_U8, Layout::Guarded);
        CHECK_MSG(!mi.ok && mi.status == NJ2K_E_HEADER, "0xFF%02X in a tile-part header: %s", m,
                  nj2k_status_string(mi.status));
        nj2k_destroy_codec(codec);
    }
    // DP-7: inside the header, skipped
    const Bytes skipped = into_first_tile_part(in, {0xFF, 0x30});
    const Outcome s = decode(*neo, skipped);
    CHECK_MSG(!decode(*oracle, skipped).ok && s.ok && s.hash == original.hash, "0xFF30 in a tile-part header");
    // where a SOT is expected: the tile-part declares no count of tile-parts (TNsot 0), so the walk
    // reads the next marker before it decodes the tile
    const bool one_part = in.size() >= 2 && in[in.size() - 2] == 0xFF && in[in.size() - 1] == 0xD9 && in[sot + 10] == 0 &&
                          in[sot + 11] == 1;
    CHECK(one_part);
    if (one_part) {
        Bytes open = in;
        open[sot + 11] = 0;
        const Outcome a = decode(*oracle, open), b = decode(*neo, open);
        CHECK_MSG(a.ok && b.ok && a.hash == b.hash && b.hash == original.hash, "TNsot 0");
        Bytes between = open;
        between.insert(between.end() - 2, {0xFF, 0x30});
        CHECK_MSG(!decode(*oracle, between).ok && !decode(*neo, between).ok, "0xFF30 where a SOT is expected");
    }
}

// A multi-tile codestream whose bytes end before its first tile's data: NJ2K_E_NEED_DATA, as for a
// single tile (nothing was found damaged).
void test_need_data()
{
    for (const char* name : {"tile_64_of_16.j2k", "sl_128_rgba_97.j2k"}) {
        const Bytes in = corpus_file(name);
        const std::size_t sot = in.empty() ? 0 : first_sot(in);
        if (!sot) {
            continue;
        }
        nj2k_codec_t* codec = memory_codec(0);
        for (std::size_t n : {sot + 2, sot + 6, sot + 12}) {
            const MemoryImage m = memory_decode(codec, in, n, NJ2K_SAMPLE_I32, Layout::Planar);
            CHECK_MSG(!m.ok && m.status == NJ2K_E_NEED_DATA, "%s cut at %zu: %s", name, n, nj2k_status_string(m.status));
        }
        nj2k_destroy_codec(codec);
    }
}

// A sparse large component must not retain coefficients from an earlier full decode.
void test_sparse_owned_reuse()
{
    const Bytes full = corpus_file("large_4096_g_97.j2k");
    if (full.empty()) {
        return;
    }
    const Bytes prefix(full.begin(), full.begin() + full.size() / 2);
    auto fresh = [](const Bytes& data) {
        nj2k_codec_t* codec = memory_codec(0);
        nj2k_decoder_set_strict_mode(codec, NJ2K_FALSE);
        MemoryImage result = memory_decode(codec, data, data.size(), NJ2K_SAMPLE_U8, Layout::Guarded);
        nj2k_destroy_codec(codec);
        return result;
    };
    const MemoryImage complete = fresh(full), partial = fresh(prefix);
    CHECK(complete.ok && partial.ok && complete.guards_ok && partial.guards_ok);
    CHECK(complete.buffer != partial.buffer);
    nj2k_codec_t* codec = memory_codec(0);
    nj2k_decoder_set_strict_mode(codec, NJ2K_FALSE);
    nj2k_decoder_set_retained_bytes(codec, 128ull << 20);
    for (const Bytes* data : {&full, &prefix, &full}) {
        const MemoryImage actual = memory_decode(codec, *data, data->size(), NJ2K_SAMPLE_U8, Layout::Guarded);
        const MemoryImage& expected = data == &prefix ? partial : complete;
        CHECK(actual.ok == expected.ok && actual.status == expected.status && actual.guards_ok);
        CHECK(actual.buffer == expected.buffer);
    }
    nj2k_destroy_codec(codec);
}

// A component selection made after nj2k_read_header_memory() holds for the decodes of that header:
// a plane for a component not selected is refused, the selected one is decoded (the alpha of an
// RGBA texture: as in the whole decode, which no colour transform touches). The next header read
// starts without one, of the same bytes (the header model held) or of others.
void test_memory_selection()
{
    const Bytes a = corpus_file("sl_128_rgba_97.j2k"), b = corpus_file("sl_64_rgb_97.j2k");
    if (a.empty() || b.empty()) {
        return;
    }
    auto oracle = make_oracle_backend();
    Request rq;
    rq.data = a.data();
    rq.size = a.size();
    rq.capture = true;
    const Outcome o = oracle->decode(rq);
    CHECK(o.ok && o.numcomps == 4);
    nj2k_codec_t* codec = memory_codec(0);
    nj2k_image_t image;
    CHECK(nj2k_read_header_memory(codec, a.data(), a.size(), &image) && image.numcomps == 4);
    const NJ2K_UINT32 alpha = 3;
    CHECK(nj2k_set_decoded_components(codec, 1, &alpha, NJ2K_FALSE));
    const std::uint32_t w = image.comps[3].w, h = image.comps[3].h;
    std::vector<std::int32_t> planes(std::size_t(w) * h * 2, -7);
    const nj2k_plane_t both[2] = {{planes.data(), std::int64_t(w) * 4, 4, 0},
                                  {planes.data() + std::size_t(w) * h, std::int64_t(w) * 4, 4, 3}};
    for (int round = 0; round < 2; ++round) {  // the second from the header the codec holds
        const nj2k_output_t refused{sizeof(nj2k_output_t), NJ2K_SAMPLE_I32, 2, both};
        CHECK_MSG(!nj2k_decode_memory(codec, a.data(), a.size(), &refused) && nj2k_get_status(codec) == NJ2K_E_OUTPUT,
                  "round %d: a plane of a component not selected", round);
        const nj2k_output_t out{sizeof(nj2k_output_t), NJ2K_SAMPLE_I32, 1, both + 1};
        CHECK_MSG(nj2k_decode_memory(codec, a.data(), a.size(), &out), "round %d: %s", round,
                  nj2k_status_string(nj2k_get_status(codec)));
        CHECK(o.ok && o.planes.size() == 4 &&
              std::equal(o.planes[3].begin(), o.planes[3].end(), planes.begin() + std::ptrdiff_t(w) * h));
        CHECK(std::all_of(planes.begin(), planes.begin() + std::ptrdiff_t(w) * h, [](std::int32_t v) { return v == -7; }));
    }
    // the same bytes read again: a new codestream, every component decoded
    CHECK(nj2k_read_header_memory(codec, a.data(), a.size(), &image) && image.numcomps == 4);
    {
        const nj2k_output_t out{sizeof(nj2k_output_t), NJ2K_SAMPLE_I32, 2, both};
        CHECK_MSG(nj2k_decode_memory(codec, a.data(), a.size(), &out), "the header read again: %s",
                  nj2k_status_string(nj2k_get_status(codec)));
        CHECK(o.ok && o.planes.size() == 4 && std::equal(o.planes[0].begin(), o.planes[0].end(), planes.begin()) &&
              std::equal(o.planes[3].begin(), o.planes[3].end(), planes.begin() + std::ptrdiff_t(w) * h));
    }
    // another header: every component again
    CHECK(nj2k_read_header_memory(codec, b.data(), b.size(), &image) && image.numcomps == 3);
    std::vector<std::int32_t> rgb(std::size_t(image.comps[0].w) * image.comps[0].h);
    const nj2k_plane_t first{rgb.data(), std::int64_t(image.comps[0].w) * 4, 4, 0};
    const nj2k_output_t out{sizeof(nj2k_output_t), NJ2K_SAMPLE_I32, 1, &first};
    CHECK(nj2k_decode_memory(codec, b.data(), b.size(), &out));
    nj2k_destroy_codec(codec);
}

// The working memory of a tile whose own COD changes it (tiny precincts here) is checked with the
// tile's parameters before the tile is allocated: with the limit at what the main header needs, the
// file decodes, and the same tile with its own coding is refused.
void test_tile_memory_limit()
{
    const Bytes in = corpus_file("sl_64_rgb_97.j2k");
    const auto mh = in.empty() ? std::nullopt : parse_main_header(in);
    if (!mh) {
        return;
    }
    std::uint64_t need = 0;
    {
        nj2k::detail::Allocator sys = nj2k::detail::Allocator::system();
        nj2k::detail::Arena arena;
        arena.init(&sys);
        nj2k::detail::Messages msg;
        nj2k::detail::HeaderSettings hs{};
        nj2k::detail::Source src(in.data(), in.size());
        nj2k::detail::MainHeader* model = nullptr;
        if (nj2k::detail::read_main_header(src, arena, msg, hs, model) == NJ2K_OK) {
            need = nj2k::detail::decode_working_memory(*model, 0, nullptr);
        }
        arena.release_all();
    }
    CHECK(need > 0);
    // COD with precincts of 2 x 2 at every resolution, the main header's coding otherwise
    Bytes cod = {0xFF, 0x52, 0x00, std::uint8_t(12 + mh->levels + 1), 0x01, 0x00, 0x00, 0x01, mh->mct, mh->levels,
                 mh->cblk_w_exp, mh->cblk_h_exp, 0x00, mh->transform};
    cod.insert(cod.end(), std::size_t(mh->levels) + 1, std::uint8_t(0x11));
    const Bytes cs = one_tile_part(in, cod, {0x00, 0x00, 0x00, 0x00});
    nj2k_decoder_limits_t limits{};
    limits.max_memory = need;
    for (int own = 0; own < 2; ++own) {
        const Bytes& file = own ? cs : in;
        nj2k_codec_t* codec = memory_codec(0);
        CHECK(nj2k_decoder_set_limits(codec, &limits));
        const MemoryImage m = memory_decode(codec, file, file.size(), NJ2K_SAMPLE_U8, Layout::Guarded);
        if (own) {
            CHECK_MSG(!m.ok && m.status == NJ2K_E_LIMIT && m.guards_ok, "the tile's own coding: %s",
                      nj2k_status_string(m.status));
        } else {
            CHECK_MSG(m.ok && m.guards_ok, "the main header's coding: %s", nj2k_status_string(m.status));
        }
        nj2k_destroy_codec(codec);
    }
}

// DP-15: a JP2 file's codestream ends where its box ends. A last tile-part of Psot = 0 followed by
// another box decodes whole (NJ2K_OK, OpenJPEG's image); a codestream without EOC followed by a box
// decodes as that codestream alone (OpenJPEG reads the box as the marker after the tile and fails).
void test_dp15()
{
    auto oracle = make_oracle_backend();
    auto neo = make_neoj2k_backend();
    const Bytes trailing = {0, 0, 0, 16, 0x66, 0x72, 0x65, 0x65, 1, 2, 3, 4, 5, 6, 7, 8};  // 'free'
    auto decode = [](Backend& b, const Bytes& data, bool jp2) {
        Request rq;
        rq.data = data.data();
        rq.size = data.size();
        rq.jp2 = jp2;
        return b.decode(rq);
    };
    auto memory = [](const Bytes& data, bool jp2) {
        nj2k_codec_t* codec = nj2k_create_decompress(jp2 ? NJ2K_CODEC_JP2 : NJ2K_CODEC_J2K);
        nj2k_decoder_set_strict_mode(codec, NJ2K_FALSE);
        const MemoryImage m = memory_decode(codec, data, data.size(), NJ2K_SAMPLE_I32, Layout::Guarded);
        nj2k_destroy_codec(codec);
        return m;
    };
    {
        const Bytes cs = corpus_file("psot0_sop_97.j2k");
        const auto mh = cs.empty() ? std::nullopt : parse_main_header(cs);
        if (mh) {
            const Bytes jp2 = jp2_file(cs, mh->width(), mh->height(), trailing);
            const Outcome bare = decode(*neo, cs, false), a = decode(*oracle, jp2, true), b = decode(*neo, jp2, true);
            CHECK_MSG(bare.ok && a.ok && b.ok && a.hash == b.hash, "Psot = 0 in a JP2 file with a box after it");
            const MemoryImage m = memory(jp2, true);
            CHECK_MSG(m.ok && m.status == NJ2K_OK, "Psot = 0 in a JP2 file: %s", nj2k_status_string(m.status));
        }
    }
    {
        Bytes cs = corpus_file("size_16x16_rgb_r5.j2k");
        const auto mh = cs.size() > 2 ? parse_main_header(cs) : std::nullopt;
        if (mh) {
            cs.resize(cs.size() - 2);  // no EOC
            const Bytes jp2 = jp2_file(cs, mh->width(), mh->height(), trailing);
            const Outcome bare = decode(*neo, cs, false), a = decode(*oracle, jp2, true), b = decode(*neo, jp2, true);
            CHECK_MSG(!a.ok, "the oracle read the box after a codestream without EOC");
            CHECK_MSG(bare.ok && b.ok && b.hash != 0, "no EOC in a JP2 file with a box after it");
            const MemoryImage m = memory(jp2, true), n = memory(cs, false);
            CHECK_MSG(m.ok && m.status == NJ2K_TRUNCATED && n.ok && m.buffer == n.buffer, "no EOC: %s",
                      nj2k_status_string(m.status));
        }
    }
}

// DP-20: the packet walk stops once a tile's data is exhausted in strict mode too (the oracle stops
// only in the non-strict modes). Every prefix of codestreams with EPH markers, with SOP markers, and
// with both, in strict mode: a cut at a packet boundary leaves packets to read from no bytes, and
// neo-j2k's decode (refused or not) is the oracle's, with the same image.
void test_dp20()
{
    auto oracle = make_oracle_backend();
    auto neo = make_neoj2k_backend();
    std::size_t cuts = 0, decoded = 0;
    for (const char* name : {"mark_eph.j2k", "mark_sop.j2k", "mark_sop_eph_97.j2k"}) {
        const Bytes cs = corpus_file(name);
        for (std::size_t n = 1; n <= cs.size(); ++n) {
            Request rq;
            rq.data = cs.data();
            rq.size = n;
            rq.mode = Mode::Strict;
            const Outcome a = oracle->decode(rq), b = neo->decode(rq);
            ++cuts;
            decoded += a.ok ? 1 : 0;
            CHECK_MSG(a.ok == b.ok && (!a.ok || a.hash == b.hash), "%s cut at %zu of %zu in strict mode: oracle %s, neo-j2k %s",
                      name, n, cs.size(), a.ok ? "decodes" : "refuses", b.ok ? "decodes" : "refuses");
        }
    }
    CHECK_MSG(cuts > 0 && decoded > 0, "DP-20: %zu cuts, %zu decoded", cuts, decoded);
}

// The viewer's path in CI: one codec for every file and reduce level (its header held while the main
// header is the same), the Second Life limits, NJ2K_REDUCE_CLAMP, non-strict, 8-bit planes with guard
// bytes, sized once nj2k_get_decoded_dimensions() accepts the header: every decode equals the
// oracle's at the reduce factor in effect, and nothing outside the planes is written.
void test_viewer_path()
{
    auto oracle = make_oracle_backend();
    nj2k_codec_t* codec = viewer_codec(0, NJ2K_REDUCE_CLAMP);
    std::size_t decodes = 0, refused = 0;
    for (const auto& path : expand_inputs(g_corpus)) {
        const std::string name = base_name(path);
        const auto data = read_file(path);
        if (!data || name == "edge_ppt_empty_tiles_97.j2k") {
            continue;  // DP-3 above
        }
        for (int r = 0; r <= 5; ++r) {
            nj2k_set_decoded_resolution_factor(codec, static_cast<NJ2K_UINT32>(r));  // false before a header
            nj2k_image_t image;
            if (!nj2k_read_header_memory(codec, data->data(), data->size(), &image) ||
                !nj2k_get_decoded_dimensions(codec, &image)) {
                CHECK_MSG(nj2k_get_status(codec) == NJ2K_E_LIMIT, "%s reduce %d: %s", name.c_str(), r,
                          nj2k_status_string(nj2k_get_status(codec)));
                ++refused;
                continue;
            }
            Request rq;
            rq.data = data->data();
            rq.size = data->size();
            rq.reduce = static_cast<int>(image.comps[0].factor);
            rq.capture = true;
            const Outcome o = oracle->decode(rq);
            const MemoryImage m = memory_decode(codec, *data, data->size(), NJ2K_SAMPLE_U8, Layout::Guarded);
            std::string why;
            CHECK_MSG(equal_to_oracle(m, o, NJ2K_SAMPLE_U8, why) && m.guards_ok, "%s reduce %d (factor %d): %s%s",
                      name.c_str(), r, rq.reduce, why.c_str(), m.guards_ok ? "" : ", guards written");
            ++decodes;
        }
    }
    nj2k_destroy_codec(codec);
    std::printf("viewer path: %zu decodes on one codec equal to the oracle's, %zu refused by the limits\n", decodes,
                refused);
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
// asks the codec's allocator for nothing, and nothing calls operator new. The codec clamps the reduce
// factor as the viewer's does, so every file decodes at every level: each decode must succeed.
void test_zero_heap()
{
    std::vector<std::pair<std::string, Bytes>> files;
    for (const auto& path : expand_inputs(g_corpus)) {
        auto data = read_file(path);
        const auto mh = data ? parse_main_header(*data) : std::nullopt;
        if (mh && mh->width() <= 256 && mh->height() <= 256) {
            files.emplace_back(base_name(path), std::move(*data));
        }
    }
    CHECK(files.size() >= 20);
    for (int r = 0; r <= 1; ++r) {
        Counting counting;
        const nj2k_allocator_t hook = counting.hook();
        nj2k_codec_t* codec = memory_codec(r, &hook);
        CHECK(codec != nullptr && nj2k_decoder_set_reduce_policy(codec, NJ2K_REDUCE_CLAMP));
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
                CHECK_MSG(false, "%s reduce %d: header refused (%s)", name.c_str(), r,
                          nj2k_status_string(nj2k_get_status(codec)));
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
            CHECK_MSG(ok, "%s reduce %d: a repeat decode failed (%s)", name.c_str(), r,
                      nj2k_status_string(nj2k_get_status(codec)));
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
                CHECK_MSG(false, "%s reduce %d: header refused in turn", name.c_str(), r);
                continue;
            }
            std::vector<std::uint8_t> raw;
            std::vector<nj2k_plane_t> planes;
            const nj2k_output_t out = u8_planes(image, raw, planes);
            g_news = 0;
            g_count_new = true;
            const bool ok = nj2k_decode_memory(codec, data.data(), data.size(), &out) != NJ2K_FALSE;
            g_count_new = false;
            news += g_news;
            CHECK_MSG(ok, "%s reduce %d: the decode in turn failed (%s)", name.c_str(), r,
                      nj2k_status_string(nj2k_get_status(codec)));
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

// The zero-heap gate per request, as the bench counts it: the viewer's request (neo-j2k's own path,
// one codec kept across every request, the Second Life limits, the header at reduce 0 then at the
// viewer's factor, the decode into its 8-bit bottom-up image), every file whose sides are at most
// max_side, at every reduce level and as the viewer cuts it (the bytes it asks for at that level,
// the first 600 bytes), each request taken once and then again: the repeat asks the allocator for
// nothing and calls no operator new, whatever was decoded before it. Without the end-of-decode
// merge of the tile-part arena (Decoder::trim_after_decode), the repeat of a request whose tile-part
// headers held more markers than any request before it made one allocation and two frees.
void test_zero_heap_requests(const std::string& inputs, std::uint32_t max_side, const char* what)
{
    auto backend = make_neoj2k_memory_backend();
    std::size_t files = 0, requests = 0, decoded = 0, failures = 0;
    for (const auto& path : expand_inputs(inputs)) {
        const auto data = read_file(path);
        const auto mh = data ? parse_main_header(*data) : std::nullopt;
        if (!mh || mh->width() > max_side || mh->height() > max_side) {
            continue;
        }
        ++files;
        const int grid_w = viewer::grid_side(mh->tiles_x(), mh->xtsiz), grid_h = viewer::grid_side(mh->tiles_y(), mh->ytsiz);
        for (int r = 0; r <= 5; ++r) {
            std::size_t sizes[3] = {data->size(), 0, 0};
            if (r > 0) {
                const auto b = viewer::data_size_j2c(grid_w, grid_h, r);
                sizes[1] = b > 0 && std::uint64_t(b) < data->size() ? std::size_t(b) : 0;
            }
            if (r == 5 && data->size() > viewer::kFirstRequestBytes) {
                sizes[2] = viewer::kFirstRequestBytes;
            }
            for (const std::size_t size : sizes) {
                if (size == 0) {
                    continue;
                }
                Request rq;
                rq.data = data->data();
                rq.size = size;
                rq.reduce = r;
                rq.mode = Mode::Viewer;
                rq.inspect = false;
                const Outcome first = backend->decode(rq);
                const bool first_ok = first.ok;
                // the whole file decodes at every level (the viewer's codec clamps the reduce
                // factor), so the gate cannot pass on requests that were all refused; the viewer
                // takes no image whose components differ in size (sub_420.j2k), refused before the decode
                CHECK_MSG(first_ok || size != data->size() || first.stage == "geometry",
                          "%s %s reduce %d: the whole file was refused (%s)", what, base_name(path).c_str(), r,
                          first.stage.c_str());
                backend->alloc_reset();
                g_news = 0;
                g_count_new = true;
                const bool ok = backend->decode(rq).ok;
                g_count_new = false;
                std::uint64_t counts[4] = {};
                backend->alloc_counts(counts);
                ++requests;
                decoded += ok;
                CHECK_MSG(ok == first_ok, "%s %s reduce %d, %zu bytes: the repeat's outcome differs", what,
                          base_name(path).c_str(), r, size);
                CHECK_MSG(counts[0] == 0 && counts[2] == 0, "%s %s reduce %d, %zu bytes: %llu allocations, %llu frees on the repeat",
                          what, base_name(path).c_str(), r, size, static_cast<unsigned long long>(counts[0]),
                          static_cast<unsigned long long>(counts[2]));
#if NJ2K_TEST_COUNT_NEW
                CHECK_MSG(g_news == 0, "%s %s reduce %d, %zu bytes: %zu operator new on the repeat", what,
                          base_name(path).c_str(), r, size, g_news.load());
#endif
                failures += (counts[0] + counts[2]) != 0;
            }
        }
    }
    CHECK_MSG(files > 0, "%s: no file up to %u x %u", what, max_side, max_side);
    CHECK_MSG(decoded > 0, "%s: no request decoded", what);
    std::printf("zero heap per request, %s: %zu files up to %u x %u, %zu requests repeated (%zu decoded), %zu with heap "
                "calls\n",
                what, files, max_side, max_side, requests, decoded, failures);
}

// ---- stage 5: threads in one image ----------------------------------------------------------------

// What a decode gives, for comparing one thread with many: the outcome, the bytes written (the
// planes' buffer, or a hash of it for large images) and every message in order.
struct ThreadedOutcome {
    bool ok = false;
    nj2k_status_t status = NJ2K_OK;
    std::uint64_t hash = 0;
    std::vector<std::uint8_t> buffer;
    std::string messages;
};

void collect_info(const char* m, void* user)
{
    *static_cast<std::string*>(user) += std::string("I:") + m;
}
void collect_warning(const char* m, void* user)
{
    *static_cast<std::string*>(user) += std::string("W:") + m;
}
void collect_error(const char* m, void* user)
{
    *static_cast<std::string*>(user) += std::string("E:") + m;
}

// A memory codec (memory_codec) with its messages collected into messages, through pool's executor
// at the given concurrency (no executor below 2), with every threshold at 0 when forced (so the
// corpus's small files take every threaded path) or the library's own sizes otherwise.
nj2k_codec_t* threaded_codec(int reduce, nj2k::test::ThreadPool* pool, std::uint32_t concurrency, bool forced,
                             std::string& messages)
{
    nj2k_codec_t* codec = memory_codec(reduce);
    if (!codec) {
        return nullptr;
    }
    nj2k_set_info_handler(codec, collect_info, &messages);
    nj2k_set_warning_handler(codec, collect_warning, &messages);
    nj2k_set_error_handler(codec, collect_error, &messages);
    if (pool && concurrency >= 2) {
        const nj2k_executor_t ex = pool->executor(concurrency);
        if (!nj2k_decoder_set_executor(codec, &ex)) {
            nj2k_destroy_codec(codec);
            return nullptr;
        }
    }
    if (forced && !nj2k::detail::set_parallel_thresholds(codec, 0, 0, 0, 0)) {
        nj2k_destroy_codec(codec);
        return nullptr;
    }
    return codec;
}

ThreadedOutcome threaded_decode(nj2k_codec_t* codec, const Bytes& data, std::size_t size, std::uint32_t type,
                                Layout layout, std::string& messages, bool keep)
{
    messages.clear();
    MemoryImage m = memory_decode(codec, data, size, type, layout);
    ThreadedOutcome o;
    o.ok = m.ok;
    o.status = m.status;
    Fnv64 h;
    h.bytes(m.buffer.data(), m.buffer.size());
    o.hash = h.value();
    if (keep) {
        o.buffer = std::move(m.buffer);
    }
    o.messages = messages;
    return o;
}

bool same_outcome(const ThreadedOutcome& a, const ThreadedOutcome& b, std::string& why)
{
    if (a.ok != b.ok || a.status != b.status) {
        why = "status " + std::to_string(a.status) + " against " + std::to_string(b.status);
        return false;
    }
    if (a.hash != b.hash || a.buffer != b.buffer) {
        why = "samples differ";
        return false;
    }
    if (a.messages != b.messages) {
        why = "messages differ: [" + a.messages + "] against [" + b.messages + "]";
        return false;
    }
    return true;
}

// 64: the most parts a stage is cut into (kMaxJobs), and 128 Tier-1 runs
constexpr std::uint32_t kThreadCounts[] = {2, 4, 8, 16, 64};

// One file at every reduce level, whole and cut, in the int32 planes and the viewer's 8-bit image:
// one thread against each count of kThreadCounts (forced: every stage through the executor, however
// small), the samples, the status and every message the same. Large images are compared by hash.
std::size_t threads_file(nj2k::test::ThreadPool& pool, const std::string& name, const Bytes& data, bool forced,
                         int max_reduce, bool cuts)
{
    std::size_t decodes = 0;
    const bool keep = data.size() < (1u << 20);
    for (int r = 0; r <= max_reduce; ++r) {
        std::vector<std::size_t> sizes{data.size()};
        if (cuts) {
            for (const std::size_t n : {std::size_t(600), data.size() / 7, data.size() / 3, data.size() * 2 / 3,
                                        data.size() - 1}) {
                if (n > 0 && n < data.size()) {
                    sizes.push_back(n);
                }
            }
        }
        for (const std::size_t size : sizes) {
            for (const std::uint32_t type : {NJ2K_SAMPLE_I32, NJ2K_SAMPLE_U8}) {
                const Layout layout = type == NJ2K_SAMPLE_I32 ? Layout::Guarded : Layout::BottomUpInterleaved;
                std::string ms;
                nj2k_codec_t* one = threaded_codec(r, nullptr, 1, false, ms);
                CHECK(one != nullptr);
                if (!one) {
                    return decodes;
                }
                const ThreadedOutcome ref = threaded_decode(one, data, size, type, layout, ms, keep);
                nj2k_destroy_codec(one);
                for (const std::uint32_t n : kThreadCounts) {
                    std::string mt;
                    nj2k_codec_t* many = threaded_codec(r, &pool, n, forced, mt);
                    CHECK(many != nullptr);
                    if (!many) {
                        return decodes;
                    }
                    // twice from the same codec: the second decode reuses what the first kept
                    for (int pass = 0; pass < 2; ++pass) {
                        const ThreadedOutcome o = threaded_decode(many, data, size, type, layout, mt, keep);
                        std::string why;
                        CHECK_MSG(same_outcome(ref, o, why),
                                  "%s reduce %d, %zu of %zu bytes, type %u, %u threads, pass %d: %s", name.c_str(), r,
                                  size, data.size(), type, n, pass, why.c_str());
                        ++decodes;
                    }
                    nj2k_destroy_codec(many);
                }
            }
        }
    }
    return decodes;
}

// The same through the stream API (nj2k_decode into the image's own planes, the compatibility API's
// way): one thread against every count, the planes, the status and every message the same.
ThreadedOutcome stream_decode(const Bytes& data, std::size_t size, int reduce, nj2k::test::ThreadPool* pool,
                              std::uint32_t concurrency, bool forced)
{
    ThreadedOutcome o;
    nj2k_codec_t* codec = nj2k_create_decompress(NJ2K_CODEC_J2K);
    if (!codec) {
        return o;
    }
    nj2k_set_info_handler(codec, collect_info, &o.messages);
    nj2k_set_warning_handler(codec, collect_warning, &o.messages);
    nj2k_set_error_handler(codec, collect_error, &o.messages);
    nj2k_dparameters_t p;
    nj2k_set_default_decoder_parameters(&p);
    p.cp_reduce = static_cast<NJ2K_UINT32>(reduce);
    bool ready = nj2k_setup_decoder(codec, &p) && nj2k_decoder_set_strict_mode(codec, NJ2K_FALSE);
    if (ready && pool && concurrency >= 2) {
        const nj2k_executor_t ex = pool->executor(concurrency);
        ready = nj2k_decoder_set_executor(codec, &ex) &&
                (!forced || nj2k::detail::set_parallel_thresholds(codec, 0, 0, 0, 0));
    }
    CHECK(ready);
    nj2k::MemoryReader reader(std::span<const std::uint8_t>(data.data(), size));
    auto stream = reader.make_stream();
    nj2k_image_t* image = nullptr;
    o.ok = ready && stream && nj2k_read_header(stream.get(), codec, &image) &&
           nj2k_decode(codec, stream.get(), image) && nj2k_end_decompress(codec, stream.get());
    o.status = nj2k_get_status(codec);
    Fnv64 h;
    for (std::uint32_t c = 0; image && c < image->numcomps; ++c) {
        const nj2k_image_comp_t& ic = image->comps[c];
        if (ic.data) {
            h.bytes(ic.data, std::size_t(ic.w) * ic.h * sizeof(std::int32_t));
        }
    }
    o.hash = h.value();
    nj2k_image_destroy(image);
    nj2k_destroy_codec(codec);
    return o;
}

std::size_t threads_stream_file(nj2k::test::ThreadPool& pool, const std::string& name, const Bytes& data, bool forced,
                                int max_reduce)
{
    std::size_t decodes = 0;
    for (int r = 0; r <= max_reduce; ++r) {
        for (const std::size_t size : {data.size(), data.size() * 2 / 3}) {
            if (size == 0) {
                continue;
            }
            const ThreadedOutcome ref = stream_decode(data, size, r, nullptr, 1, false);
            for (const std::uint32_t n : kThreadCounts) {
                const ThreadedOutcome o = stream_decode(data, size, r, &pool, n, forced);
                std::string why;
                CHECK_MSG(same_outcome(ref, o, why),
                          "%s through the stream API, reduce %d, %zu of %zu bytes, %u threads: %s", name.c_str(), r,
                          size, data.size(), n, why.c_str());
                ++decodes;
            }
        }
    }
    return decodes;
}

// An allocator hook that refuses what would take its live bytes above cap.
struct Capped {
    std::size_t cap = SIZE_MAX, live = 0;
    Counting counting;

    static void* alloc(void* user, NJ2K_SIZE_T size, NJ2K_SIZE_T alignment)
    {
        auto* c = static_cast<Capped*>(user);
        if (size > c->cap - c->live) {
            return nullptr;
        }
        void* p = Counting::alloc(&c->counting, size, alignment);
        if (p) {
            c->live += size;
        }
        return p;
    }
    static void release(void* user, void* ptr, NJ2K_SIZE_T size)
    {
        auto* c = static_cast<Capped*>(user);
        c->live -= size;
        Counting::release(&c->counting, ptr, size);
    }
    nj2k_allocator_t hook() { return nj2k_allocator_t{sizeof(nj2k_allocator_t), alloc, release, this}; }
};

// One decode under an allocator capped at cap bytes, on one thread or through pool (every stage
// forced); peak: the most it held.
ThreadedOutcome capped_decode(const Bytes& data, std::size_t cap, nj2k::test::ThreadPool* pool, std::size_t* peak)
{
    Capped capped;
    capped.cap = cap;
    const nj2k_allocator_t hook = capped.hook();
    std::string messages;
    ThreadedOutcome o;
    nj2k_codec_t* codec = memory_codec(0, &hook);
    if (!codec) {
        o.status = NJ2K_E_NO_MEMORY;  // the codec itself did not fit
        return o;
    }
    nj2k_set_info_handler(codec, collect_info, &messages);
    nj2k_set_warning_handler(codec, collect_warning, &messages);
    nj2k_set_error_handler(codec, collect_error, &messages);
    if (pool) {
        const nj2k_executor_t ex = pool->executor(4);
        CHECK(nj2k_decoder_set_executor(codec, &ex) && nj2k::detail::set_parallel_thresholds(codec, 0, 0, 0, 0));
    }
    o = threaded_decode(codec, data, data.size(), NJ2K_SAMPLE_I32, Layout::Guarded, messages, true);
    nj2k_destroy_codec(codec);
    if (peak) {
        *peak = capped.counting.peak;
    }
    return o;
}

// A decode through the executor needs memory one thread does not (a Tier-1 scratch a run, the
// code-blocks' costs, a wavelet line a part); when the allocator refuses it, the tile falls back to
// one thread with that memory given back, so the outcome is one thread's at every cap: below the
// least one thread decodes with, at it, and up past what the threads need.
std::size_t threads_capped(nj2k::test::ThreadPool& pool, const std::string& name, const Bytes& data)
{
    std::size_t one_peak = 0, many_peak = 0;
    const ThreadedOutcome free_one = capped_decode(data, SIZE_MAX, nullptr, &one_peak);
    (void)capped_decode(data, SIZE_MAX, &pool, &many_peak);
    if (!free_one.ok) {
        return 0;  // a file that does not decode: no least memory to find
    }
    // the least cap one thread decodes with
    std::size_t lo = 0, hi = one_peak;
    while (lo + 1 < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        (capped_decode(data, mid, nullptr, nullptr).ok ? hi : lo) = mid;
    }
    const std::size_t least = hi;
    std::vector<std::size_t> caps{least - 1, least - least / 64, least};
    const std::size_t extra = many_peak > least ? many_peak - least : 0;
    for (std::size_t k = 1; k <= 16; ++k) {
        caps.push_back(least + extra * k / 16);
    }
    caps.push_back(least + extra + 4096);
    std::size_t decodes = 0;
    for (const std::size_t cap : caps) {
        const ThreadedOutcome one = capped_decode(data, cap, nullptr, nullptr);
        const ThreadedOutcome many = capped_decode(data, cap, &pool, nullptr);
        std::string why;
        CHECK_MSG(same_outcome(one, many, why), "%s under a cap of %zu bytes (one thread decodes from %zu): %s",
                  name.c_str(), cap, least, why.c_str());
        decodes += 2;
    }
    return decodes;
}

// A grey image of side x side in tiles of tile x tile (the oracle's encoder). With side up to 2,048 the
// image is below the tile floor, so only a plane clear of kParallelClearBytes or more may call the
// executor; a larger side with tiles of kParallelTileFloor samples or more is threaded tile by tile.
Bytes small_tiles_image(std::uint32_t side, std::uint32_t tile)
{
    EncodeSpec e;
    e.width = side;
    e.height = side;
    e.dx = {1};
    e.dy = {1};
    e.tile = tile;
    e.reversible = true;
    e.numres = 4;
    std::vector<std::uint8_t> plane(std::size_t(side) * side);
    for (std::uint32_t y = 0; y < side; ++y) {
        for (std::uint32_t x = 0; x < side; ++x) {
            plane[std::size_t(y) * side + x] = static_cast<std::uint8_t>(x * 3 + y * 5 + ((x / 11 + y / 7) & 1) * 70);
        }
    }
    e.planes.push_back(std::move(plane));
    Bytes data;
    CHECK_MSG(oracle_encode(e, data), "the oracle's encoder refused %ux%u in %u tiles", side, side, tile);
    return data;
}

// The images of test_large_generated, made by the oracle's encoder: five of more than 2^22 samples a
// component, and a 1,024 x 1,024 RGB one (the last), all above the tile floor, so no forcing
std::vector<std::pair<std::string, Bytes>> large_threaded_images()
{
    auto oracle = make_oracle_backend();
    const struct {
        int w, h, comps;
        bool reversible;
    } images[] = {{2051, 2051, 3, true}, {2049, 2050, 1, true}, {2100, 2000, 1, true}, {2051, 2052, 3, false},
                  {2053, 2049, 1, false}, {1024, 1024, 3, true}};
    std::vector<std::pair<std::string, Bytes>> out;
    std::mt19937 rng(20261120);
    for (const auto& im : images) {
        std::vector<std::uint8_t> raw(std::size_t(im.w) * im.h * im.comps);
        for (int y = 0; y < im.h; ++y) {
            for (int x = 0; x < im.w; ++x) {
                for (int c = 0; c < im.comps; ++c) {
                    const int edge = ((x / 97 + y / 61 + c) & 1) * 60;
                    const int v = (x * (c + 1) / 9 + y / (c + 2) + edge + static_cast<int>(rng() % 23)) & 0xFF;
                    raw[(std::size_t(y) * im.w + x) * im.comps + c] = static_cast<std::uint8_t>(v);
                }
            }
        }
        Bytes data;
        CHECK_MSG(oracle->encode_upload(raw.data(), im.w, im.h, im.comps, im.reversible, data),
                  "the oracle's encoder refused %dx%d", im.w, im.h);
        out.emplace_back(std::to_string(im.w) + "x" + std::to_string(im.h) + "x" + std::to_string(im.comps) +
                             (im.reversible ? " 5/3" : " 9/7"),
                         std::move(data));
    }
    return out;
}

// Gate (1) of stage 5: the corpus, every stage forced through the executor; the large images at the
// library's own thresholds, whole and cut; one large image decoded again and again at 8 threads
// (timing-dependent faults); several decoders sharing one pool at once, as a host's decode threads
// would (no deadlock, the same samples); and below the threshold, a codec with an executor never
// calls it (small images pay nothing).
void test_threads(const std::string& files)
{
    nj2k::test::ThreadPool pool(15);
    std::size_t decodes = 0, corpus_files = 0;
    for (const auto& path : expand_inputs(g_corpus)) {
        const auto data = read_file(path);
        if (!data) {
            continue;
        }
        ++corpus_files;
        decodes += threads_file(pool, base_name(path), *data, true, 5, data->size() <= 65536);
    }
    std::printf("threads: %zu corpus files, %zu threaded decodes (forced) equal to one thread's\n", corpus_files,
                decodes);

    const auto large = large_threaded_images();
    std::size_t large_decodes = 0;
    for (const auto& [name, data] : large) {
        large_decodes += threads_file(pool, name, data, false, 2, true);
    }
    std::printf("threads: %zu large generated images, %zu threaded decodes equal to one thread's\n", large.size(),
                large_decodes);

    // With an executor lent, the tile decides (the host decides whether to lend one): no call for an
    // image whose tiles have fewer than 2^18 samples in their largest component and whose planes are
    // below the clear threshold (the corpus, decoded into the viewer's 8-bit image and into int32
    // planes, 512 x 512 grey in 4 and in 16 tiles, and single-tile 511 x 511 and 512 x 511 grey),
    // but calls for 1,024 x 1,024 grey in 256 tiles of 64 x 64 into int32 planes (a 4 MiB clear), for a
    // single tile of 512 x 512 (exactly the floor), for 1,024 x 1,024 and 2,051 x 2,051 RGB in one tile,
    // and for a 2,112 x 2,112 grey image in four tiles of 1,056 (each above the floor)
    const Bytes tiles512_4 = small_tiles_image(512, 256), tiles512_16 = small_tiles_image(512, 128);
    const Bytes tiles1024 = small_tiles_image(1024, 64);
    const Bytes tiles2112 = small_tiles_image(2112, 1056);
    const Bytes one511 = small_tiles_image(511, 511), one512 = small_tiles_image(512, 512);
    {
        std::string m;
        nj2k_codec_t* codec = threaded_codec(0, &pool, 8, false, m);
        CHECK(codec != nullptr);
        const std::uint64_t before = pool.batches();
        std::size_t below = 0;
        auto small = [&](const Bytes& data, std::uint64_t tile_side = 0) {
            const auto mh = parse_main_header(data);
            if (!codec || !mh) {
                return;
            }
            // a tile's largest component (a single-tile image's own)
            const std::uint64_t tile = tile_side ? tile_side * tile_side : std::uint64_t(mh->width()) * mh->height();
            const std::uint64_t plane = mh->width() * mh->height() * sizeof(std::int32_t);
            if (tile >= nj2k::detail::kParallelTileFloor || plane >= nj2k::detail::kParallelClearBytes) {
                return;
            }
            (void)threaded_decode(codec, data, data.size(), NJ2K_SAMPLE_U8, Layout::BottomUpInterleaved, m, false);
            (void)threaded_decode(codec, data, data.size(), NJ2K_SAMPLE_I32, Layout::Guarded, m, false);
            ++below;
        };
        for (const auto& path : expand_inputs(g_corpus)) {
            if (const auto data = read_file(path)) {
                small(*data);
            }
        }
        small(tiles512_4, 256);
        small(tiles512_16, 128);
        small(one511);
        CHECK_MSG(pool.batches() == before, "the executor ran %llu batches for %zu images below the thresholds",
                  static_cast<unsigned long long>(pool.batches() - before), below);
        (void)threaded_decode(codec, tiles1024, tiles1024.size(), NJ2K_SAMPLE_I32, Layout::Guarded, m, false);
        const std::uint64_t clear = pool.batches() - before;
        CHECK_MSG(clear > 0, "the executor did not clear the plane of a 1024 x 1024 image in 64 x 64 tiles");
        const std::uint64_t mid = pool.batches();
        (void)threaded_decode(codec, tiles2112, tiles2112.size(), NJ2K_SAMPLE_U8, Layout::BottomUpInterleaved, m,
                              false);
        CHECK_MSG(pool.batches() > mid, "the executor was not used for the tiles of a 2112 x 2112 image");
        const std::uint64_t big = pool.batches();
        (void)threaded_decode(codec, large[0].second, large[0].second.size(), NJ2K_SAMPLE_U8,
                              Layout::BottomUpInterleaved, m, false);
        CHECK_MSG(pool.batches() > big, "the executor was not used for a 2051 x 2051 RGB image");
        // the floor at the default thresholds: a single tile of 512 x 512 (2^18 samples) is threaded, one of
        // 511 x 511 is not
        const std::uint64_t edge = pool.batches();
        (void)threaded_decode(codec, one511, one511.size(), NJ2K_SAMPLE_U8, Layout::BottomUpInterleaved, m, false);
        CHECK_MSG(pool.batches() == edge, "the executor ran a batch for a single 511 x 511 tile");
        (void)threaded_decode(codec, one512, one512.size(), NJ2K_SAMPLE_U8, Layout::BottomUpInterleaved, m, false);
        CHECK_MSG(pool.batches() > edge, "the executor was not used for a single 512 x 512 tile");
        // the floor applies to the decoded size: a 1,024 x 1,024 image decoded at reduce 1 (512 x 512) is
        // threaded, at reduce 2 (256 x 256) it is not, RGB and grey alike (default thresholds)
        {
            const Bytes grey1024 = small_tiles_image(1024, 1024);
            const Bytes* images[] = {&large.back().second, &grey1024};
            for (const Bytes* img : images) {
                for (int reduce = 1; reduce <= 2; ++reduce) {
                    std::string mr;
                    nj2k_codec_t* rc = threaded_codec(reduce, &pool, 8, false, mr);
                    CHECK(rc != nullptr);
                    const std::uint64_t pre = pool.batches();
                    if (rc) {
                        (void)threaded_decode(rc, *img, img->size(), NJ2K_SAMPLE_U8, Layout::BottomUpInterleaved, mr,
                                              false);
                        nj2k_destroy_codec(rc);
                    }
                    if (reduce == 1) {
                        CHECK_MSG(pool.batches() > pre, "no batch for a 1024 x 1024 image decoded at reduce 1");
                    } else {
                        CHECK_MSG(pool.batches() == pre, "a batch for a 1024 x 1024 image decoded at reduce 2");
                    }
                }
            }
        }
        // a 1,024 x 1,024 RGB image in one tile (2^20 samples a component, above the 2^18 floor) is
        // threaded when an executor is lent
        const std::uint64_t rgb = pool.batches();
        (void)threaded_decode(codec, large.back().second, large.back().second.size(), NJ2K_SAMPLE_U8,
                              Layout::BottomUpInterleaved, m, false);
        CHECK_MSG(pool.batches() > rgb, "the executor was not used for a 1024 x 1024 RGB image");
        // and with the executor taken back (nj2k_decoder_set_executor with null), nothing is threaded at any
        // size: the pool, which counts its batches, is never called
        if (codec) {
            const std::uint64_t idle = pool.batches();
            CHECK_MSG(nj2k_decoder_set_executor(codec, nullptr), "nj2k_decoder_set_executor refused null");
            for (const Bytes* data : {&large[0].second, &large.back().second, &tiles2112}) {
                (void)threaded_decode(codec, *data, data->size(), NJ2K_SAMPLE_U8, Layout::BottomUpInterleaved, m,
                                      false);
            }
            nj2k_destroy_codec(codec);
            CHECK_MSG(pool.batches() == idle, "the pool ran a batch for a codec with no executor");
        }
        std::printf("threads: %zu images below the thresholds, no batch; a 4 MiB plane clear, %llu batches\n", below,
                    static_cast<unsigned long long>(clear));
    }
    // planes that share bytes (every component into one plane's bytes, and rows that overlap): such a
    // decode keeps to one thread (no batch) and writes what one thread writes
    {
        const Bytes& data = large[0].second;  // 2051 x 2051 RGB, one tile
        auto shared = [&](nj2k::test::ThreadPool* p, std::int64_t row, std::vector<std::uint8_t>& buf) {
            std::string m;
            nj2k_codec_t* codec = threaded_codec(0, p, 8, false, m);
            nj2k_image_t image;
            bool ok = codec && nj2k_read_header_memory(codec, data.data(), data.size(), &image);
            const std::uint32_t w = ok ? image.comps[0].w : 0, h = ok ? image.comps[0].h : 0;
            buf.assign(std::size_t(w) * 4 * h + 64, 0);
            std::vector<nj2k_plane_t> planes;
            for (std::uint32_t c = 0; ok && c < image.numcomps; ++c) {
                planes.push_back(nj2k_plane_t{buf.data(), row, 4, c});
            }
            const nj2k_output_t out{sizeof(nj2k_output_t), NJ2K_SAMPLE_I32, std::uint32_t(planes.size()), planes.data()};
            ok = ok && nj2k_decode_memory(codec, data.data(), data.size(), &out);
            if (codec) {
                nj2k_destroy_codec(codec);
            }
            return ok;
        };
        for (const std::int64_t row : {std::int64_t(1024 * 4), std::int64_t(1024 * 2)}) {
            std::vector<std::uint8_t> ref, got;
            CHECK(shared(nullptr, row, ref));
            const std::uint64_t before = pool.batches();
            CHECK(shared(&pool, row, got));
            CHECK_MSG(pool.batches() == before, "the executor ran for planes that share bytes (row stride %lld)",
                      static_cast<long long>(row));
            CHECK_MSG(ref == got, "planes that share bytes (row stride %lld): not one thread's bytes",
                      static_cast<long long>(row));
        }
    }

    // the plane clears' bands and the small tiles through the executor: one thread's outcome
    {
        const std::size_t n = threads_file(pool, "1024x1024 in 64 x 64 tiles", tiles1024, false, 1, true) +
                              threads_file(pool, "2112x2112 in 1056 x 1056 tiles", tiles2112, false, 0, true) +
                              threads_file(pool, "512x512 in 128 x 128 tiles", tiles512_16, true, 1, false);
        std::printf("threads: small-tile images, %zu threaded decodes equal to one thread's\n", n);
    }

    // the stream API (nj2k_decode into the image's planes)
    {
        std::size_t n = 0, corpus_n = 0;
        for (const auto& path : expand_inputs(g_corpus)) {
            const auto data = read_file(path);
            if (data && data->size() <= 65536) {
                n += threads_stream_file(pool, base_name(path), *data, true, 1);
                ++corpus_n;
            }
        }
        n += threads_stream_file(pool, large[0].first, large[0].second, false, 1);
        n += threads_stream_file(pool, large[4].first, large[4].second, false, 0);
        n += threads_stream_file(pool, "1024x1024 in 64 x 64 tiles", tiles1024, false, 0);
        std::printf("threads: the stream API, %zu corpus files and 3 large images, %zu threaded decodes equal to "
                    "one thread's\n",
                    corpus_n, n);
    }

    // under a capped allocator, one thread's outcome at every cap
    {
        std::size_t n = 0, files_n = 0;
        for (const char* f : {"sl_64_rgb_97.j2k", "sl_128_rgba_97.j2k", "size_17x37_rgb_97.j2k", "layers_4_53.j2k",
                              "tile_100x80_t64_97.j2k", "packed_ppt_tiles.j2k", "mark_tlm_tiles.j2k", "sub_420.j2k"}) {
            const auto data = read_file(g_corpus + "/" + f);
            CHECK_MSG(data.has_value(), "cannot read %s", f);
            if (data) {
                n += threads_capped(pool, f, *data);
                ++files_n;
            }
        }
        n += threads_capped(pool, "512x512 in 128 x 128 tiles", tiles512_16);
        std::printf("threads: %zu files under capped allocators, %zu decodes, one thread's outcome at every cap\n",
                    files_n + 1, n);
    }

    // again and again
    {
        const Bytes& data = large[3].second;
        std::string ms;
        nj2k_codec_t* one = threaded_codec(0, nullptr, 1, false, ms);
        const ThreadedOutcome ref = threaded_decode(one, data, data.size(), NJ2K_SAMPLE_I32, Layout::Planar, ms, false);
        nj2k_destroy_codec(one);
        std::string mt;
        nj2k_codec_t* many = threaded_codec(0, &pool, 8, false, mt);
        int differ = 0;
        constexpr int kRepeats = 100;
        for (int i = 0; i < kRepeats; ++i) {
            std::string why;
            const ThreadedOutcome o =
                threaded_decode(many, data, data.size(), NJ2K_SAMPLE_I32, Layout::Planar, mt, false);
            differ += !same_outcome(ref, o, why);
        }
        nj2k_destroy_codec(many);
        CHECK_MSG(differ == 0, "%d of %d repeated decodes at 8 threads differ", differ, kRepeats);
        std::printf("threads: %s decoded %d times at 8 threads, %d different\n", large[3].first.c_str(), kRepeats,
                    differ);
    }

    // decoders sharing the pool
    {
        std::vector<ThreadedOutcome> refs;
        for (const auto& [name, data] : large) {
            std::string ms;
            nj2k_codec_t* one = threaded_codec(0, nullptr, 1, false, ms);
            refs.push_back(threaded_decode(one, data, data.size(), NJ2K_SAMPLE_I32, Layout::Planar, ms, false));
            nj2k_destroy_codec(one);
        }
        std::atomic<int> differ{0}, done{0};
        std::vector<std::thread> hosts;
        for (int t = 0; t < 4; ++t) {
            hosts.emplace_back([&, t] {
                std::string m;
                nj2k_codec_t* codec = threaded_codec(0, &pool, 8, false, m);
                for (int round = 0; round < 3; ++round) {
                    for (std::size_t i = 0; i < large.size(); ++i) {
                        const std::size_t k = (i + std::size_t(t)) % large.size();
                        std::string why;
                        const ThreadedOutcome o = threaded_decode(codec, large[k].second, large[k].second.size(),
                                                                  NJ2K_SAMPLE_I32, Layout::Planar, m, false);
                        differ += !same_outcome(refs[k], o, why);
                        ++done;
                    }
                }
                nj2k_destroy_codec(codec);
            });
        }
        for (std::thread& h : hosts) {
            h.join();
        }
        CHECK_MSG(differ == 0, "%d of %d decodes by decoders sharing the pool differ", differ.load(), done.load());
        std::printf("threads: 4 decoders sharing one pool, %d decodes, %d different\n", done.load(), differ.load());
    }

    // the API's checks
    {
        nj2k_codec_t* codec = memory_codec(0);
        nj2k_executor_t ex = pool.executor(4);
        CHECK(nj2k_decoder_set_executor(codec, &ex) && nj2k_get_status(codec) == NJ2K_OK);
        CHECK(nj2k_decoder_set_executor(codec, nullptr));
        ex.struct_size = sizeof(nj2k_executor_t) - 1;
        CHECK(!nj2k_decoder_set_executor(codec, &ex) && nj2k_get_status(codec) == NJ2K_E_ARGUMENT);
        ex = pool.executor(4);
        ex.parallel_for = nullptr;
        CHECK(!nj2k_decoder_set_executor(codec, &ex) && nj2k_get_status(codec) == NJ2K_E_ARGUMENT);
        CHECK(!nj2k_decoder_set_executor(nullptr, nullptr));
        nj2k_destroy_codec(codec);
    }

    // other files (the private bench corpus, the large images), locally: every count against one
    // thread at the library's thresholds and forced, whole at reduce 0-2
    if (!files.empty()) {
        std::size_t n = 0, d = 0;
        for (const auto& path : expand_inputs(files)) {
            const auto data = read_file(path);
            if (!data) {
                CHECK_MSG(false, "cannot read %s", path.c_str());
                continue;
            }
            ++n;
            const bool big = data->size() > (4u << 20);
            d += threads_file(pool, base_name(path), *data, false, big ? 0 : 2, false);
            if (!big) {
                d += threads_file(pool, base_name(path), *data, true, 0, false);
            }
        }
        std::printf("threads: %zu other files, %zu threaded decodes equal to one thread's\n", n, d);
    }
}

}  // namespace

int main(int argc, char** argv)
{
    console_reports();
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // every line out before a crash
    const bool threads = argc >= 3 && std::strcmp(argv[2], "--threads") == 0;
    if (!(argc == 2 || (argc == 4 && std::strcmp(argv[2], "--heap-files") == 0) || (threads && argc == 3) ||
          (threads && argc == 5 && std::strcmp(argv[3], "--files") == 0))) {
        std::fprintf(stderr, "usage: nj2k_decode_unit <corpus folder> [--heap-files <list or folder> | --threads "
                             "[--files <list or folder>]]\n");
        return 2;
    }
    g_corpus = argv[1];
    if (threads) {
        // stage 5's gate alone (the ThreadSanitizer job; with --files, the private bench corpus and
        // the large images, locally)
        test_threads(argc == 5 ? argv[4] : "");
        std::printf("%d checks, %d failures\n", g_checks, g_failures);
        return g_failures ? 1 : 0;
    }
    if (argc == 4) {
        // the zero-heap gate alone, on other files (the private bench corpus, locally)
        test_zero_heap_requests(argv[3], 512, "files");
        std::printf("%d checks, %d failures\n", g_checks, g_failures);
        return g_failures ? 1 : 0;
    }
    test_dp3();
    test_two_invk();
    test_dp2();
#if defined(__AVX2__)
    test_kernels_colour();
    test_kernels_store();
    test_kernels_wavelet();
    test_kernels_output();
#else
    std::printf("kernels: scalar build, no AVX2 versions to compare\n");
#endif
    test_output_fused();
    test_dwt_whole();
    test_dp12();
    test_dp13();
    test_dp15();
    test_dp20();
    test_memory_decode();
    test_large_generated();
    test_mct_unpaired();
    test_poc_fewer_resolutions();
    test_memory_states();
    test_memory_bound();
    test_clamp_tile_styles();
    test_tile_part_markers();
    test_need_data();
    test_memory_selection();
    test_sparse_owned_reuse();
    test_tile_memory_limit();
    test_viewer_path();
    test_zero_heap();
    test_zero_heap_requests(g_corpus, 512, "corpus");
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

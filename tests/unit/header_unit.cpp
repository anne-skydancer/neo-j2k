// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// Stage 1b: the header model and the limits. Statuses (NEED_DATA at every cut inside a header, never
// damage), the limit profiles, JP2 reading (streams without a length, the codestream box's length,
// the bits-per-component box), component depths and their warning, the fork audit's probe rows (S2,
// S3) and the plan audit's (A1) with the memory they make neo-j2k allocate, marker precedence (W7),
// quantization counts (W8), Psot = 0 (W5), the reduce policy (W4) and the reduce setter (S1), the
// allocator hook with failure injection, a header model that owns its bytes (plan audit D5), tile-part
// markers in any order at the same cost, the component selection, the decoded dimensions against the
// oracle's decodes, and header reads of mutated headers. The oracle (OpenJPEG at the pinned fork
// commit) is linked beside neo-j2k; the internal header reader is reached through the static library.
#include <openjpeg.h>

#include <neoj2k/neoj2k.h>
#include <neoj2k/neoj2k.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#endif

#include "common.h"
#include "decoder.h"
#include "j2k_scan.h"

// the library's internals (by their path: src/ is no include directory)
#include "../../src/header.h"
#include "../../src/jp2.h"

using namespace nj2k::test;
namespace fs = std::filesystem;

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

std::vector<std::pair<std::string, Bytes>> corpus_files()
{
    std::vector<std::pair<std::string, Bytes>> out;
    for (const auto& path : expand_inputs(g_corpus)) {
        if (auto data = read_file(path)) {
            out.emplace_back(base_name(path), std::move(*data));
        }
    }
    return out;
}

// ---- a counting allocator for the hook --------------------------------------------------------------

struct Counting {
    std::size_t allocs = 0, frees = 0, live = 0, peak = 0, total = 0;
    std::size_t fail_at = SIZE_MAX;  // the allocation (counted from 0) that fails

    static void* alloc(void* user, NJ2K_SIZE_T size, NJ2K_SIZE_T alignment)
    {
        auto* c = static_cast<Counting*>(user);
        if (c->allocs++ == c->fail_at) {
            return nullptr;
        }
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
            c->total += size;
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

// ---- crafted codestreams -------------------------------------------------------------------------

struct Writer {
    Bytes b;
    Writer& u8(unsigned v)
    {
        b.push_back(std::uint8_t(v));
        return *this;
    }
    Writer& u16(unsigned v) { return u8(v >> 8).u8(v & 0xFF); }
    Writer& u32(std::uint32_t v) { return u16(v >> 16).u16(v & 0xFFFF); }
    Writer& bytes(const Bytes& v)
    {
        b.insert(b.end(), v.begin(), v.end());
        return *this;
    }
    Writer& segment(unsigned marker, const Bytes& body) { return u16(marker).u16(unsigned(body.size() + 2)).bytes(body); }
};

struct Siz {
    std::uint32_t x1 = 32, y1 = 32, x0 = 0, y0 = 0, tdx = 0, tdy = 0, tx0 = 0, ty0 = 0;
    std::uint32_t comps = 3;
    std::uint8_t ssiz = 7, dx = 1, dy = 1;
    std::uint16_t rsiz = 0;
    Bytes body() const
    {
        Writer w;
        w.u16(rsiz).u32(x1).u32(y1).u32(x0).u32(y0).u32(tdx ? tdx : x1).u32(tdy ? tdy : y1).u32(tx0).u32(ty0).u16(comps);
        for (std::uint32_t c = 0; c < comps; ++c) {
            w.u8(ssiz).u8(dx).u8(dy);
        }
        return w.b;
    }
};

struct Cod {
    unsigned scod = 0, prg = 0, layers = 1, mct = 0, levels = 5, xcb = 4, ycb = 4, style = 0, transform = 1;
    Bytes precincts;  // one byte per resolution when scod & 1
    Bytes body() const
    {
        Writer w;
        w.u8(scod).u8(prg).u16(layers).u8(mct).u8(levels).u8(xcb).u8(ycb).u8(style).u8(transform).bytes(precincts);
        return w.b;
    }
};

Bytes coc_body(unsigned comp, unsigned levels, unsigned scoc = 0, Bytes precincts = {})
{
    Writer w;
    w.u8(comp).u8(scoc).u8(levels).u8(4).u8(4).u8(0).u8(1).bytes(precincts);
    return w.b;
}

// No quantization (5/3), one 8-bit step per band; bands = 3 x levels + 1
Bytes qcd_none(unsigned bands, unsigned guard = 2)
{
    Writer w;
    w.u8(guard << 5);
    for (unsigned i = 0; i < bands; ++i) {
        w.u8((9 + (i ? 1 : 0)) << 3);
    }
    return w.b;
}

// A whole codestream: SOC, SIZ, COD, QCD, extra main-header segments, then one tile-part with no
// data (SOT Psot = 14, SOD) and EOC.
Bytes codestream(const Siz& siz, const Cod& cod, const Bytes& extra_main = {}, bool qcd = true)
{
    Writer w;
    w.u16(0xFF4F).segment(0xFF51, siz.body()).segment(0xFF52, cod.body());
    if (qcd) {
        w.segment(0xFF5C, qcd_none(3 * cod.levels + 1));
    }
    w.bytes(extra_main);
    w.u16(0xFF90).u16(10).u16(0).u32(14).u8(0).u8(1).u16(0xFF93).u16(0xFFD9);
    return w.b;
}

// ---- reading headers through the API ----------------------------------------------------------------

struct Read {
    nj2k_codec_t* codec = nullptr;
    nj2k_image_t* image = nullptr;
    nj2k_status_t status = NJ2K_E_STATE;
    bool ok = false;
    std::string errors;
    Read() = default;
    Read(const Read&) = delete;
    Read& operator=(const Read&) = delete;
    ~Read()
    {
        nj2k_image_destroy(image);
        nj2k_destroy_codec(codec);
    }
};

void collect(const char* msg, void* user)
{
    auto* s = static_cast<std::string*>(user);
    if (s->size() < 600) {
        *s += msg;
    }
}

struct Options {
    const nj2k_decoder_limits_t* limits = nullptr;
    nj2k_allocator_t* allocator = nullptr;
    NJ2K_CODEC_FORMAT format = NJ2K_CODEC_J2K;
    int reduce = -1;  // set in the parameters (before the header) when >= 0
    std::uint32_t policy = NJ2K_REDUCE_EXACT;
    std::size_t buffer = 0;  // the stream's buffer size; 0: the bytes'
    bool no_length = false;  // the stream does not give its length (as a pipe)
    std::string* warnings = nullptr;  // collects the warnings when set
};

void read_header(Read& r, const std::uint8_t* data, std::size_t size, const Options& o = {})
{
    r.codec = o.allocator ? nj2k_create_decompress_with_allocator(o.format, o.allocator) : nj2k_create_decompress(o.format);
    if (!r.codec) {
        r.status = NJ2K_E_NO_MEMORY;
        return;
    }
    nj2k_set_error_handler(r.codec, collect, &r.errors);
    if (o.warnings) {
        nj2k_set_warning_handler(r.codec, collect, o.warnings);
    }
    nj2k_dparameters_t p;
    nj2k_set_default_decoder_parameters(&p);
    if (o.reduce >= 0) {
        p.cp_reduce = std::uint32_t(o.reduce);
    }
    nj2k_setup_decoder(r.codec, &p);
    nj2k_decoder_set_strict_mode(r.codec, NJ2K_FALSE);
    nj2k_decoder_set_reduce_policy(r.codec, o.policy);
    if (o.limits) {
        nj2k_decoder_set_limits(r.codec, o.limits);
    }
    nj2k::MemoryReader reader(std::span<const std::uint8_t>(data, size));
    auto stream = reader.make_stream(o.buffer);
    if (stream && o.no_length) {
        nj2k_stream_set_user_data_length(stream.get(), 0);  // 0: not given
    }
    r.ok = stream && nj2k_read_header(stream.get(), r.codec, &r.image);
    r.status = nj2k_get_status(r.codec);
}

void read_header(Read& r, const Bytes& b, const Options& o = {})
{
    read_header(r, b.data(), b.size(), o);
}

// The oracle's header read (non-strict, as the fuzz target and the viewer's header pass), its
// warnings collected when asked; with_length false makes a stream that does not give its length.
bool oracle_read(const Bytes& b, bool jp2, bool with_length = true, std::string* warnings = nullptr)
{
    struct Pos {
        const Bytes* b;
        std::size_t at;
    } pos{&b, 0};
    opj_codec_t* codec = opj_create_decompress(jp2 ? OPJ_CODEC_JP2 : OPJ_CODEC_J2K);
    opj_dparameters_t params;
    opj_set_default_decoder_parameters(&params);
    opj_setup_decoder(codec, &params);
    opj_decoder_set_strict_mode(codec, OPJ_FALSE);
    opj_set_error_handler(codec, [](const char*, void*) {}, nullptr);
    if (warnings) {
        opj_set_warning_handler(codec, collect, warnings);
    }
    opj_stream_t* s = opj_stream_create(b.empty() ? 1 : b.size(), OPJ_TRUE);
    opj_stream_set_user_data(s, &pos, nullptr);
    if (with_length) {
        opj_stream_set_user_data_length(s, b.size());
    }
    opj_stream_set_read_function(s, [](void* out, OPJ_SIZE_T n, void* user) -> OPJ_SIZE_T {
        auto* p = static_cast<Pos*>(user);
        if (p->at >= p->b->size()) {
            return static_cast<OPJ_SIZE_T>(-1);
        }
        const std::size_t k = std::min<std::size_t>(n, p->b->size() - p->at);
        std::memcpy(out, p->b->data() + p->at, k);
        p->at += k;
        return k;
    });
    opj_stream_set_skip_function(s, [](OPJ_OFF_T n, void* user) -> OPJ_OFF_T {
        auto* p = static_cast<Pos*>(user);
        const OPJ_OFF_T to = OPJ_OFF_T(p->at) + n;
        if (to < 0 || to > OPJ_OFF_T(p->b->size())) {
            p->at = to < 0 ? 0 : p->b->size();
            return -1;
        }
        p->at = std::size_t(to);
        return n;
    });
    opj_stream_set_seek_function(s, [](OPJ_OFF_T to, void* user) -> OPJ_BOOL {
        auto* p = static_cast<Pos*>(user);
        if (to < 0 || to > OPJ_OFF_T(p->b->size())) {
            return OPJ_FALSE;
        }
        p->at = std::size_t(to);
        return OPJ_TRUE;
    });
    opj_image_t* image = nullptr;
    const bool ok = opj_read_header(s, codec, &image) != 0;
    opj_image_destroy(image);
    opj_stream_destroy(s);
    opj_destroy_codec(codec);
    return ok;
}

// ---- the tests --------------------------------------------------------------------------------------

void test_limits_api()
{
    nj2k_decoder_limits_t n{}, s{}, os{}, u{};
    opj_decoder_limits_t on{}, osl{};
    CHECK(nj2k_decoder_limits_init(&n, NJ2K_DECODER_LIMITS_NONE));
    CHECK(nj2k_decoder_limits_init(&s, NJ2K_DECODER_LIMITS_SL_TEXTURE));
    CHECK(opj_decoder_limits_init(&on, OPJ_DECODER_LIMITS_NONE));
    CHECK(opj_decoder_limits_init(&osl, OPJ_DECODER_LIMITS_SL_TEXTURE));
    // the same profiles as the fork's stage 1a hotfix, field for field
    static_assert(sizeof(nj2k_decoder_limits_t) == sizeof(opj_decoder_limits_t), "limits layout");
    CHECK(std::memcmp(&n, &on, sizeof(n)) == 0);
    CHECK(std::memcmp(&s, &osl, sizeof(s)) == 0);
    CHECK(s.max_tiles == 1024 && s.max_components == 8 && s.max_layers == 64 && s.max_decoded_side == 2048 &&
          s.max_memory == (256ull << 20));
#if NJ2K_TEST_OPENSIM
    CHECK(nj2k_decoder_limits_init(&os, NJ2K_DECODER_LIMITS_OPENSIM_TEXTURE));
    CHECK(os.max_decoded_side == 16384 && os.max_components == 8 && os.max_memory == s.max_memory &&
          os.max_tiles == s.max_tiles && os.max_layers == s.max_layers);
#else
    CHECK(!nj2k_decoder_limits_init(&os, NJ2K_DECODER_LIMITS_OPENSIM_TEXTURE));
#endif
    u.max_tiles = 7;
    CHECK(!nj2k_decoder_limits_init(&u, 99));
    CHECK(u.max_tiles == 0 && u.max_memory == 0);
    CHECK(!nj2k_decoder_limits_init(nullptr, NJ2K_DECODER_LIMITS_SL_TEXTURE));
    nj2k::Codec codec = nj2k::make_decoder();
    CHECK(codec != nullptr);
    CHECK(!nj2k_decoder_set_limits(codec.get(), nullptr) && nj2k_get_status(codec.get()) == NJ2K_E_ARGUMENT);
    CHECK(nj2k_decoder_set_limits(codec.get(), &s) && nj2k_get_status(codec.get()) == NJ2K_OK);
    CHECK(!nj2k_decoder_set_reduce_policy(codec.get(), 7) && nj2k_get_status(codec.get()) == NJ2K_E_ARGUMENT);
    {
        // after the header, limits are refused and change nothing, as the fork's (6bad83c0)
        const auto files = corpus_files();
        CHECK(!files.empty());
        Read r;
        read_header(r, files.front().second);
        CHECK(r.ok);
        CHECK(!nj2k_decoder_set_limits(r.codec, &s) && nj2k_get_status(r.codec) == NJ2K_E_STATE);
    }
    CHECK(nj2k_get_status(nullptr) == NJ2K_E_ARGUMENT);
    // every status has its own text
    const nj2k_status_t all[] = {NJ2K_OK, NJ2K_TRUNCATED, NJ2K_E_NEED_DATA, NJ2K_E_ARGUMENT, NJ2K_E_NOT_JPEG2000,
                                 NJ2K_E_HEADER, NJ2K_E_UNSUPPORTED, NJ2K_E_LIMIT, NJ2K_E_NO_MEMORY, NJ2K_E_DATA,
                                 NJ2K_E_OUTPUT, NJ2K_E_STATE};
    for (std::size_t i = 0; i < std::size(all); ++i) {
        for (std::size_t j = i + 1; j < std::size(all); ++j) {
            CHECK(std::strcmp(nj2k_status_string(all[i]), nj2k_status_string(all[j])) != 0);
        }
    }
    CHECK(std::strcmp(nj2k_status_string(-77), "failure") == 0 && std::strcmp(nj2k_status_string(77), "success") == 0);
}

// Every cut inside a main header is "need more data", never damage; the header reads once the SOT
// marker after it has arrived.
void test_need_data()
{
    std::size_t files = 0, prefixes = 0;
    for (const auto& [name, data] : corpus_files()) {
        const auto mh = parse_main_header(data);
        if (!mh) {
            continue;
        }
        ++files;
        const std::size_t end = mh->end + 2;
        for (std::size_t n = 0; n <= end && n <= data.size(); ++n) {
            Read r;
            read_header(r, data.data(), n);
            ++prefixes;
            if (n < end) {
                CHECK_MSG(!r.ok && r.status == NJ2K_E_NEED_DATA, "%s cut at %zu: status %d", name.c_str(), n, r.status);
            } else {
                CHECK_MSG(r.ok && r.status == NJ2K_OK, "%s at %zu: status %d %s", name.c_str(), n, r.status,
                          r.errors.c_str());
            }
        }
        // a stream whose read function hands out one byte at a time gives the same answer
        Read r;
        Options o;
        o.buffer = 1;
        read_header(r, data, o);
        CHECK_MSG(r.ok, "%s through a 1-byte buffer: %s", name.c_str(), r.errors.c_str());
    }
    CHECK(files > 50);
    std::printf("  need-data: %zu files, %zu prefixes read\n", files, prefixes);
    // not JPEG 2000 at the first byte that says so
    const std::uint8_t zero[2] = {0x00, 0x4F}, ff00[2] = {0xFF, 0x00}, ff[1] = {0xFF};
    {
        Read r;
        read_header(r, zero, 1);
        CHECK(r.status == NJ2K_E_NOT_JPEG2000);
    }
    {
        Read r;
        read_header(r, ff00, 2);
        CHECK(r.status == NJ2K_E_NOT_JPEG2000);
    }
    {
        Read r;
        read_header(r, ff, 1);
        CHECK(r.status == NJ2K_E_NEED_DATA);
    }
    {
        Read r;
        read_header(r, ff, 0);
        CHECK(r.status == NJ2K_E_NEED_DATA);
    }
}

// A JP2 file around a corpus codestream: signature, file type, header (image header, colour), and
// the codestream box; and the same cut at every byte before the codestream.
Bytes jp2_wrap(const Bytes& j2k, std::uint32_t w, std::uint32_t h, std::uint16_t nc, std::uint32_t enumcs,
               const Bytes& icc = {}, std::uint8_t bpc = 7)
{
    Writer ihdr;
    ihdr.u32(h).u32(w).u16(nc).u8(bpc).u8(7).u8(0).u8(0);
    Writer colr;
    if (icc.empty()) {
        colr.u8(1).u8(0).u8(0).u32(enumcs);
    } else {
        colr.u8(2).u8(0).u8(0).bytes(icc);
    }
    Writer jp2h;
    jp2h.u32(8 + 14).u32(0x69686472).bytes(ihdr.b).u32(8 + std::uint32_t(colr.b.size())).u32(0x636F6C72).bytes(colr.b);
    Writer f;
    f.u32(12).u32(0x6A502020).u32(0x0D0A870A);
    f.u32(20).u32(0x66747970).u32(0x6A703220).u32(0).u32(0x6A703220);
    f.u32(8 + std::uint32_t(jp2h.b.size())).u32(0x6A703268).bytes(jp2h.b);
    f.u32(16).u32(0x786D6C20).u32(0x3C3F786D).u32(0x6C3F3E00);  // an xml box, skipped
    f.u32(8 + std::uint32_t(j2k.size())).u32(0x6A703263).bytes(j2k);
    return f.b;
}

void test_jp2()
{
    const auto data = read_file(g_corpus + "/sl_32_rgb_97.j2k");
    CHECK(data.has_value());
    if (!data) {
        return;
    }
    const Bytes file = jp2_wrap(*data, 32, 32, 3, 16);
    Options o;
    o.format = NJ2K_CODEC_JP2;
    {
        Read r;
        read_header(r, file, o);
        CHECK_MSG(r.ok, "%s", r.errors.c_str());
        CHECK(r.ok && r.image->color_space == NJ2K_CLRSPC_SRGB && r.image->numcomps == 3 && r.image->comps[0].w == 32);
    }
    // the oracle reads the same file to the same facts
    {
        auto oracle = make_oracle_backend(), neo = make_neoj2k_backend();
        Request rq;
        rq.data = file.data();
        rq.size = file.size();
        rq.jp2 = true;
        const HeaderFacts a = oracle->header(rq), b = neo->header(rq);
        CHECK_MSG(a.ok && header_difference(a, b).empty(), "%s", header_difference(a, b).c_str());
    }
    // an ICC profile reaches the image
    {
        const Bytes icc = {1, 2, 3, 4, 5, 6, 7, 8, 9};
        const Bytes with_icc = jp2_wrap(*data, 32, 32, 3, 0, icc);
        Read r;
        read_header(r, with_icc, o);
        CHECK(r.ok && r.image->icc_profile_len == icc.size() &&
              std::memcmp(r.image->icc_profile_buf, icc.data(), icc.size()) == 0 &&
              r.image->color_space == NJ2K_CLRSPC_UNKNOWN);
    }
    // the image header must agree with SIZ
    {
        const Bytes wrong = jp2_wrap(*data, 33, 32, 3, 16);
        Read r;
        read_header(r, wrong, o);
        CHECK(!r.ok && r.status == NJ2K_E_HEADER);
    }
    // every cut before the codestream's main header is complete needs more data
    const std::size_t cs = file.size() - data->size();
    const auto mh = parse_main_header(*data);
    for (std::size_t n = 0; mh && n < cs + mh->end + 2; ++n) {
        Read r;
        read_header(r, file.data(), n, o);
        CHECK_MSG(r.status == NJ2K_E_NEED_DATA, "jp2 cut at %zu: %d", n, r.status);
    }
    // a palette box that declares more entries than it holds is refused before its values are
    // allocated (fuzz regression header-jp2-pclr-values-before-size)
    {
        Writer pclr;
        pclr.u16(1024).u8(255);
        for (int i = 0; i < 255; ++i) {
            pclr.u8(7);
        }
        pclr.bytes(Bytes(300, 0x11));
        Writer jp2h;
        jp2h.u32(8 + 14).u32(0x69686472).u32(32).u32(32).u16(3).u8(7).u8(7).u8(0).u8(0);
        jp2h.u32(8 + std::uint32_t(pclr.b.size())).u32(0x70636C72).bytes(pclr.b);
        Writer f;
        f.u32(12).u32(0x6A502020).u32(0x0D0A870A).u32(20).u32(0x66747970).u32(0x6A703220).u32(0).u32(0x6A703220);
        f.u32(8 + std::uint32_t(jp2h.b.size())).u32(0x6A703268).bytes(jp2h.b);
        f.u32(8 + std::uint32_t(data->size())).u32(0x6A703263).bytes(*data);
        Counting c;
        nj2k_allocator_t hook = c.hook();
        Options po = o;
        po.allocator = &hook;
        Read r;
        read_header(r, f.b, po);
        CHECK(!r.ok && r.status == NJ2K_E_HEADER);
        CHECK_MSG(c.peak < 16 * 1024, "%zu bytes", c.peak);
    }
    // a codestream is no JP2 file, and a JP2 file no codestream
    {
        Read r;
        read_header(r, *data, o);
        CHECK(r.status == NJ2K_E_NOT_JPEG2000);
    }
    {
        Read r;
        read_header(r, file);
        CHECK(r.status == NJ2K_E_NOT_JPEG2000);
    }
    // From a stream whose length is not given no box is read: its size would be all there is to
    // go by, and 28 bytes can declare a file type box of 4 GiB. OpenJPEG reads no JP2 file from
    // such a stream either.
    {
        Writer bomb;
        bomb.u32(12).u32(0x6A502020).u32(0x0D0A870A).u32(0xFFFFFFF0u).u32(0x66747970).u32(0x6A703220).u32(0);
        CHECK(bomb.b.size() == 28);
        nj2k_decoder_limits_t sl{};
        nj2k_decoder_limits_init(&sl, NJ2K_DECODER_LIMITS_SL_TEXTURE);
        const Bytes* const inputs[2] = {&bomb.b, &file};
        const nj2k_decoder_limits_t* const profiles[2] = {nullptr, &sl};
        for (const Bytes* input : inputs) {
            for (const nj2k_decoder_limits_t* lim : profiles) {
                Counting c;
                nj2k_allocator_t hook = c.hook();
                Options po = o;
                po.allocator = &hook;
                po.limits = lim;
                po.no_length = true;
                Read r;
                read_header(r, *input, po);
                CHECK_MSG(!r.ok && r.status == NJ2K_E_UNSUPPORTED, "status %d", r.status);
                CHECK_MSG(c.peak < 16 * 1024, "%zu bytes", c.peak);
            }
        }
        // the same bytes with their length given: the box is found short before it is allocated
        Counting c;
        nj2k_allocator_t hook = c.hook();
        Options po = o;
        po.allocator = &hook;
        Read r;
        read_header(r, bomb.b, po);
        CHECK(!r.ok && r.status == NJ2K_E_NEED_DATA && c.peak < 16 * 1024);
#if defined(NDEBUG)
        // the oracle, which asserts on a stream without a length in builds without NDEBUG
        CHECK(oracle_read(file, true) && !oracle_read(file, true, false));
#endif
    }
    auto oracle = make_oracle_backend(), neo = make_neoj2k_backend();
    auto same_as_oracle = [&](const Bytes& f, bool reads, const char* what) {
        Request rq;
        rq.data = f.data();
        rq.size = f.size();
        rq.jp2 = true;
        const HeaderFacts a = oracle->header(rq), b = neo->header(rq);
        CHECK_MSG(a.ok == reads && header_difference(a, b).empty(), "%s: %s", what, header_difference(a, b).c_str());
    };
    // the codestream box is taken whatever its length says, and read to the end, as OpenJPEG does:
    // a length too short for its own header, and an XL length of 4 GiB or more
    {
        const std::size_t box = file.size() - data->size() - 8;
        for (std::uint32_t length : {2u, 7u}) {
            Bytes f = file;
            f[box] = 0;
            f[box + 1] = 0;
            f[box + 2] = 0;
            f[box + 3] = std::uint8_t(length);
            same_as_oracle(f, true, "short codestream box");
        }
        Bytes xl(file.begin(), file.begin() + std::ptrdiff_t(box));
        Writer w;
        w.u32(1).u32(0x6A703263).u32(1).u32(0x00000010).bytes(*data);
        xl.insert(xl.end(), w.b.begin(), w.b.end());
        same_as_oracle(xl, true, "codestream box of 4 GiB and more");
        Read r;
        read_header(r, xl, o);
        CHECK_MSG(r.ok, "%s", r.errors.c_str());
    }
    // a bits-per-component box before the image header, empty as numcomps then is: read, and it
    // gives no values (the image header then says 3 components)
    {
        Writer jp2h;
        jp2h.u32(8).u32(0x62706363);  // bpcc, empty
        jp2h.u32(8 + 14).u32(0x69686472).u32(32).u32(32).u16(3).u8(255).u8(7).u8(0).u8(0);
        jp2h.u32(8 + 7).u32(0x636F6C72).u8(1).u8(0).u8(0).u32(16);
        Writer f;
        f.u32(12).u32(0x6A502020).u32(0x0D0A870A).u32(20).u32(0x66747970).u32(0x6A703220).u32(0).u32(0x6A703220);
        f.u32(8 + std::uint32_t(jp2h.b.size())).u32(0x6A703268).bytes(jp2h.b);
        f.u32(8 + std::uint32_t(data->size())).u32(0x6A703263).bytes(*data);
        same_as_oracle(f.b, true, "empty bpcc before ihdr");
        namespace d = nj2k::detail;
        d::Allocator sys = d::Allocator::system();
        d::Arena arena;
        arena.init(&sys);
        d::Messages msg;
        d::Source src(f.b.data(), f.b.size());
        d::Jp2Header* jp2 = nullptr;
        CHECK(d::read_jp2_boxes(src, arena, msg, jp2) == NJ2K_OK && jp2 && jp2->numcomps == 3 && jp2->bpcc == nullptr);
    }
    // DP-11: a box OpenJPEG skips sets its state to JP2_STATE_UNKNOWN (0x7FFFFFFF), which claims the
    // file type and JP2 header boxes read (fuzz regression header-jp2-image-box-before-ftyp). An
    // image box between the signature and a JP2 header box, with no file type box: OpenJPEG reads
    // the file, neo-j2k refuses it (the file type box must be second). A file type box, an unknown
    // box, then a greyscale colour box before the JP2 header box (whose colour is sRGB): OpenJPEG
    // takes the greyscale one, neo-j2k ignores it as outside the JP2 header box.
    {
        Writer jp2h;
        jp2h.u32(8 + 14).u32(0x69686472).u32(32).u32(32).u16(3).u8(7).u8(7).u8(0).u8(0);
        jp2h.u32(8 + 7).u32(0x636F6C72).u8(1).u8(0).u8(0).u32(16);
        Writer no_ftyp;
        no_ftyp.u32(12).u32(0x6A502020).u32(0x0D0A870A).u32(20).u32(0x62706363).u32(0x6A703220).u32(0).u32(0x6A703220);
        no_ftyp.u32(8 + std::uint32_t(jp2h.b.size())).u32(0x6A703268).bytes(jp2h.b);
        no_ftyp.u32(8 + std::uint32_t(data->size())).u32(0x6A703263).bytes(*data);
        Writer grey;
        grey.u32(12).u32(0x6A502020).u32(0x0D0A870A).u32(20).u32(0x66747970).u32(0x6A703220).u32(0).u32(0x6A703220);
        grey.u32(8 + 4).u32(0x786D6C20).u32(0x3C782F3E);  // 'xml ', "<x/>"
        grey.u32(8 + 7).u32(0x636F6C72).u8(1).u8(0).u8(0).u32(17);
        grey.u32(8 + std::uint32_t(jp2h.b.size())).u32(0x6A703268).bytes(jp2h.b);
        grey.u32(8 + std::uint32_t(data->size())).u32(0x6A703263).bytes(*data);
        Request rq;
        rq.jp2 = true;
        rq.data = no_ftyp.b.data();
        rq.size = no_ftyp.b.size();
        const HeaderFacts a1 = oracle->header(rq), b1 = neo->header(rq);
        CHECK_MSG(a1.ok && !b1.ok, "no file type box: oracle %d, neo-j2k %d", int(a1.ok), int(b1.ok));
        Read r;
        read_header(r, no_ftyp.b, o);
        CHECK(!r.ok && r.status == NJ2K_E_HEADER);
        rq.data = grey.b.data();
        rq.size = grey.b.size();
        const HeaderFacts a2 = oracle->header(rq), b2 = neo->header(rq);
        CHECK_MSG(a2.ok && b2.ok && a2.color_space == NJ2K_CLRSPC_GRAY && b2.color_space == NJ2K_CLRSPC_SRGB,
                  "colour box before the JP2 header box: oracle %d, neo-j2k %d", a2.color_space, b2.color_space);
    }
}

// Components of different precision and sign: a codestream may have them without a word, as in
// OpenJPEG's J2K decoder; a JP2 file whose image header gives one depth for all (BPC != 255) gets
// OpenJPEG's warning, and one whose BPC is 255 does not. The oracle warns in the same cases.
void test_varying_depth()
{
    Siz s;  // 8-bit, 12-bit and signed 8-bit components
    Writer siz;
    siz.u16(0).u32(s.x1).u32(s.y1).u32(0).u32(0).u32(s.x1).u32(s.y1).u32(0).u32(0).u16(3);
    siz.u8(7).u8(1).u8(1).u8(11).u8(1).u8(1).u8(0x87).u8(1).u8(1);
    Cod c;
    Writer cs;
    cs.u16(0xFF4F).segment(0xFF51, siz.b).segment(0xFF52, c.body()).segment(0xFF5C, qcd_none(16));
    cs.u16(0xFF90).u16(10).u16(0).u32(14).u8(0).u8(1).u16(0xFF93).u16(0xFFD9);
    const auto warned = [](const std::string& text) { return text.find("Despite JP2 BPC!=255") != std::string::npos; };
    struct Case {
        const char* name;
        Bytes bytes;
        bool jp2, warns;
    };
    const Case cases[] = {
        {"codestream", cs.b, false, false},
        {"JP2, BPC 7", jp2_wrap(cs.b, 32, 32, 3, 16, {}, 7), true, true},
        {"JP2, BPC 255", jp2_wrap(cs.b, 32, 32, 3, 16, {}, 255), true, false},
    };
    for (const Case& k : cases) {
        std::string neo_warnings, oracle_warnings;
        Options o;
        o.format = k.jp2 ? NJ2K_CODEC_JP2 : NJ2K_CODEC_J2K;
        o.warnings = &neo_warnings;
        Read r;
        read_header(r, k.bytes, o);
        CHECK_MSG(r.ok, "%s: %s", k.name, r.errors.c_str());
        CHECK_MSG(warned(neo_warnings) == k.warns, "%s: %s", k.name, neo_warnings.c_str());
        CHECK_MSG(oracle_read(k.bytes, k.jp2, true, &oracle_warnings) && warned(oracle_warnings) == k.warns, "%s: oracle: %s",
                  k.name, oracle_warnings.c_str());
    }
}

std::size_t header_peak(const Bytes& b, const nj2k_decoder_limits_t* limits, nj2k_status_t& status, Counting& c)
{
    nj2k_allocator_t hook = c.hook();
    Options o;
    o.allocator = &hook;
    o.limits = limits;
    Read r;
    read_header(r, b, o);
    status = r.status;
    return c.peak;
}

// The fork audit's S2 and S3 probe rows and the plan audit's A1 rows: neo-j2k's header state stays
// flat, whatever a header declares, and the Second Life profile refuses each before allocating
// what it bounds.
void test_probe_rows()
{
    nj2k_decoder_limits_t sl{};
    nj2k_decoder_limits_init(&sl, NJ2K_DECODER_LIMITS_SL_TEXTURE);
    struct Row {
        const char* name;
        Bytes cs;
        nj2k_status_t sl_header;  // the header read under the Second Life profile
    };
    std::vector<Row> rows;
    {
        Siz s;
        Cod c;
        rows.push_back({"32x32 RGB, 1 tile (baseline)", codestream(s, c), NJ2K_OK});
    }
    {
        Siz s;
        s.x1 = 255;
        s.y1 = 257;
        s.tdx = s.tdy = 1;
        Cod c;
        c.levels = 0;
        rows.push_back({"S2: 255x257 in 1x1 tiles (65,535), 3 comps", codestream(s, c), NJ2K_E_LIMIT});
        s.comps = 32;
        rows.push_back({"S2: the same, 32 comps", codestream(s, c), NJ2K_E_LIMIT});
    }
    {
        Siz s;
        s.x1 = s.y1 = 64;
        Cod c;
        c.layers = 65535;
        rows.push_back({"S3: 64x64, 65,535 layers", codestream(s, c), NJ2K_E_LIMIT});
        Siz t;
        t.x1 = t.y1 = 1024;
        Cod d;
        d.layers = 4000;
        d.scod = 1;
        d.precincts.assign(6, 0x44);
        d.precincts[0] = 0x44;
        rows.push_back({"S3: 1024x1024, 16x16 precincts, 4,000 layers", codestream(t, d), NJ2K_E_LIMIT});
    }
    {
        Siz s;
        s.x1 = s.y1 = 16384;
        s.comps = 1;
        Cod c;
        rows.push_back({"A1: 16384x16384 grey, 1 layer", codestream(s, c), NJ2K_OK});
        Siz t;
        t.x1 = t.y1 = 4096;
        t.comps = 16;
        rows.push_back({"A1: 4096x4096, 16 comps", codestream(t, c), NJ2K_E_LIMIT});
    }
    std::size_t baseline = 0;
    for (const Row& row : rows) {
        Counting none, limited;
        nj2k_status_t st_none = 0, st_sl = 0;
        const std::size_t peak = header_peak(row.cs, nullptr, st_none, none);
        header_peak(row.cs, &sl, st_sl, limited);
        if (!baseline) {
            baseline = peak;
        }
        std::printf("  %-48s %4zu bytes: no limits %s, header state %zu bytes; Second Life profile %s\n", row.name,
                    row.cs.size(), nj2k_status_string(st_none), peak, nj2k_status_string(st_sl));
        CHECK_MSG(st_none == NJ2K_OK, "%s", row.name);
        // flat: within a few KiB of the 32 x 32 texture's, whatever the tiles, layers or size
        CHECK_MSG(peak <= baseline + 8192, "%s: %zu bytes against %zu", row.name, peak, baseline);
        CHECK_MSG(st_sl == row.sl_header, "%s: %d", row.name, st_sl);
        CHECK(none.live == 0 && limited.live == 0 && none.allocs == none.frees && limited.allocs == limited.frees);
    }
    // A1's 16384 x 16384: refused before decoding at reduce 0, decoded at reduce 3 (2048 x 2048)
    {
        Siz s;
        s.x1 = s.y1 = 16384;
        s.comps = 1;
        Cod c;
        const Bytes cs = codestream(s, c);
        for (int reduce : {0, 3}) {
            Options o;
            o.limits = &sl;
            o.reduce = reduce;
            Read r;
            read_header(r, cs, o);
            CHECK(r.ok);
            const bool fits = r.ok && nj2k_get_decoded_dimensions(r.codec, r.image);
            CHECK_MSG(fits == (reduce == 3), "reduce %d: %s", reduce, r.errors.c_str());
            CHECK(fits || nj2k_get_status(r.codec) == NJ2K_E_LIMIT);
        }
    }
}

// The bombs the fuzz filters keep from the oracle, and the oracle's known resource defects: under
// the Second Life profile neo-j2k reads each header in a few KiB and refuses, before decoding, every
// request whose header-computed need passes the profile's budget.
void test_bombs(const std::string& bombs_dir, const std::string& defects_dir)
{
    nj2k_decoder_limits_t sl{};
    nj2k_decoder_limits_init(&sl, NJ2K_DECODER_LIMITS_SL_TEXTURE);
    std::vector<std::string> files;
    std::error_code ec;
    for (const std::string& dir : {bombs_dir, defects_dir}) {
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            const std::string ext = e.path().extension().string();
            if (ext == ".j2k" || ext == ".bin") {
                files.push_back(e.path().generic_string());
            }
        }
    }
    std::sort(files.begin(), files.end());
    std::size_t seen = 0;
    for (const auto& path : files) {
        auto data = read_file(path);
        if (!data) {
            continue;
        }
        // decode bombs (.bin) carry the fuzz target's option bytes: reduce level first
        std::size_t skip = 0;
        int reduce = 0;
        if (path.size() > 4 && path.compare(path.size() - 4, 4, ".bin") == 0) {
            if (data->size() < 5 || (*data)[0] == 0xFF) {
                continue;  // a round-trip input, not a codestream
            }
            reduce = (*data)[0] % 8;
            skip = 4;
        } else if (path.find("od5") != std::string::npos) {
            reduce = 5;
        }
        ++seen;
        Counting count;
        nj2k_allocator_t hook = count.hook();
        Options o;
        o.allocator = &hook;
        o.limits = &sl;
        o.reduce = reduce;
        o.policy = NJ2K_REDUCE_CLAMP;
        Read r;
        read_header(r, data->data() + skip, data->size() - skip, o);
        std::string verdict = r.ok ? "header read" : std::string("refused: ") + nj2k_status_string(r.status);
        if (r.ok) {
            const bool fits = nj2k_get_decoded_dimensions(r.codec, r.image);
            verdict += fits ? ", decode within the profile" : std::string(", decode refused: ") + nj2k_status_string(nj2k_get_status(r.codec));
        }
        std::printf("  %-44s r%d: %s; header state %zu bytes\n", base_name(path).c_str(), reduce, verdict.c_str(), count.peak);
        CHECK_MSG(count.peak <= 64 * 1024, "%s: %zu bytes of header state", path.c_str(), count.peak);
    }
    CHECK(seen >= 12);
}

// W7: marker precedence whatever the order. A COC before the COD it overrides keeps its component;
// OpenJPEG lets the later COD overwrite it (doc/departures.md).
void test_precedence()
{
    Siz s;
    Cod c;  // 5 levels for every component
    Writer pre;
    pre.u16(0xFF4F).segment(0xFF51, s.body()).segment(0xFF53, coc_body(1, 2)).segment(0xFF52, c.body());
    pre.segment(0xFF5C, qcd_none(16));
    pre.u16(0xFF90).u16(10).u16(0).u32(14).u8(0).u8(1).u16(0xFF93).u16(0xFFD9);
    Read r;
    read_header(r, pre.b);
    CHECK_MSG(r.ok, "%s", r.errors.c_str());
    nj2k_codestream_info_v2_t* info = r.ok ? nj2k_get_cstr_info(r.codec) : nullptr;
    CHECK(info && info->m_default_tile_info.tccp_info[1].numresolutions == 3 &&
          info->m_default_tile_info.tccp_info[0].numresolutions == 6 &&
          info->m_default_tile_info.tccp_info[2].numresolutions == 6);
    nj2k_destroy_cstr_info(&info);
    CHECK(info == nullptr);
    // the oracle loses the COC
    auto oracle = make_oracle_backend();
    Request rq;
    rq.data = pre.b.data();
    rq.size = pre.b.size();
    const HeaderFacts f = oracle->header(rq);
    CHECK(f.ok && f.tccp.size() == 3 && f.tccp[1].numresolutions == 6);

    // tile-part headers: a tile COC before the tile COD keeps its component too, and the tile's COD
    // overrides the main header's COC (tile COD > main COC)
    namespace d = nj2k::detail;
    Writer main;
    main.u16(0xFF4F).segment(0xFF51, s.body()).segment(0xFF52, c.body()).segment(0xFF53, coc_body(2, 1));
    main.segment(0xFF5C, qcd_none(16));
    const std::size_t sot = main.b.size();
    Cod tile_cod = c;
    tile_cod.levels = 4;
    Writer tph;
    tph.segment(0xFF53, coc_body(0, 3)).segment(0xFF52, tile_cod.body());
    main.u16(0xFF90).u16(10).u16(0).u32(std::uint32_t(12 + tph.b.size() + 2)).u8(0).u8(1).bytes(tph.b).u16(0xFF93);
    d::Allocator sys = d::Allocator::system();
    d::Arena arena;
    arena.init(&sys);
    d::Messages msg;
    d::HeaderSettings hs{};
    d::Source src(main.b.data(), main.b.size());
    d::MainHeader* mh = nullptr;
    CHECK(d::read_main_header(src, arena, msg, hs, mh) == NJ2K_OK && mh && mh->end == sot);
    d::TilePartHeader tp{};
    CHECK(mh && d::read_tile_part_header(src, arena, msg, hs, *mh, sot, tp) == NJ2K_OK);
    if (mh) {
        const d::TileOverrides* t = mh->overrides(0);
        CHECK(t != nullptr);
        d::TileComponentParams p0{}, p1{}, p2{};
        CHECK(d::resolve_tile_component(*mh, t, 0, msg, p0) == NJ2K_OK && p0.style->numresolutions == 4);
        CHECK(d::resolve_tile_component(*mh, t, 1, msg, p1) == NJ2K_OK && p1.style->numresolutions == 5);
        CHECK(d::resolve_tile_component(*mh, t, 2, msg, p2) == NJ2K_OK && p2.style->numresolutions == 5);
        d::TileComponentParams m2{};
        CHECK(d::resolve_tile_component(*mh, nullptr, 2, msg, m2) == NJ2K_OK && m2.style->numresolutions == 2);
        CHECK(tp.tile == 0 && tp.data == main.b.size() && tp.data_length == 0);
    }
}

// W8: a quantization with fewer step sizes than its component's sub-bands is refused; OpenJPEG
// takes it and dequantizes with step sizes of 0 (doc/departures.md).
void test_quantization_counts()
{
    Siz s;
    Cod c;  // 6 resolutions: 16 sub-bands
    {
        Read r;
        read_header(r, codestream(s, c, {}, true));
        CHECK(r.ok);
    }
    {
        Writer w;
        w.u16(0xFF4F).segment(0xFF51, s.body()).segment(0xFF52, c.body()).segment(0xFF5C, qcd_none(10));
        w.u16(0xFF90).u16(10).u16(0).u32(14).u8(0).u8(1).u16(0xFF93).u16(0xFFD9);
        Read r;
        read_header(r, w.b);
        CHECK(!r.ok && r.status == NJ2K_E_HEADER);
        auto oracle = make_oracle_backend();
        Request rq;
        rq.data = w.b.data();
        rq.size = w.b.size();
        CHECK(oracle->header(rq).ok);
    }
    {
        // a QCC that is short for its component, and a derived quantization (one step size)
        Writer w;
        Writer qcc;
        qcc.u8(1).bytes(qcd_none(4));
        w.u16(0xFF4F).segment(0xFF51, s.body()).segment(0xFF52, c.body()).segment(0xFF5C, qcd_none(16));
        w.segment(0xFF5D, qcc.b).u16(0xFF90).u16(10).u16(0).u32(14).u8(0).u8(1).u16(0xFF93).u16(0xFFD9);
        Read r;
        read_header(r, w.b);
        CHECK(!r.ok && r.status == NJ2K_E_HEADER);
        Writer derived;
        derived.u8(0x41).u16(0x4800);
        Writer v;
        v.u16(0xFF4F).segment(0xFF51, s.body()).segment(0xFF52, c.body()).segment(0xFF5C, derived.b);
        v.u16(0xFF90).u16(10).u16(0).u32(14).u8(0).u8(1).u16(0xFF93).u16(0xFFD9);
        Read d;
        read_header(d, v.b);
        CHECK(d.ok);
        nj2k_codestream_info_v2_t* info = d.ok ? nj2k_get_cstr_info(d.codec) : nullptr;
        CHECK(info && info->m_default_tile_info.tccp_info[0].qntsty == 1 &&
              info->m_default_tile_info.tccp_info[0].stepsizes_expn[0] == 9 &&
              info->m_default_tile_info.tccp_info[0].stepsizes_mant[0] == 0);
        nj2k_destroy_cstr_info(&info);
    }
}

// Header rules where neo-j2k follows the standard and OpenJPEG does not (doc/departures.md DP-7,
// DP-8): unknown marker segments are skipped by their length, Part 2 markers and HT mixed
// code-blocks are not supported, and invalid values OpenJPEG lets through are refused.
void test_header_rules()
{
    Siz s;
    Cod c;
    auto main_with = [&](const Bytes& extra) { return codestream(s, c, extra); };
    {
        // an unknown marker segment (0xFF6A) in the main header: skipped by its length
        Writer w;
        w.segment(0xFF6A, Bytes{0xFF, 0x90, 0x00, 0x0A});
        Read r;
        read_header(r, main_with(w.b));
        CHECK_MSG(r.ok, "%s", r.errors.c_str());
    }
    {
        // a Part 2 marker (DCO, 0xFF70): not supported
        Writer w;
        w.segment(0xFF70, Bytes{0x00, 0x00});
        Read r;
        read_header(r, main_with(w.b));
        CHECK(!r.ok && r.status == NJ2K_E_UNSUPPORTED);
    }
    {
        // EOC, SOD or a tile-part marker in the main header
        Writer eoc;
        eoc.u16(0xFFD9);
        Read r;
        read_header(r, main_with(eoc.b));
        CHECK(!r.ok && r.status == NJ2K_E_HEADER);
        Writer plt;
        plt.segment(0xFF58, Bytes{0x00, 0x05});
        Read p;
        read_header(p, main_with(plt.b));
        CHECK(!p.ok && p.status == NJ2K_E_HEADER);
    }
    {
        // an unknown progression order and a reserved quantization style: refused, where OpenJPEG
        // reads them (DP-8)
        Cod bad = c;
        bad.prg = 5;
        const Bytes prg = codestream(s, bad);
        Read r;
        read_header(r, prg);
        CHECK(!r.ok && r.status == NJ2K_E_HEADER);
        CHECK(oracle_read(prg, false));
        Writer q;
        q.u16(0xFF4F).segment(0xFF51, s.body()).segment(0xFF52, c.body()).segment(0xFF5C, Bytes{0x43, 0x48, 0x00});
        q.u16(0xFF90).u16(10).u16(0).u32(14).u8(0).u8(1).u16(0xFF93).u16(0xFFD9);
        Read rq;
        read_header(rq, q.b);
        CHECK(!rq.ok && rq.status == NJ2K_E_HEADER);
        CHECK(oracle_read(q.b, false));
    }
    {
        // refused by both (no departure): a SIZ with no components, and PPM markers whose Nppm
        // fields add up to no packet header bytes (OpenJPEG's opj_malloc(0) gives NULL for them)
        Siz none = s;
        none.comps = 0;
        const Bytes no_comps = codestream(none, c);
        Read rn;
        read_header(rn, no_comps);
        CHECK(!rn.ok && rn.status == NJ2K_E_HEADER);
        CHECK(!oracle_read(no_comps, false));
        Writer ppm;
        ppm.segment(0xFF60, Bytes{0x00, 0x00, 0x00, 0x00, 0x00}).segment(0xFF60, Bytes{0x01, 0x00, 0x00, 0x00, 0x00});
        const Bytes empty_ppm = main_with(ppm.b);
        Read rp;
        read_header(rp, empty_ppm);
        CHECK_MSG(!rp.ok && rp.status == NJ2K_E_HEADER, "status %d", rp.status);
        CHECK(!oracle_read(empty_ppm, false));
        // with one packet header byte the same markers read, in both
        Writer one;
        one.segment(0xFF60, Bytes{0x00, 0x00, 0x00, 0x00, 0x01, 0x80});
        const Bytes one_ppm = main_with(one.b);
        Read ro;
        read_header(ro, one_ppm);
        CHECK_MSG(ro.ok, "%s", ro.errors.c_str());
        CHECK(oracle_read(one_ppm, false));
    }
    {
        // HT code-blocks: the header reads (stage 1c refuses the decode until the HT module);
        // HT mixed code-blocks are not supported, as in OpenJPEG
        Cod ht = c;
        ht.style = 0x40;
        Read r;
        read_header(r, codestream(s, ht));
        CHECK(r.ok);
        ht.style = 0xC0;
        Read m;
        read_header(m, codestream(s, ht));
        CHECK(!m.ok && m.status == NJ2K_E_UNSUPPORTED);
    }
    {
        // SIZ must follow SOC: an unknown marker segment before it is refused, where OpenJPEG scans
        // past it for the SIZ and reads the header (DP-7)
        Writer w;
        w.u16(0xFF4F).segment(0xFF6A, Bytes{0x00, 0x01}).segment(0xFF51, s.body()).segment(0xFF52, c.body());
        w.segment(0xFF5C, qcd_none(16)).u16(0xFF90).u16(10).u16(0).u32(14).u8(0).u8(1).u16(0xFF93).u16(0xFFD9);
        Read r;
        read_header(r, w.b);
        CHECK(!r.ok && r.status == NJ2K_E_HEADER);
        CHECK(oracle_read(w.b, false));
    }
    {
        // a tile-part header with an unknown marker segment: skipped by its length (OpenJPEG fails
        // the tile-part)
        namespace d = nj2k::detail;
        Writer w;
        w.u16(0xFF4F).segment(0xFF51, s.body()).segment(0xFF52, c.body()).segment(0xFF5C, qcd_none(16));
        const std::size_t sot = w.b.size();
        Writer tph;
        tph.segment(0xFF6A, Bytes{1, 2, 3});
        w.u16(0xFF90).u16(10).u16(0).u32(std::uint32_t(12 + tph.b.size() + 2 + 1)).u8(0).u8(1).bytes(tph.b);
        w.u16(0xFF93).u8(0x00).u16(0xFFD9);
        d::Allocator sys = d::Allocator::system();
        d::Arena arena;
        arena.init(&sys);
        d::Messages msg;
        d::HeaderSettings hs{};
        d::Source src(w.b.data(), w.b.size());
        d::MainHeader* mh = nullptr;
        d::TilePartHeader tp{};
        CHECK(d::read_main_header(src, arena, msg, hs, mh) == NJ2K_OK &&
              d::read_tile_part_header(src, arena, msg, hs, *mh, sot, tp) == NJ2K_OK && tp.data_length == 1);
        // the same marker longer than Psot allows
        w.b[sot + 6] = 0;
        w.b[sot + 7] = 0;
        w.b[sot + 8] = 0;
        w.b[sot + 9] = 16;
        d::Source again(w.b.data(), w.b.size());
        d::Arena arena2;
        arena2.init(&sys);
        CHECK(d::read_main_header(again, arena2, msg, hs, mh) == NJ2K_OK &&
              d::read_tile_part_header(again, arena2, msg, hs, *mh, sot, tp) == NJ2K_E_HEADER);
    }
}

// Tile-part headers of every corpus file, read in order, against the test scanner: positions, Psot,
// data lengths (Psot = 0: to the end, less an EOC the codestream ends with, plan W5).
void test_tile_parts()
{
    namespace d = nj2k::detail;
    std::size_t files = 0, parts = 0;
    for (const auto& [name, data] : corpus_files()) {
        const auto smh = parse_main_header(data);
        if (!smh) {
            continue;
        }
        const auto expected = scan_tile_parts(data, *smh);
        d::Allocator sys = d::Allocator::system();
        d::Arena arena;
        arena.init(&sys);
        d::Messages msg;
        d::HeaderSettings hs{};
        d::Source src(data.data(), data.size());
        d::MainHeader* mh = nullptr;
        if (d::read_main_header(src, arena, msg, hs, mh) != NJ2K_OK) {
            CHECK_MSG(false, "%s: main header", name.c_str());
            continue;
        }
        ++files;
        std::uint64_t sot = mh->end;
        std::size_t i = 0;
        for (;; ++i) {
            d::TilePartHeader tp{};
            const nj2k_status_t st = d::read_tile_part_header(src, arena, msg, hs, *mh, sot, tp);
            CHECK_MSG(st == NJ2K_OK, "%s tile-part %zu: %d", name.c_str(), i, st);
            if (st != NJ2K_OK) {
                break;
            }
            ++parts;
            const bool eoc = data.size() >= 2 && data[data.size() - 2] == 0xFF && data[data.size() - 1] == 0xD9;
            const std::uint64_t length = tp.psot ? tp.data_length : d::psot0_data_length(data.size() - tp.data, eoc);
            CHECK_MSG(i < expected.size() && expected[i].sot == tp.sot && expected[i].data == tp.data &&
                          expected[i].tile == tp.tile && expected[i].psot == tp.psot && expected[i].end == tp.data + length,
                      "%s tile-part %zu", name.c_str(), i);
            const std::uint64_t next = tp.data + length;
            if (next + 2 > data.size() || !src.skip(next - src.tell())) {
                break;
            }
            std::uint8_t m[2];
            if (src.read(m, 2) != 2 || m[0] != 0xFF || m[1] != 0x90) {
                break;  // EOC, or the end
            }
            sot = next;
        }
        CHECK_MSG(i + 1 == expected.size(), "%s: %zu tile-parts, the scanner %zu", name.c_str(), i + 1, expected.size());
        // the packets a tile declares, for the work budgets of stage 1c
        std::vector<d::TileComponentParams> params(mh->numcomps);
        CHECK(d::resolve_tile(*mh, mh->overrides(0), msg, params.data()) == NJ2K_OK &&
              d::tile_packets(*mh, 0, params.data()) > 0);
    }
    CHECK(files > 50);
    std::printf("  tile-parts: %zu files, %zu tile-part headers\n", files, parts);
}

// The image dimensions neo-j2k will decode, against the oracle's decodes: every corpus file at every
// reduce level, the factor given before the header and after it (OpenJPEG sizes the image
// differently in the two orders when the origin is odd).
void test_decoded_dimensions()
{
    auto oracle = make_oracle_backend();
    std::size_t compared = 0, refused = 0;
    for (const auto& [name, data] : corpus_files()) {
        if (data.size() > 40000) {
            continue;
        }
        for (int reduce = 0; reduce <= 5; ++reduce) {
            for (bool after : {false, true}) {
                Request rq;
                rq.data = data.data();
                rq.size = data.size();
                rq.reduce = reduce;
                rq.reduce_after_header = after;
                const Outcome o = oracle->decode(rq);
                Read r;
                Options opt;
                opt.reduce = after ? -1 : reduce;
                read_header(r, data, opt);
                bool ok = r.ok;
                if (ok && after && reduce > 0) {
                    ok = nj2k_set_decoded_resolution_factor(r.codec, std::uint32_t(reduce)) != 0;
                }
                if (ok) {
                    ok = nj2k_get_decoded_dimensions(r.codec, r.image) != 0;
                }
                if (!o.ok) {
                    // refused by the header or the reduce factor: neo-j2k too; a refusal while
                    // decoding is stage 1c's (the oracle's known defects among them)
                    if (o.stage == "header") {
                        CHECK_MSG(!ok, "%s r%d %s: the oracle refuses the header, neo-j2k does not", name.c_str(),
                                  reduce, after ? "after" : "before");
                        ++refused;
                    }
                    continue;
                }
                CHECK_MSG(ok, "%s r%d %s: neo-j2k refuses (%s)", name.c_str(), reduce, after ? "after" : "before",
                          r.errors.c_str());
                if (!ok) {
                    continue;
                }
                ++compared;
                CHECK(r.image->numcomps == o.numcomps && o.comps.size() == o.numcomps);
                for (std::uint32_t c = 0; c < r.image->numcomps && c < o.comps.size(); ++c) {
                    const auto& n = r.image->comps[c];
                    const auto& e = o.comps[c];
                    CHECK_MSG(n.w == e.w && n.h == e.h && n.x0 == e.x0 && n.y0 == e.y0 && n.factor == e.factor,
                              "%s r%d %s comp %u: %ux%u at %u,%u f%u, the oracle %ux%u at %u,%u f%u", name.c_str(),
                              reduce, after ? "after" : "before", c, n.w, n.h, n.x0, n.y0, n.factor, e.w, e.h, e.x0,
                              e.y0, e.factor);
                }
            }
        }
    }
    std::printf("  decoded dimensions: %zu decodes compared with the oracle's, %zu refused by both\n", compared, refused);
    CHECK(compared > 400);
}

// W4 and S1: the reduce policy, and the setter after the header.
void test_reduce_policy()
{
    struct File {
        const char* name;
        std::uint32_t resolutions;
    };
    for (const File& f : {File{"size_1x1_g.j2k", 1}, File{"size_4x4_rgb_r3.j2k", 3}, File{"size_16x16_rgb_r5.j2k", 5},
                          File{"size_512x16_rgb_97_r5.j2k", 5}}) {
        const auto data = read_file(g_corpus + "/" + f.name);
        CHECK(data.has_value());
        if (!data) {
            continue;
        }
        const auto mh = parse_main_header(*data);
        for (std::uint32_t reduce = 0; reduce <= 5; ++reduce) {
            for (std::uint32_t policy : {std::uint32_t(NJ2K_REDUCE_EXACT), std::uint32_t(NJ2K_REDUCE_CLAMP)}) {
                Read r;
                Options o;
                o.reduce = int(reduce);
                o.policy = policy;
                read_header(r, *data, o);
                const bool fits = reduce < f.resolutions;
                if (policy == NJ2K_REDUCE_EXACT && !fits) {
                    CHECK_MSG(!r.ok && r.status == NJ2K_E_LIMIT, "%s r%u exact", f.name, reduce);
                    continue;
                }
                CHECK_MSG(r.ok, "%s r%u: %s", f.name, reduce, r.errors.c_str());
                if (!r.ok || !mh) {
                    continue;
                }
                const std::uint32_t used = fits ? reduce : f.resolutions - 1;
                CHECK(r.image->comps[0].factor == used);
                const std::uint32_t w = nj2k::detail::ceildivpow2(std::uint32_t(mh->width()), used);
                const std::uint32_t h = nj2k::detail::ceildivpow2(std::uint32_t(mh->height()), used);
                CHECK_MSG(r.image->comps[0].w == w && r.image->comps[0].h == h, "%s r%u: %ux%u", f.name, reduce,
                          r.image->comps[0].w, r.image->comps[0].h);
            }
        }
        // S1: after the header a factor at the resolution count is refused and changes nothing
        Read r;
        read_header(r, *data);
        CHECK(r.ok);
        if (r.ok) {
            CHECK(!nj2k_set_decoded_resolution_factor(r.codec, f.resolutions));
            CHECK(nj2k_get_status(r.codec) == NJ2K_E_LIMIT);
            CHECK(nj2k_get_decoded_dimensions(r.codec, r.image) && r.image->comps[0].factor == 0);
            if (f.resolutions > 1) {
                CHECK(nj2k_set_decoded_resolution_factor(r.codec, 1));
                CHECK(nj2k_get_decoded_dimensions(r.codec, r.image) && r.image->comps[0].factor == 1);
            }
            CHECK(nj2k_decoder_set_reduce_policy(r.codec, NJ2K_REDUCE_CLAMP));
            CHECK(nj2k_set_decoded_resolution_factor(r.codec, 30));
        }
        // before the header the factor is kept and the call answers false, as OpenJPEG's
        nj2k::Codec codec = nj2k::make_decoder();
        CHECK(!nj2k_set_decoded_resolution_factor(codec.get(), 1) && nj2k_get_status(codec.get()) == NJ2K_E_STATE);
    }
}

// The allocator hook: every allocation of the codec goes through it and comes back; a failure at
// any of them fails cleanly (NO_MEMORY), and the codec can still be destroyed; a budget that the
// header state passes is a limit.
void test_allocator()
{
    const auto data = read_file(g_corpus + "/poc_lrcp_rlcp.j2k");
    CHECK(data.has_value());
    if (!data) {
        return;
    }
    Counting all;
    {
        nj2k_allocator_t hook = all.hook();
        Options o;
        o.allocator = &hook;
        Read r;
        read_header(r, *data, o);
        CHECK(r.ok);
        nj2k_codestream_info_v2_t* info = nj2k_get_cstr_info(r.codec);
        CHECK(info != nullptr);
        nj2k_destroy_cstr_info(&info);
    }
    CHECK(all.allocs > 1 && all.allocs == all.frees && all.live == 0);
    std::printf("  allocator: a header read takes %zu allocations, %zu bytes at most\n", all.allocs, all.peak);
    for (std::size_t k = 0; k < all.allocs; ++k) {
        Counting c;
        c.fail_at = k;
        nj2k_allocator_t hook = c.hook();
        Options o;
        o.allocator = &hook;
        Read r;
        read_header(r, *data, o);
        CHECK_MSG(!r.ok && (k == 0 ? r.codec == nullptr : r.status == NJ2K_E_NO_MEMORY), "failing allocation %zu: %d", k,
                  r.status);
        nj2k_destroy_codec(r.codec);
        r.codec = nullptr;
        CHECK_MSG(c.live == 0, "failing allocation %zu leaks %zu bytes", k, c.live);
    }
    // an incomplete hook makes no codec
    nj2k_allocator_t bad{sizeof(nj2k_allocator_t), nullptr, Counting::release, nullptr};
    CHECK(nj2k_create_decompress_with_allocator(NJ2K_CODEC_J2K, &bad) == nullptr);
    bad.allocate = Counting::alloc;
    bad.struct_size = 8;
    CHECK(nj2k_create_decompress_with_allocator(NJ2K_CODEC_J2K, &bad) == nullptr);
    CHECK(nj2k_create_decompress_with_allocator(NJ2K_CODEC_J2K, nullptr) == nullptr);
    CHECK(nj2k_create_decompress(NJ2K_CODEC_JPT) == nullptr);
    // a memory budget the header state does not fit
    nj2k_decoder_limits_t tiny{};
    tiny.max_memory = 1024;
    Read r;
    Options o;
    o.limits = &tiny;
    read_header(r, *data, o);
    CHECK(!r.ok && r.status == NJ2K_E_LIMIT);
}

// The header model owns its bytes (plan audit D5): with the input freed after the header read, the
// codestream information, the dimensions and the component selection still work, and the packed
// packet headers (PPM, merged, and their markers; PPT) still hold what a read of a copy that stays
// alive holds. The freed input is overwritten first, so a pointer into it gives other bytes in any
// build; AddressSanitizer builds also see the read itself.
void test_owned_bytes()
{
    const auto file = read_file(g_corpus + "/packed_ppm.j2k");
    const auto ppt_file = read_file(g_corpus + "/packed_ppt.j2k");
    CHECK(file.has_value() && ppt_file.has_value());
    if (!file || !ppt_file) {
        return;
    }
    auto freed_copy = [](const Bytes& b) {
        auto* copy = static_cast<std::uint8_t*>(std::malloc(b.size()));
        std::memcpy(copy, b.data(), b.size());
        return copy;
    };
    auto release = [](std::uint8_t* copy, std::size_t size) {
        std::memset(copy, 0xA5, size);
        std::free(copy);
    };
    std::uint8_t* copy = freed_copy(*file);
    nj2k::Codec codec = nj2k::make_decoder();
    nj2k_image_t* image = nullptr;
    {
        nj2k::MemoryReader reader(std::span<const std::uint8_t>(copy, file->size()));
        auto stream = reader.make_stream();
        CHECK(nj2k_read_header(stream.get(), codec.get(), &image));
    }
    release(copy, file->size());
    nj2k_codestream_info_v2_t* info = nj2k_get_cstr_info(codec.get());
    CHECK(info && info->nbcomps == 3 && info->m_default_tile_info.numlayers == 3);
    nj2k_destroy_cstr_info(&info);
    const NJ2K_UINT32 comps[2] = {2, 0};
    CHECK(nj2k_set_decoded_components(codec.get(), 2, comps, NJ2K_FALSE));
    CHECK(nj2k_set_decoded_resolution_factor(codec.get(), 2));
    CHECK(nj2k_get_decoded_dimensions(codec.get(), image) && image->comps[0].w == 12 && image->comps[0].h == 10);
    nj2k_image_destroy(image);

    // the packed packet headers, through the reader stage 1c reads them with
    namespace d = nj2k::detail;
    d::Allocator sys = d::Allocator::system();
    d::Messages msg;
    d::HeaderSettings hs{};
    auto same_chunks = [](const d::ArenaList<d::PackedChunk>& a, const d::ArenaList<d::PackedChunk>& b) {
        if (a.size != b.size || a.size == 0) {
            return false;
        }
        for (std::uint32_t i = 0; i < a.size; ++i) {
            if (a.data[i].z != b.data[i].z || a.data[i].size != b.data[i].size ||
                std::memcmp(a.data[i].data, b.data[i].data, a.data[i].size) != 0) {
                return false;
            }
        }
        return true;
    };
    {
        d::Arena kept_arena, arena;
        kept_arena.init(&sys);
        arena.init(&sys);
        d::Source kept_src(file->data(), file->size());
        d::MainHeader* kept = nullptr;
        CHECK(d::read_main_header(kept_src, kept_arena, msg, hs, kept) == NJ2K_OK && kept && kept->ppm_size > 0);
        std::uint8_t* input = freed_copy(*file);
        d::Source src(input, file->size());
        d::MainHeader* mh = nullptr;
        CHECK(d::read_main_header(src, arena, msg, hs, mh) == NJ2K_OK && mh);
        release(input, file->size());
        if (kept && mh) {
            CHECK(mh->ppm_size == kept->ppm_size && std::memcmp(mh->ppm_data, kept->ppm_data, std::size_t(kept->ppm_size)) == 0);
            CHECK(same_chunks(mh->ppm_chunks, kept->ppm_chunks));
            CHECK(mh->ppm_lengths.size == kept->ppm_lengths.size && mh->ppm_lengths.size > 0);
        }
    }
    {
        d::Arena kept_arena, arena;
        kept_arena.init(&sys);
        arena.init(&sys);
        d::Source kept_src(ppt_file->data(), ppt_file->size());
        d::MainHeader* kept = nullptr;
        d::TilePartHeader tp{};
        CHECK(d::read_main_header(kept_src, kept_arena, msg, hs, kept) == NJ2K_OK && kept &&
              d::read_tile_part_header(kept_src, kept_arena, msg, hs, *kept, kept->end, tp) == NJ2K_OK);
        std::uint8_t* input = freed_copy(*ppt_file);
        d::Source src(input, ppt_file->size());
        d::MainHeader* mh = nullptr;
        CHECK(d::read_main_header(src, arena, msg, hs, mh) == NJ2K_OK && mh &&
              d::read_tile_part_header(src, arena, msg, hs, *mh, mh->end, tp) == NJ2K_OK);
        release(input, ppt_file->size());
        const d::TileOverrides* a = mh ? mh->overrides(tp.tile) : nullptr;
        const d::TileOverrides* b = kept ? kept->overrides(tp.tile) : nullptr;
        CHECK(a && b && same_chunks(a->ppt, b->ppt));
    }
}

// Tile-part markers cost the same whatever order they come in (the audit's finding 5): the tile-parts
// of 65,535 tiles in descending tile order, each with an RGN marker; and one tile of 16,384
// components with a COC marker for each in descending order, and a second one for some. Every
// tile and component resolves to its last marker, the whole-tile resolution gives what resolving
// each component alone does, and the header state stays within a few bytes per input byte.
void test_tile_directory()
{
    namespace d = nj2k::detail;
    d::Allocator sys = d::Allocator::system();
    d::Messages msg;
    d::HeaderSettings hs{};
    auto read_all = [&](const Bytes& cs, d::Arena& arena, d::MainHeader*& mh) {
        d::Source src(cs.data(), cs.size());
        if (d::read_main_header(src, arena, msg, hs, mh) != NJ2K_OK) {
            return std::size_t(0);
        }
        std::size_t parts = 0;
        std::uint64_t sot = mh->end;
        for (;;) {
            d::TilePartHeader tp{};
            if (d::read_tile_part_header(src, arena, msg, hs, *mh, sot, tp) != NJ2K_OK) {
                return std::size_t(0);
            }
            ++parts;
            sot = tp.data + tp.data_length;
            std::uint8_t m[2];
            if (!src.skip(sot - src.tell()) || src.read(m, 2) != 2 || m[0] != 0xFF || m[1] != 0x90) {
                return parts;
            }
        }
    };
    {
        Siz s;
        s.x1 = 255;
        s.y1 = 257;
        s.tdx = s.tdy = 1;
        s.comps = 1;
        Cod c;
        c.levels = 0;
        Writer w;
        w.u16(0xFF4F).segment(0xFF51, s.body()).segment(0xFF52, c.body()).segment(0xFF5C, qcd_none(1));
        for (int t = 65534; t >= 0; --t) {
            w.u16(0xFF90).u16(10).u16(unsigned(t)).u32(12 + 7 + 2).u8(0).u8(1);
            w.segment(0xFF5E, Bytes{0, 0, std::uint8_t(t % 37)});
            w.u16(0xFF93);
        }
        w.u16(0xFFD9);
        d::Arena arena;
        arena.init(&sys);
        d::MainHeader* mh = nullptr;
        CHECK(read_all(w.b, arena, mh) == 65535);
        if (mh) {
            CHECK(mh->tiles.size == 65535);
            std::size_t right = 0;
            for (std::uint32_t t = 0; t < 65535; ++t) {
                d::TileComponentParams one{}, all{};
                const d::TileOverrides* o = mh->overrides(t);
                right += o && o->tile == t && d::resolve_tile_component(*mh, o, 0, msg, one) == NJ2K_OK &&
                         d::resolve_tile(*mh, o, msg, &all) == NJ2K_OK && one.roishift == t % 37 &&
                         all.roishift == t % 37;
            }
            CHECK_MSG(right == 65535, "%zu tiles", right);
            CHECK_MSG(arena.reserved() <= 16 * w.b.size(), "%llu bytes from %zu", (unsigned long long)arena.reserved(),
                      w.b.size());
        }
    }
    {
        constexpr std::uint32_t n = 16384;
        Siz s;
        s.x1 = s.y1 = 16;
        s.comps = n;
        Cod c;  // 6 resolutions; a COC gives component k (k % 6) + 1, or (k + 3) % 6 + 1 the second time
        Writer tph;
        auto coc = [&](std::uint32_t k, unsigned levels) {
            tph.segment(0xFF53, Bytes{std::uint8_t(k >> 8), std::uint8_t(k & 0xFF), 0, std::uint8_t(levels), 4, 4, 0, 1});
        };
        for (std::uint32_t k = n; k-- > 0;) {
            coc(k, k % 6);
        }
        for (std::uint32_t k = 0; k < n; k += 41) {
            coc(k, (k + 3) % 6);
        }
        Writer w;
        w.u16(0xFF4F).segment(0xFF51, s.body()).segment(0xFF52, c.body()).segment(0xFF5C, qcd_none(16));
        w.u16(0xFF90).u16(10).u16(0).u32(std::uint32_t(12 + tph.b.size() + 2)).u8(0).u8(1).bytes(tph.b).u16(0xFF93);
        w.u16(0xFFD9);
        d::Arena arena;
        arena.init(&sys);
        d::MainHeader* mh = nullptr;
        CHECK(read_all(w.b, arena, mh) == 1);
        const d::TileOverrides* o = mh ? mh->overrides(0) : nullptr;
        CHECK(o != nullptr);
        if (mh && o) {
            std::vector<d::TileComponentParams> all(n);
            CHECK(d::resolve_tile(*mh, o, msg, all.data()) == NJ2K_OK);
            std::size_t right = 0, alone = 0, sampled = 0;
            for (std::uint32_t k = 0; k < n; ++k) {
                const unsigned levels = k % 41 == 0 ? (k + 3) % 6 : k % 6;
                right += all[k].style->numresolutions == levels + 1;
                // a component alone is looked for through the tile's markers: a sample of them
                if (k % 97 == 0 || k % 41 == 0) {
                    ++sampled;
                    d::TileComponentParams one{};
                    alone += d::resolve_tile_component(*mh, o, k, msg, one) == NJ2K_OK && one.style == all[k].style &&
                             one.quant == all[k].quant && one.coding == all[k].coding;
                }
            }
            CHECK_MSG(right == n, "%zu components", right);
            CHECK_MSG(alone == sampled, "%zu of %zu", alone, sampled);
            CHECK_MSG(arena.reserved() <= 16 * w.b.size(), "%llu bytes from %zu", (unsigned long long)arena.reserved(),
                      w.b.size());
        }
    }
}

// The rest of the API's order and argument rules.
void test_api_rules()
{
    const auto data = read_file(g_corpus + "/sl_32_rgb_97.j2k");
    CHECK(data.has_value());
    if (!data) {
        return;
    }
    Read r;
    read_header(r, *data);
    CHECK(r.ok);
    nj2k::MemoryReader reader(*data);
    auto stream = reader.make_stream();
    nj2k_image_t* again = nullptr;
    CHECK(!nj2k_read_header(stream.get(), r.codec, &again) && again == nullptr);
    CHECK(nj2k_get_status(r.codec) == NJ2K_E_STATE);
    nj2k_dparameters_t p;
    nj2k_set_default_decoder_parameters(&p);
    CHECK(!nj2k_setup_decoder(r.codec, &p) && nj2k_get_status(r.codec) == NJ2K_E_STATE);
    const NJ2K_UINT32 bad[2] = {0, 0}, out_of_range[1] = {3}, good[1] = {1};
    CHECK(!nj2k_set_decoded_components(r.codec, 2, bad, NJ2K_FALSE));
    CHECK(!nj2k_set_decoded_components(r.codec, 1, out_of_range, NJ2K_FALSE));
    CHECK(!nj2k_set_decoded_components(r.codec, 1, good, NJ2K_TRUE));
    CHECK(nj2k_set_decoded_components(r.codec, 1, good, NJ2K_FALSE));
    CHECK(nj2k_set_decoded_components(r.codec, 0, nullptr, NJ2K_FALSE));
    // decoding comes in stage 1c
    CHECK(!nj2k_decode(r.codec, stream.get(), r.image) && nj2k_get_status(r.codec) == NJ2K_E_UNSUPPORTED);
    // the component selection: repeated calls take no more memory, and a refused one leaves the
    // earlier selection, as OpenJPEG does; seen through the working memory of a 2,048 x 2,048
    // three-component image under a 32 MiB limit, which fits one component and not three
    {
        Siz big;
        big.x1 = big.y1 = 2048;
        const Bytes cs = codestream(big, Cod{});
        nj2k_decoder_limits_t tight{};
        tight.max_memory = 32ull << 20;
        Counting count;
        nj2k_allocator_t hook = count.hook();
        Options o;
        o.allocator = &hook;
        o.limits = &tight;
        Read sel;
        read_header(sel, cs, o);
        CHECK(sel.ok && !nj2k_get_decoded_dimensions(sel.codec, sel.image) &&
              nj2k_get_status(sel.codec) == NJ2K_E_LIMIT);
        const NJ2K_UINT32 one[1] = {1}, twice[2] = {2, 2}, past_last[1] = {3};
        CHECK(nj2k_set_decoded_components(sel.codec, 1, one, NJ2K_FALSE));
        const std::size_t allocs = count.allocs;
        for (int i = 0; i < 1000; ++i) {
            const NJ2K_UINT32 pick[1] = {NJ2K_UINT32(i % 3)};
            CHECK(nj2k_set_decoded_components(sel.codec, 1, pick, NJ2K_FALSE));
        }
        CHECK_MSG(count.allocs == allocs, "%zu allocations, %zu before", count.allocs, allocs);
        CHECK(nj2k_set_decoded_components(sel.codec, 1, one, NJ2K_FALSE));
        CHECK(!nj2k_set_decoded_components(sel.codec, 2, twice, NJ2K_FALSE));
        CHECK(!nj2k_set_decoded_components(sel.codec, 1, past_last, NJ2K_FALSE));
        CHECK_MSG(nj2k_get_decoded_dimensions(sel.codec, sel.image), "%s", sel.errors.c_str());
        CHECK(nj2k_set_decoded_components(sel.codec, 0, nullptr, NJ2K_FALSE));
        CHECK(!nj2k_get_decoded_dimensions(sel.codec, sel.image) && nj2k_get_status(sel.codec) == NJ2K_E_LIMIT);
    }
    CHECK(!nj2k_read_header(nullptr, r.codec, &again));
    nj2k::Codec fresh = nj2k::make_decoder();
    CHECK(!nj2k_get_cstr_info(fresh.get()) && nj2k_get_status(fresh.get()) == NJ2K_E_STATE);
    CHECK(!nj2k_set_decoded_components(fresh.get(), 1, good, NJ2K_FALSE));
    CHECK(!nj2k_get_decoded_dimensions(fresh.get(), r.image));
    CHECK(!nj2k_read_header(stream.get(), fresh.get(), nullptr) && nj2k_get_status(fresh.get()) == NJ2K_E_ARGUMENT);
    nj2k_destroy_codec(nullptr);
    CHECK(!nj2k_set_error_handler(nullptr, nullptr, nullptr));
    // a failed codec stays failed
    Read cut;
    read_header(cut, data->data(), 20);
    CHECK(cut.status == NJ2K_E_NEED_DATA);
    CHECK(!nj2k_set_decoded_resolution_factor(cut.codec, 1) && nj2k_get_status(cut.codec) == NJ2K_E_STATE);
    // file streams
    std::string tmp = env_value("TEMP");
    if (tmp.empty()) {
        tmp = env_value("TMPDIR");
    }
    const std::string path = (tmp.empty() ? std::string(".") : tmp) + "/nj2k_header_unit_file.j2k";
    CHECK(write_file(path, *data));
    nj2k_stream_t* fs_stream = nj2k_stream_create_default_file_stream(path.c_str(), NJ2K_TRUE);
    CHECK(fs_stream != nullptr);
    nj2k::Codec fc = nj2k::make_decoder();
    nj2k_image_t* fimg = nullptr;
    CHECK(fs_stream && nj2k_read_header(fs_stream, fc.get(), &fimg) && fimg->comps[0].w == 32);
    nj2k_image_destroy(fimg);
    nj2k_stream_destroy(fs_stream);
    std::remove(path.c_str());
    CHECK(nj2k_stream_create_default_file_stream((path + ".missing").c_str(), NJ2K_TRUE) == nullptr);
    // a read function that answers 0 ends the stream (OpenJPEG would call it for ever)
    nj2k_stream_t* zero = nj2k_stream_create(64, NJ2K_TRUE);
    nj2k_stream_set_read_function(zero, [](void*, NJ2K_SIZE_T, void*) -> NJ2K_SIZE_T { return 0; });
    nj2k::Codec zc = nj2k::make_decoder();
    nj2k_image_t* zimg = nullptr;
    CHECK(!nj2k_read_header(zero, zc.get(), &zimg) && nj2k_get_status(zc.get()) == NJ2K_E_NEED_DATA);
    nj2k_stream_destroy(zero);
}

// Mutated headers: every byte of the main header of every corpus file, flipped in eight ways, then
// read with no limits and with the Second Life profile. Every read ends with a status, a refusal
// leaves no image, and the allocator gets back everything it gave.
void test_mutations()
{
    nj2k_decoder_limits_t sl{};
    nj2k_decoder_limits_init(&sl, NJ2K_DECODER_LIMITS_SL_TEXTURE);
    std::size_t reads = 0, read_ok = 0;
    for (const auto& [name, data] : corpus_files()) {
        const auto mh = parse_main_header(data);
        if (!mh) {
            continue;
        }
        Bytes m = data;
        for (std::size_t at = 0; at < mh->end + 2; ++at) {
            for (std::uint8_t x : {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80}) {
                m[at] ^= x;
                const nj2k_decoder_limits_t* const profiles[2] = {nullptr, &sl};
                for (const nj2k_decoder_limits_t* lim : profiles) {
                    Counting c;
                    nj2k_allocator_t hook = c.hook();
                    Options o;
                    o.allocator = &hook;
                    o.limits = lim;
                    {
                        Read r;
                        read_header(r, m, o);
                        ++reads;
                        read_ok += r.ok;
                        CHECK(r.ok == (r.status == NJ2K_OK));
                        CHECK(r.ok || r.image == nullptr);
                        if (r.ok) {
                            nj2k_get_decoded_dimensions(r.codec, r.image);
                            nj2k_codestream_info_v2_t* info = nj2k_get_cstr_info(r.codec);
                            nj2k_destroy_cstr_info(&info);
                        }
                    }
                    CHECK(c.live == 0);
                }
                m[at] ^= x;
            }
        }
    }
    std::printf("  mutations: %zu header reads, %zu read\n", reads, read_ok);
}

}  // namespace

int main(int argc, char** argv)
{
    console_reports();
    if (argc < 4) {
        std::fprintf(stderr, "usage: nj2k_header_unit <corpus dir> <fuzz bombs dir> <known-defects dir>\n");
        return 2;
    }
    (void)single_thread_openjpeg();
    g_corpus = argv[1];
    test_limits_api();
    test_need_data();
    test_jp2();
    test_varying_depth();
    test_probe_rows();
    test_bombs(argv[2], argv[3]);
    test_precedence();
    test_quantization_counts();
    test_header_rules();
    test_tile_parts();
    test_decoded_dimensions();
    test_reduce_policy();
    test_allocator();
    test_owned_bytes();
    test_tile_directory();
    test_api_rules();
    test_mutations();
    std::printf("header_unit: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

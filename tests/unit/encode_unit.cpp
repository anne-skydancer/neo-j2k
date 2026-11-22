// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// Stage 6a: the encoder, byte-identical to OpenJPEG for the Second Life viewer's settings (owner
// decision D9). The viewer's upload encode (JPEG2KEncode::encode: RLCP, 64 x 64 code-blocks, no
// precincts, one tile, 9/7 with rate layers by surface or 5/3 lossless, the colour transform from
// three components, fewer resolutions under 128 px, the comment of the caller) is replayed against
// neo-j2k through the compatibility header and against the oracle, and every codestream must be the
// oracle's byte for byte: over generated textures of every shape the viewer sends, over the images
// the corpus decodes to, and over the settings the viewer varies (layers, rates, resolutions, colour
// transform, comment). A refusal must be a refusal on both sides. Then the round trip (neo-j2k's
// codestreams decode, in both decoders, to the samples encoded or to the lossy decode the oracle's
// stream gives), what the core encoder refuses with NJ2K_E_UNSUPPORTED (the rest of OpenJPEG's
// settings belong to the j2k-extra module), its limits, state errors, retries after a refusal, output
// streams of every buffer size and a host that takes a few bytes a call, output failures, and
// departures DP-16 to DP-19.
#include <neoj2k/neoj2k.h>

#include <algorithm>
#include <cstdio>
#include <climits>
#include <cstdlib>
#include <limits>
#include <cstring>
#include <random>
#include <tuple>
#include <utility>
#include <string>
#include <vector>

#include "common.h"
#include "decoder.h"
#include "thread_pool.h"

using namespace nj2k::test;

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
std::mt19937 g_rng(0x6a6a6a);

using Raw = std::vector<std::uint8_t>;

// textures of every kind: smooth with noise, flat, black, white, one-pixel checks, noise, edges, stripes
Raw make_raw(int w, int h, int comps, int kind)
{
    Raw raw(std::size_t(w) * h * comps);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < comps; ++c) {
                int v = 0;
                switch (kind) {
                case 0: {
                    const int edge = ((x / 97 + y / 61 + c) & 1) * 60;
                    v = (x * (c + 1) / 9 + y / (c + 2) + edge + static_cast<int>(g_rng() % 23)) & 0xFF;
                    break;
                }
                case 1:
                    v = 77 + c * 40;
                    break;
                case 2:
                    v = 0;
                    break;
                case 3:
                    v = 255;
                    break;
                case 4:
                    v = ((x + y + c) & 1) ? 255 : 0;
                    break;
                case 5:
                    v = static_cast<int>(g_rng() & 0xFF);
                    break;
                case 6:
                    v = x < w / 3 ? 20 + c : 230 - c;
                    break;
                default:
                    v = ((y / 3) & 1 ? 200 : 40) + static_cast<int>(g_rng() % 5) + c;
                    break;
                }
                raw[(std::size_t(y) * w + x) * comps + c] = static_cast<std::uint8_t>(v);
            }
        }
    }
    return raw;
}

const char* kind_name(int kind)
{
    static const char* names[] = {"texture", "flat", "black", "white", "checks", "noise", "edge", "stripes"};
    return names[kind & 7];
}

// both encoders on one spec: the same bytes, or both refuse. Returns whether the oracle encoded.
bool same_codestream(const UploadSpec& us, const char* what)
{
    Bytes a, b;
    static const bool trace = !env_value("NJ2K_TEST_TRACE").empty();
    if (trace) {
        std::printf("%s %dx%d c%d %s layers %d rates %zu numres %d mct %d\n", what, us.width, us.height, us.comps,
                    us.reversible ? "5/3" : "9/7", us.layers, us.rates.size(), us.numres, us.mct);
    }
    const bool oa = oracle_upload(us, a);
    const bool ob = neoj2k_upload(us, b);
    CHECK_MSG(oa == ob, "%s %dx%d c%d %s: the oracle %s, neo-j2k %s", what, us.width, us.height, us.comps,
              us.reversible ? "5/3" : "9/7", oa ? "encodes" : "refuses", ob ? "encodes" : "refuses");
    if (oa && ob) {
        CHECK_MSG(a == b, "%s %dx%d c%d %s: %zu bytes against the oracle's %zu%s", what, us.width, us.height, us.comps,
                  us.reversible ? "5/3" : "9/7", b.size(), a.size(),
                  a.size() == b.size() ? " (same size, other bytes)" : "");
    }
    return oa;
}

// Every shape the viewer sends: powers of two and the sizes between (bakes are 512 x 512 and
// 1024 x 1024 with five components; uploads are scaled to powers of two but local files and the
// auction preview are not), one to five components, lossless and lossy.
void test_viewer_matrix()
{
    std::size_t encoded = 0, compared_bytes = 0;
    const int sides[] = {4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 48, 63, 64, 65, 96, 100, 127, 128, 129, 191, 255, 256, 257};
    for (const int w : sides) {
        for (const int h : sides) {
            // the full product over the small sides, a sample of it over the larger ones
            if ((w > 100 || h > 100) && ((w * 31 + h * 17) % 4) != 0) {
                continue;
            }
            for (const int comps : {1, 3, 4, 5}) {
                for (const bool reversible : {true, false}) {
                    const int kind = static_cast<int>((w + h + comps + (reversible ? 1 : 0)) % 8);
                    const Raw raw = make_raw(w, h, comps, kind);
                    UploadSpec us;
                    us.raw = raw.data();
                    us.width = w;
                    us.height = h;
                    us.comps = comps;
                    us.reversible = reversible;
                    if (same_codestream(us, kind_name(kind))) {
                        ++encoded;
                    }
                }
            }
        }
    }
    // one to three samples a side: the viewer's stream has no buffer then (its size guess is 0, which
    // OpenJPEG's stream_create refuses and neo-j2k's too), so both refuse
    for (const auto& dims : {std::pair<int, int>{1, 1}, {3, 64}, {2, 2}, {100, 3}, {1, 100}, {3, 3}}) {
        const Raw raw = make_raw(dims.first, dims.second, 3, 0);
        UploadSpec us;
        us.raw = raw.data();
        us.width = dims.first;
        us.height = dims.second;
        us.comps = 3;
        us.reversible = true;
        CHECK_MSG(!same_codestream(us, "tiny"), "%dx%d", dims.first, dims.second);
    }
    // components the viewer's rule leaves out of the main matrix: 2, 6 and 8
    for (const int comps : {2, 6, 8}) {
        for (const int w : {33, 100}) {
            for (const bool reversible : {true, false}) {
                const int h = w * 3 / 4 + 5;
                const Raw raw = make_raw(w, h, comps, comps + w);
                UploadSpec us;
                us.raw = raw.data();
                us.width = w;
                us.height = h;
                us.comps = comps;
                us.reversible = reversible;
                if (same_codestream(us, "components")) {
                    ++encoded;
                }
            }
        }
    }
    // the limits: 4,096 samples a side, wide and tall
    for (const bool reversible : {true, false}) {
        for (const auto& dims : {std::pair<int, int>{4096, 16}, std::pair<int, int>{16, 4096}}) {
            const Raw raw = make_raw(dims.first, dims.second, 3, 0);
            UploadSpec us;
            us.raw = raw.data();
            us.width = dims.first;
            us.height = dims.second;
            us.comps = 3;
            us.reversible = reversible;
            CHECK_MSG(same_codestream(us, "limit side"), "%dx%d", dims.first, dims.second);
            ++encoded;
        }
    }
    // each kind of texture at a few sizes, both modes
    for (int kind = 0; kind < 8; ++kind) {
        for (const int w : {31, 64, 100, 256, 333}) {
            for (const bool reversible : {true, false}) {
                const int h = w * 3 / 4 + 1;
                const Raw raw = make_raw(w, h, 3, kind);
                UploadSpec us;
                us.raw = raw.data();
                us.width = w;
                us.height = h;
                us.comps = 3;
                us.reversible = reversible;
                if (same_codestream(us, kind_name(kind))) {
                    ++encoded;
                }
            }
        }
    }
    // the sizes of uploads and bakes: 512 and 1024 squares with four and five components, and larger
    // textures, whose lossy layers use the 1, 5 and 6 layer rules
    const struct {
        int w, h, comps;
    } big[] = {{512, 512, 5}, {512, 512, 3}, {512, 256, 4}, {640, 480, 3}, {1024, 1024, 3}, {1024, 768, 4},
               {2048, 512, 3}, {512, 2048, 1}, {1500, 900, 3}, {2048, 2048, 3}};
    for (const auto& im : big) {
        for (const bool reversible : {true, false}) {
            const Raw raw = make_raw(im.w, im.h, im.comps, 0);
            UploadSpec us;
            us.raw = raw.data();
            us.width = im.w;
            us.height = im.h;
            us.comps = im.comps;
            us.reversible = reversible;
            if (same_codestream(us, "large")) {
                ++encoded;
            }
        }
    }
    (void)compared_bytes;
    std::printf("viewer settings: %zu codestreams byte for byte the oracle's\n", encoded);
}

// the image each corpus file decodes to, encoded as the viewer would
void test_corpus_images()
{
    auto oracle = make_oracle_backend();
    const auto manifest = read_file(join_path(g_corpus, "MANIFEST.tsv"));
    CHECK(manifest.has_value());
    if (!manifest) {
        return;
    }
    std::size_t files = 0, encoded = 0;
    std::string text(manifest->begin(), manifest->end());
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t eol = text.find('\n', at);
        if (eol == std::string::npos) {
            eol = text.size();
        }
        std::string line = text.substr(at, eol - at);
        at = eol + 1;
        const std::size_t tab = line.find('\t');
        std::string name = line.substr(0, tab);
        if (name.size() < 5 || name.substr(name.size() - 4) != ".j2k") {
            continue;
        }
        const auto data = read_file(join_path(g_corpus, name));
        if (!data) {
            continue;
        }
        Request rq;
        rq.data = data->data();
        rq.size = data->size();
        rq.mode = Mode::Plain;
        rq.capture = true;
        const Outcome o = oracle->decode(rq);
        if (!o.ok || o.numcomps == 0 || o.numcomps > 5 || o.planes.size() != o.numcomps) {
            continue;
        }
        bool fits = true;
        for (const auto& c : o.comps) {
            fits = fits && c.w == o.comps[0].w && c.h == o.comps[0].h && c.prec == 8 && c.sgnd == 0 && c.dx == 1 &&
                   c.dy == 1;
        }
        const int w = static_cast<int>(o.comps[0].w), h = static_cast<int>(o.comps[0].h);
        if (!fits || w < 4 || h < 4 || std::size_t(w) * h > 600 * 600) {
            continue;
        }
        const int comps = static_cast<int>(o.numcomps);
        Raw raw(std::size_t(w) * h * comps);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                for (int c = 0; c < comps; ++c) {
                    const std::int32_t v = o.planes[std::size_t(c)][std::size_t(h - 1 - y) * w + x];
                    raw[(std::size_t(y) * w + x) * comps + c] = static_cast<std::uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
                }
            }
        }
        ++files;
        for (const bool reversible : {true, false}) {
            UploadSpec us;
            us.raw = raw.data();
            us.width = w;
            us.height = h;
            us.comps = comps;
            us.reversible = reversible;
            if (same_codestream(us, name.c_str())) {
                ++encoded;
            }
        }
    }
    CHECK_MSG(files >= 10, "only %zu corpus images", files);
    std::printf("corpus: %zu images, %zu codestreams byte for byte the oracle's\n", files, encoded);
}

// the settings the viewer varies and the ones its tools could: layers and rates, resolutions, the
// colour transform, the comment
void test_settings()
{
    std::size_t encoded = 0;
    const Raw rgb = make_raw(200, 120, 3, 0);
    const Raw rgba = make_raw(130, 130, 4, 0);
    const Raw grey = make_raw(65, 190, 1, 0);
    struct Case {
        const Raw* raw;
        int w, h, comps;
    } images[] = {{&rgb, 200, 120, 3}, {&rgba, 130, 130, 4}, {&grey, 65, 190, 1}};
    const std::vector<std::vector<float>> rate_sets = {
        {8.0f},
        {32.0f, 8.0f},
        {512.0f, 128.0f, 32.0f, 8.0f},
        {1.0f},                    // lossless by the rule
        {0.0f, 0.0f},
        {100.0f, 1.0f},            // the last layer lossless
        {4.0f, 8.0f},              // not decreasing
        {20.0f, 20.0f, 20.0f},     // equal
        {2.0f, 1.5f, 1.0f, 0.5f},
        {1e6f, 1e3f, 10.0f},
        {8192.0f, 2048.0f, 512.0f, 128.0f, 32.0f, 8.0f},
        {3.0f, 2.5f, 2.0f, 1.75f, 1.5f, 1.25f, 1.125f, 1.0625f},
    };
    for (const Case& im : images) {
        for (const bool reversible : {true, false}) {
            for (std::size_t r = 0; r < rate_sets.size(); ++r) {
                UploadSpec us;
                us.raw = im.raw->data();
                us.width = im.w;
                us.height = im.h;
                us.comps = im.comps;
                us.reversible = reversible;
                us.layers = static_cast<int>(rate_sets[r].size());
                us.rates = rate_sets[r];
                if (same_codestream(us, "rates")) {
                    ++encoded;
                }
            }
            for (const int numres : {1, 2, 3, 4, 5, 6, 7, 8}) {
                UploadSpec us;
                us.raw = im.raw->data();
                us.width = im.w;
                us.height = im.h;
                us.comps = im.comps;
                us.reversible = reversible;
                us.numres = numres;
                if (same_codestream(us, "resolutions")) {
                    ++encoded;
                }
            }
            for (const int mct : {0, 1}) {
                if (mct == 1 && im.comps < 3) {
                    continue;  // OpenJPEG reads components that are not there; neo-j2k refuses it (DP-18)
                }
                UploadSpec us;
                us.raw = im.raw->data();
                us.width = im.w;
                us.height = im.h;
                us.comps = im.comps;
                us.reversible = reversible;
                us.mct = mct;
                if (same_codestream(us, mct ? "mct on" : "mct off")) {
                    ++encoded;
                }
            }
            for (const char* comment : {"", "x", "Vulkanstorm RGBHM", static_cast<const char*>(nullptr)}) {
                UploadSpec us;
                us.raw = im.raw->data();
                us.width = im.w;
                us.height = im.h;
                us.comps = im.comps;
                us.reversible = reversible;
                us.comment = comment;
                if (same_codestream(us, "comment")) {
                    ++encoded;
                }
            }
            {
                const std::string longer(300, 'c');
                UploadSpec us;
                us.raw = im.raw->data();
                us.width = im.w;
                us.height = im.h;
                us.comps = im.comps;
                us.reversible = reversible;
                us.comment = longer.c_str();
                if (same_codestream(us, "long comment")) {
                    ++encoded;
                }
            }
        }
    }
    // a codestream size the caller asks for lifts the rates (the same on both sides)
    for (const int max_cs : {1500, 6000, 40000}) {
        UploadSpec us;
        us.raw = rgb.data();
        us.width = 200;
        us.height = 120;
        us.comps = 3;
        us.reversible = false;
        us.max_cs_size = max_cs;
        CHECK_MSG(same_codestream(us, "max_cs_size"), "max_cs_size %d", max_cs);
        us.layers = 3;
        us.rates = {40.0f, 20.0f, 10.0f};
        CHECK_MSG(same_codestream(us, "max_cs_size layers"), "max_cs_size %d", max_cs);
    }
    // rates no one means: not a number, infinite, negative, huge (OpenJPEG takes them as it takes the rest)
    for (const float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -5.0f,
                            -std::numeric_limits<float>::infinity(), 1e30f, 1e-30f}) {
        for (const bool reversible : {true, false}) {
            UploadSpec us;
            us.raw = rgb.data();
            us.width = 200;
            us.height = 120;
            us.comps = 3;
            us.reversible = reversible;
            us.layers = 3;
            us.rates = {bad, 20.0f, bad};
            (void)same_codestream(us, "hostile rate");
            us.rates = {40.0f, 20.0f, bad};
            (void)same_codestream(us, "hostile last rate");
            us.layers = 1;
            us.rates = {bad};
            (void)same_codestream(us, "hostile only rate");
        }
    }
    // comments at the edge of Lcom: OpenJPEG writes them correctly up to 65,531 bytes
    for (const std::size_t n : {std::size_t(65529), std::size_t(65530), std::size_t(65531)}) {
        const std::string text(n, 'k');
        UploadSpec us;
        us.raw = rgb.data();
        us.width = 200;
        us.height = 120;
        us.comps = 3;
        us.reversible = true;
        us.comment = text.c_str();
        CHECK_MSG(same_codestream(us, "long comment"), "comment of %zu bytes", n);
    }
    // noise, the least compressible texture: both encode alike (no image of these settings has been found
    // to outgrow the buffer OpenJPEG guesses for the codestream, 1.4 times the raw size, so that path is
    // ported but not reached)
    for (const bool reversible : {true, false}) {
        const Raw noise = make_raw(64, 64, 4, 5);
        UploadSpec us;
        us.raw = noise.data();
        us.width = 64;
        us.height = 64;
        us.comps = 4;
        us.reversible = reversible;
        (void)same_codestream(us, "noise");
    }
    std::printf("settings: %zu codestreams byte for byte the oracle's\n", encoded);
}

// neo-j2k's codestreams decode: lossless to the samples encoded, lossy to what the oracle's identical
// stream gives, in the oracle and in neo-j2k
void test_round_trip()
{
    auto oracle = make_oracle_backend();
    auto neo = make_neoj2k_backend();
    std::size_t checked = 0;
    for (const int w : {4, 9, 64, 100, 257}) {
        for (const int comps : {1, 3, 4, 5}) {
            for (const bool reversible : {true, false}) {
                const int h = w * 2 / 3 + 4;
                const Raw raw = make_raw(w, h, comps, 0);
                UploadSpec us;
                us.raw = raw.data();
                us.width = w;
                us.height = h;
                us.comps = comps;
                us.reversible = reversible;
                Bytes cs;
                if (!neoj2k_upload(us, cs)) {
                    continue;
                }
                Request rq;
                rq.data = cs.data();
                rq.size = cs.size();
                rq.mode = Mode::Plain;
                rq.capture = true;
                const Outcome o = oracle->decode(rq);
                const Outcome n = neo->decode(rq);
                CHECK_MSG(o.ok && n.ok && o.hash == n.hash, "%dx%d c%d: the decoders disagree on neo-j2k's stream", w, h,
                          comps);
                if (reversible && o.ok) {
                    bool same = o.planes.size() == std::size_t(comps);
                    for (int c = 0; same && c < comps; ++c) {
                        for (int y = 0; same && y < h; ++y) {
                            for (int x = 0; x < w; ++x) {
                                // the image's rows are the raw's bottom row first
                                const int v = raw[(std::size_t(h - 1 - y) * w + x) * comps + c];
                                if (o.planes[std::size_t(c)][std::size_t(y) * w + x] != v) {
                                    same = false;
                                    break;
                                }
                            }
                        }
                    }
                    CHECK_MSG(same, "%dx%d c%d: the lossless round trip differs", w, h, comps);
                }
                ++checked;
            }
        }
    }
    std::printf("round trip: %zu codestreams decode alike in both decoders (lossless ones to the samples)\n", checked);
}

// ---- what the core encoder refuses, its limits and its states ----

struct Sink {
    std::vector<std::uint8_t> bytes;
    std::size_t fail_after = SIZE_MAX;  // the write function fails once this many bytes went in
    std::size_t per_call = SIZE_MAX;    // the most bytes it takes a call
};

NJ2K_SIZE_T sink_write(void* buf, NJ2K_SIZE_T n, void* user)
{
    auto* s = static_cast<Sink*>(user);
    if (s->bytes.size() + n > s->fail_after) {
        return static_cast<NJ2K_SIZE_T>(-1);
    }
    if (n > s->per_call) {
        n = s->per_call;
    }
    s->bytes.insert(s->bytes.end(), static_cast<std::uint8_t*>(buf), static_cast<std::uint8_t*>(buf) + n);
    return n;
}

struct Messages {
    int errors = 0;
    std::string last;
};

void on_error(const char* msg, void* user)
{
    auto* m = static_cast<Messages*>(user);
    ++m->errors;
    m->last = msg ? msg : "";
}

struct Job {
    nj2k_cparameters_t p;
    nj2k_image_t* image = nullptr;
    nj2k_codec_t* codec = nullptr;
    nj2k_stream_t* stream = nullptr;
    Sink sink;
    Messages msgs;

    explicit Job(int w = 40, int h = 40, int comps = 3, std::uint32_t prec = 8)
    {
        nj2k_set_default_encoder_parameters(&p);
        p.prog_order = NJ2K_RLCP;
        p.cp_disto_alloc = 1;
        p.tcp_numlayers = 1;
        p.tcp_rates[0] = 0.0f;
        p.cp_comment = const_cast<char*>("test");
        p.tcp_mct = comps >= 3 ? 1 : 0;
        std::vector<nj2k_image_cmptparm_t> parms(static_cast<std::size_t>(comps));
        for (auto& c : parms) {
            std::memset(&c, 0, sizeof(c));
            c.prec = prec;
            c.dx = c.dy = 1;
            c.w = static_cast<NJ2K_UINT32>(w);
            c.h = static_cast<NJ2K_UINT32>(h);
        }
        image = nj2k_image_create(static_cast<NJ2K_UINT32>(comps), parms.data(), NJ2K_CLRSPC_SRGB);
        if (image) {
            image->x1 = static_cast<NJ2K_UINT32>(w);
            image->y1 = static_cast<NJ2K_UINT32>(h);
            for (int c = 0; c < comps; ++c) {
                for (int i = 0; i < w * h; ++i) {
                    image->comps[c].data[i] = (i * 7 + c * 31) & 0xFF;
                }
            }
        }
        codec = nj2k_create_compress(NJ2K_CODEC_J2K);
        if (codec) {
            nj2k_set_error_handler(codec, on_error, &msgs);
            nj2k_set_warning_handler(codec, on_error, &msgs);
        }
        stream = nj2k_stream_create(1, NJ2K_FALSE);  // one byte: every byte reaches the sink as it is written
        if (stream) {
            nj2k_stream_set_user_data(stream, &sink, nullptr);
            nj2k_stream_set_write_function(stream, sink_write);
        }
    }
    ~Job()
    {
        if (stream) {
            nj2k_stream_destroy(stream);
        }
        if (codec) {
            nj2k_destroy_codec(codec);
        }
        if (image) {
            nj2k_image_destroy(image);
        }
    }
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;
    bool setup() { return nj2k_setup_encoder(codec, &p, image) != 0; }
    bool run()
    {
        return setup() && nj2k_start_compress(codec, image, stream) && nj2k_encode(codec, stream) &&
               nj2k_end_compress(codec, stream);
    }
    nj2k_status_t status() const { return nj2k_get_status(codec); }
};

template <class F>
void expect_unsupported(const char* what, F&& tweak)
{
    Job j;
    CHECK(j.codec && j.image && j.stream);
    if (!j.codec || !j.image || !j.stream) {
        return;
    }
    tweak(j);
    CHECK_MSG(!j.run(), "%s: the core encoder took it", what);
    CHECK_MSG(j.status() == NJ2K_E_UNSUPPORTED, "%s: status %s", what, nj2k_status_string(j.status()));
    CHECK_MSG(j.msgs.errors > 0, "%s: no message", what);
    CHECK_MSG(j.sink.bytes.empty(), "%s: bytes written", what);
}

void test_unsupported()
{
    // the settings of OpenJPEG's encoder the viewer does not use: j2k-extra's
    expect_unsupported("tiles", [](Job& j) {
        j.p.tile_size_on = NJ2K_TRUE;
        j.p.cp_tdx = j.p.cp_tdy = 32;
    });
    expect_unsupported("a tile offset", [](Job& j) { j.p.cp_tx0 = 1; });
    expect_unsupported("fixed layers", [](Job& j) { j.p.cp_fixed_alloc = 1; });
    expect_unsupported("fixed quality", [](Job& j) { j.p.cp_fixed_quality = 1; });
    expect_unsupported("sop", [](Job& j) { j.p.csty = 2; });
    expect_unsupported("eph", [](Job& j) { j.p.csty = 4; });
    expect_unsupported("precincts", [](Job& j) { j.p.csty = 1; });
    expect_unsupported("LRCP", [](Job& j) { j.p.prog_order = NJ2K_LRCP; });
    expect_unsupported("RPCL", [](Job& j) { j.p.prog_order = NJ2K_RPCL; });
    expect_unsupported("32 x 32 blocks", [](Job& j) { j.p.cblockw_init = j.p.cblockh_init = 32; });
    expect_unsupported("64 x 32 blocks", [](Job& j) { j.p.cblockh_init = 32; });
    expect_unsupported("bypass", [](Job& j) { j.p.mode = 1; });
    expect_unsupported("termall", [](Job& j) { j.p.mode = 4; });
    expect_unsupported("a profile", [](Job& j) { j.p.rsiz = NJ2K_PROFILE_CINEMA_2K; });
    expect_unsupported("cinema", [](Job& j) { j.p.cp_cinema = NJ2K_CINEMA2K_24; });
    expect_unsupported("max_comp_size", [](Job& j) { j.p.max_comp_size = 1000; });
    expect_unsupported("a tile y offset", [](Job& j) { j.p.cp_ty0 = 2; });
    expect_unsupported("a component y offset", [](Job& j) { j.image->comps[0].y0 = 1; });
    expect_unsupported("vertical subsampling", [](Job& j) { j.image->comps[1].dy = 2; });
    expect_unsupported("progression order changes", [](Job& j) { j.p.numpocs = 1; });
    expect_unsupported("the MCT profile", [](Job& j) { j.p.cp_rsiz = NJ2K_MCT; });
    expect_unsupported("tile-parts", [](Job& j) { j.p.tp_on = 1; });
    expect_unsupported("a custom transform", [](Job& j) {
        static float mct[9 + 3] = {};
        j.p.mct_data = mct;
    });
    expect_unsupported("tcp_mct 2", [](Job& j) { j.p.tcp_mct = 2; });
    expect_unsupported("a region of interest", [](Job& j) {
        j.p.roi_compno = 0;
        j.p.roi_shift = 5;
    });
    expect_unsupported("16-bit samples", [](Job& j) {
        Job k(40, 40, 3, 16);
        std::swap(j.image, k.image);
    });
    expect_unsupported("signed samples", [](Job& j) { j.image->comps[0].sgnd = 1; });
    expect_unsupported("subsampling", [](Job& j) { j.image->comps[1].dx = 2; });
    expect_unsupported("an image offset", [](Job& j) { j.image->x0 = 4; });
    expect_unsupported("components of other sizes", [](Job& j) { j.image->comps[2].w = 20; });
    for (const char* option : {"PLT=YES", "TLM=YES", "GUARD_BITS=3", "GUARD_BITS=0"}) {
        Job j;
        CHECK(j.setup());
        const char* opts[] = {option, nullptr};
        CHECK_MSG(!nj2k_encoder_set_extra_options(j.codec, opts), "%s", option);
        CHECK_MSG(j.status() == NJ2K_E_UNSUPPORTED, "%s: %s", option, nj2k_status_string(j.status()));
    }
    {
        Job j;
        CHECK(j.setup());
        const char* ok_opts[] = {"PLT=NO", "TLM=NO", "GUARD_BITS=2", nullptr};
        CHECK(nj2k_encoder_set_extra_options(j.codec, ok_opts));
        for (const char* bad : {"NOPE=1", "PLT=MAYBE", "TLM=", "GUARD_BITS=9", "GUARD_BITS=-1"}) {
            const char* bad_opts[] = {bad, nullptr};
            CHECK_MSG(!nj2k_encoder_set_extra_options(j.codec, bad_opts), "%s", bad);
            CHECK_MSG(j.status() == NJ2K_E_ARGUMENT, "%s: %s", bad, nj2k_status_string(j.status()));
        }
        CHECK(j.run() == false);  // the encoder was set up already: a second setup is a state error
        CHECK(j.status() == NJ2K_E_STATE);
    }
    // JP2 and JPT output
    CHECK(nj2k_create_compress(NJ2K_CODEC_JP2) == nullptr);
    CHECK(nj2k_create_compress(NJ2K_CODEC_JPT) == nullptr);
    // writing a tile at a time, custom matrices
    {
        Job j;
        CHECK(!nj2k_write_tile(j.codec, 0, nullptr, 0, j.stream));
        CHECK(!nj2k_set_MCT(&j.p, nullptr, nullptr, 0));
    }
    // what the core takes does encode
    {
        Job j;
        CHECK(j.run());
        CHECK(j.sink.bytes.size() > 100 && j.sink.bytes[0] == 0xFF && j.sink.bytes[1] == 0x4F);
        CHECK(j.sink.bytes[j.sink.bytes.size() - 2] == 0xFF && j.sink.bytes.back() == 0xD9);
        CHECK(j.status() == NJ2K_OK);
    }
}

void test_limits_and_states()
{
    {
        Job j(4100, 4, 1);
        CHECK(j.codec && j.image);
        CHECK(!j.run());
        CHECK_MSG(j.status() == NJ2K_E_LIMIT, "%s", nj2k_status_string(j.status()));
        CHECK(j.sink.bytes.empty());
    }
    {
        Job j(8, 8, 9);
        CHECK(!j.run());
        CHECK_MSG(j.status() == NJ2K_E_LIMIT, "%s", nj2k_status_string(j.status()));
        CHECK(j.sink.bytes.empty());
    }
    {
        Job j;
        j.p.tcp_numlayers = 101;
        CHECK(!j.run());
        CHECK(j.status() == NJ2K_E_LIMIT);
        CHECK(j.sink.bytes.empty());
    }
    {
        Job j;
        j.p.numresolution = 0;
        CHECK(!j.run());
        CHECK(j.status() == NJ2K_E_ARGUMENT);
        CHECK(j.sink.bytes.empty());
    }
    for (const int bad : {-1, 34, 100}) {
        Job j;
        j.p.numresolution = bad;
        CHECK_MSG(!j.run() && j.status() == NJ2K_E_ARGUMENT && j.sink.bytes.empty(), "numresolution %d", bad);
    }
    for (const int deep : {32, 33}) {
        // no tile holds 2^31 samples across: refused at setup without shifting a signed 1 by 31 (OpenJPEG's UBSan
        // finding in opj_j2k_encoding_validation)
        Job j;
        j.p.numresolution = deep;
        CHECK_MSG(!j.setup() && j.status() == NJ2K_E_ARGUMENT && j.sink.bytes.empty(), "numresolution %d", deep);
    }
    {
        Job j;
        j.p.tcp_numlayers = -1;
        CHECK(!j.run() && j.status() == NJ2K_E_ARGUMENT && j.sink.bytes.empty());
    }
    {
        // more resolutions than the image holds (OpenJPEG refuses at its start): refused at setup, nothing written;
        // the caller lowers numresolution and the codestream is the oracle's
        Job j(20, 20, 3);
        j.p.numresolution = 6;
        CHECK(!j.setup());
        CHECK(j.status() == NJ2K_E_ARGUMENT);
        CHECK(j.sink.bytes.empty());
        j.p.numresolution = 3;
        CHECK(j.run());
        Raw raw(20 * 20 * 3);
        for (int y = 0; y < 20; ++y) {
            for (int x = 0; x < 20; ++x) {
                for (int c = 0; c < 3; ++c) {
                    raw[(std::size_t(19 - y) * 20 + x) * 3 + c] = static_cast<std::uint8_t>(j.image->comps[c].data[y * 20 + x]);
                }
            }
        }
        UploadSpec us;
        us.raw = raw.data();
        us.width = us.height = 20;
        us.comps = 3;
        us.reversible = true;
        us.numres = 3;
        us.comment = "test";
        Bytes theirs;
        CHECK(oracle_upload(us, theirs));
        CHECK(theirs == j.sink.bytes);
    }
    {
        Job j;
        j.p.tcp_mct = 1;
        Job k(40, 40, 2);
        std::swap(j.image, k.image);
        CHECK(!j.setup());
        CHECK(j.status() == NJ2K_E_ARGUMENT);
    }
    // calls out of order
    {
        Job j;
        CHECK(!nj2k_start_compress(j.codec, j.image, j.stream));
        CHECK(j.status() == NJ2K_E_STATE);
        CHECK(!nj2k_encode(j.codec, j.stream));
        CHECK(j.status() == NJ2K_E_STATE);
        CHECK(!nj2k_end_compress(j.codec, j.stream));
        CHECK(j.status() == NJ2K_E_STATE);
        CHECK(j.setup());
        CHECK(!j.setup());
        CHECK(j.status() == NJ2K_E_STATE);
        CHECK(nj2k_start_compress(j.codec, j.image, j.stream));
        CHECK(!nj2k_end_compress(j.codec, j.stream));
        CHECK(j.status() == NJ2K_E_STATE);
        CHECK(nj2k_encode(j.codec, j.stream));
        CHECK(!nj2k_encode(j.codec, j.stream));
        CHECK(nj2k_end_compress(j.codec, j.stream));
    }
    // an input stream, and none
    {
        Job j;
        CHECK(j.setup());
        nj2k_stream_t* in = nj2k_stream_default_create(NJ2K_TRUE);
        CHECK(!nj2k_start_compress(j.codec, j.image, in));
        CHECK(j.status() == NJ2K_E_ARGUMENT);
        nj2k_stream_destroy(in);
        CHECK(!nj2k_start_compress(j.codec, j.image, nullptr));
        CHECK(!nj2k_start_compress(nullptr, j.image, j.stream));
    }
    // the limits' edges, set on an image whose planes stay small (setup reads none): 4,096 samples a side and
    // 2^26 samples are taken, one more of either is NJ2K_E_LIMIT
    for (const auto& edge : {std::tuple<int, int, int, bool>{4096, 4096, 4, true}, {4097, 1, 1, false},
                             {1, 4097, 1, false}, {4096, 4096, 5, false}, {4096, 4096, 1, true}}) {
        Job j(8, 8, std::get<2>(edge));
        j.image->x1 = static_cast<NJ2K_UINT32>(std::get<0>(edge));
        j.image->y1 = static_cast<NJ2K_UINT32>(std::get<1>(edge));
        for (std::uint32_t c = 0; c < j.image->numcomps; ++c) {
            j.image->comps[c].w = j.image->x1;
            j.image->comps[c].h = j.image->y1;
        }
        const bool ok = j.setup();
        CHECK_MSG(ok == std::get<3>(edge), "%dx%dx%d", std::get<0>(edge), std::get<1>(edge), std::get<2>(edge));
        if (!ok) {
            CHECK_MSG(j.status() == NJ2K_E_LIMIT, "%dx%dx%d: %s", std::get<0>(edge), std::get<1>(edge),
                      std::get<2>(edge), nj2k_status_string(j.status()));
        }
        // the image's own sizes are restored before it is destroyed (its planes are the small ones)
        for (std::uint32_t c = 0; c < j.image->numcomps; ++c) {
            j.image->comps[c].w = j.image->comps[c].h = 8;
        }
    }
    // the codestream size written back saturates at INT_MAX (OpenJPEG's cast overflows at exactly 2^31)
    {
        Job j(8, 8, 4);
        j.image->x1 = j.image->y1 = 4096;
        for (std::uint32_t c = 0; c < 4; ++c) {
            j.image->comps[c].w = j.image->comps[c].h = 4096;
        }
        j.p.tcp_rates[0] = 0.03125f;
        j.p.tcp_mct = 0;
        CHECK(j.setup());
        CHECK_MSG(j.p.max_cs_size == INT_MAX, "%d", j.p.max_cs_size);
        for (std::uint32_t c = 0; c < 4; ++c) {
            j.image->comps[c].w = j.image->comps[c].h = 8;
        }
    }
    // streams: every buffer size and a host that takes a few bytes a call write the same codestream
    {
        Job ref;
        CHECK(ref.run());
        for (const std::size_t buffer : {std::size_t(1), std::size_t(16), std::size_t(4096)}) {
            for (const std::size_t per_call : {SIZE_MAX, std::size_t(7), std::size_t(1)}) {
                Job j;
                j.sink.per_call = per_call;
                nj2k_stream_destroy(j.stream);
                j.stream = nj2k_stream_create(buffer, NJ2K_FALSE);
                nj2k_stream_set_user_data(j.stream, &j.sink, nullptr);
                nj2k_stream_set_write_function(j.stream, sink_write);
                CHECK_MSG(j.run(), "buffer %zu, %zu a call", buffer, per_call);
                CHECK_MSG(j.sink.bytes == ref.sink.bytes, "buffer %zu, %zu a call: other bytes", buffer, per_call);
            }
        }
        // as OpenJPEG, a stream has a buffer
        CHECK(nj2k_stream_create(0, NJ2K_FALSE) == nullptr);
        CHECK(nj2k_stream_create(0, NJ2K_TRUE) == nullptr);
    }
    // a refusal leaves the encoder where it was: the caller mends the call and goes on
    {
        Job ref;
        CHECK(ref.run());
        Job j;
        j.p.tile_size_on = NJ2K_TRUE;
        CHECK(!j.setup());
        CHECK(j.status() == NJ2K_E_UNSUPPORTED);
        j.p.tile_size_on = NJ2K_FALSE;
        CHECK(j.setup());
        const NJ2K_INT32 good = j.image->comps[2].data[7];
        j.image->comps[2].data[7] = 300;
        CHECK(!nj2k_start_compress(j.codec, j.image, j.stream));
        CHECK(j.status() == NJ2K_E_ARGUMENT);
        j.image->comps[2].data[7] = good;
        CHECK(nj2k_start_compress(j.codec, j.image, j.stream));
        CHECK(!nj2k_end_compress(j.codec, j.stream));  // out of order: refused, the encode goes on
        CHECK(j.status() == NJ2K_E_STATE);
        CHECK(nj2k_encode(j.codec, j.stream));
        CHECK(nj2k_end_compress(j.codec, j.stream));
        CHECK(j.sink.bytes == ref.sink.bytes);
    }
    // the same parameters, written back by the first setup, serve a second encode (a codestream size
    // the last rate implies is in max_cs_size then)
    {
        Job a(64, 64, 3);
        a.p.irreversible = 2;  // any non-zero value is the 9/7
        a.p.tcp_numlayers = 2;
        a.p.tcp_rates[0] = 40.0f;
        a.p.tcp_rates[1] = 10.0f;
        CHECK(a.run());
        CHECK(a.p.max_cs_size > 0);
        Job b(64, 64, 3);
        b.p = a.p;
        CHECK(b.run());
    }
    // a size cap cuts the 5/3 too, as OpenJPEG's: no layers asked is one layer at rate 0, written back,
    // and the cap lifts that rate (3 x 64 x 64 x 8 bits in 5,000 bytes: 2.4576), written back as well.
    // The encode fuzz target took such a stream for a lossless one (fuzz/regressions/encode-size-cap-lossless.bin)
    {
        const auto columns = [](nj2k_image_t* image) {
            for (std::uint32_t c = 0; c < image->numcomps; ++c) {
                for (int i = 0; i < 64 * 64; ++i) {
                    image->comps[c].data[i] = (i & 1) ? 5 : 206;
                }
            }
        };
        Job none(64, 64, 3);
        columns(none.image);
        none.p.irreversible = 0;
        none.p.tcp_numlayers = 0;
        none.p.max_cs_size = 5000;
        CHECK(none.run());
        CHECK_MSG(none.p.tcp_numlayers == 1, "%d layers", none.p.tcp_numlayers);
        CHECK_MSG(none.p.tcp_rates[0] > 2.4575f && none.p.tcp_rates[0] < 2.4577f, "rate %g", none.p.tcp_rates[0]);
        Job one(64, 64, 3);
        columns(one.image);
        one.p.irreversible = 0;
        one.p.max_cs_size = 5000;
        CHECK(one.run());
        CHECK(one.sink.bytes == none.sink.bytes);
        // the oracle's encoder writes the same bytes, and its decoder gives other samples than the image's
        Raw raw(std::size_t(64) * 64 * 3);
        for (std::size_t i = 0; i < raw.size(); ++i) {
            raw[i] = ((i / 3) & 1) ? 5 : 206;
        }
        UploadSpec us;
        us.raw = raw.data();
        us.width = us.height = 64;
        us.comps = 3;
        us.reversible = true;
        us.numres = 1;  // the fuzz input's settings: one resolution, no colour transform
        us.mct = 0;
        us.max_cs_size = 5000;
        CHECK(same_codestream(us, "max_cs_size lossless"));
        Bytes cs;
        CHECK(oracle_upload(us, cs));
        Request rq;
        rq.data = cs.data();
        rq.size = cs.size();
        rq.mode = Mode::Plain;
        rq.capture = true;
        const Outcome o = make_oracle_backend()->decode(rq);
        CHECK(o.ok && o.planes.size() == 3);
        bool same = o.ok && o.planes.size() == 3;
        for (std::size_t c = 0; same && c < 3; ++c) {
            for (std::size_t i = 0; i < std::size_t(64) * 64; ++i) {
                if (o.planes[c][i] != ((i & 1) ? 5 : 206)) {
                    same = false;
                    break;
                }
            }
        }
        CHECK_MSG(!same, "the capped 5/3 stream of %zu bytes decodes to the image", cs.size());
    }
    // DP-16: samples outside the 8-bit range are refused where OpenJPEG's arithmetic would wrap
    {
        Job j;
        j.image->comps[1].data[5] = 256;
        CHECK(j.setup());
        CHECK(!nj2k_start_compress(j.codec, j.image, j.stream));
        CHECK(j.status() == NJ2K_E_ARGUMENT);
        CHECK(j.sink.bytes.empty());  // before anything is written
        // and the caller mends the sample and goes on: the same codestream as a clean run's
        j.image->comps[1].data[5] = 3;
        CHECK(nj2k_start_compress(j.codec, j.image, j.stream));
        CHECK(nj2k_encode(j.codec, j.stream));
        CHECK(nj2k_end_compress(j.codec, j.stream));
        Job fresh;
        fresh.image->comps[1].data[5] = 3;
        CHECK(fresh.run());
        CHECK(j.sink.bytes == fresh.sink.bytes);
    }
    {
        Job j;
        j.image->comps[0].data[0] = -1;
        CHECK(j.setup());
        CHECK(!nj2k_start_compress(j.codec, j.image, j.stream));
        CHECK(j.status() == NJ2K_E_ARGUMENT);
        CHECK(j.sink.bytes.empty());
    }
    // an output stream that fails: at the header, at the tile, at the end
    Job clean;
    CHECK(clean.run());
    const std::size_t clean_size = clean.sink.bytes.size();
    for (const std::size_t fail_after :
         {std::size_t(0), std::size_t(50), std::size_t(300), clean_size - 1, clean_size}) {
        Job j;
        j.sink.fail_after = fail_after;
        nj2k_stream_destroy(j.stream);
        j.stream = nj2k_stream_create(16, NJ2K_FALSE);  // a small buffer: the failure reaches the host at once
        nj2k_stream_set_user_data(j.stream, &j.sink, nullptr);
        nj2k_stream_set_write_function(j.stream, sink_write);
        const bool ok = j.run();
        if (!ok) {
            CHECK_MSG(j.status() == NJ2K_E_OUTPUT, "fail_after %zu: %s", fail_after, nj2k_status_string(j.status()));
        }
        CHECK_MSG(ok == (fail_after >= clean_size), "fail_after %zu: %s", fail_after, ok ? "encoded" : "failed");
    }
    // DP-17: a write function that returns 0 fails the stream, where OpenJPEG would call it for ever
    {
        Job j;
        nj2k_stream_destroy(j.stream);
        j.stream = nj2k_stream_create(16, NJ2K_FALSE);
        nj2k_stream_set_user_data(j.stream, &j.sink, nullptr);
        nj2k_stream_set_write_function(j.stream, [](void*, NJ2K_SIZE_T, void*) -> NJ2K_SIZE_T { return 0; });
        CHECK(!j.run());
        CHECK_MSG(j.status() == NJ2K_E_OUTPUT, "%s", nj2k_status_string(j.status()));
    }
    // DP-18: a comment longer than a marker segment holds is refused, where OpenJPEG writes a wrong length
    {
        Job j;
        std::string longer(65532, 'c');  // Lcom = 65536 no longer fits
        j.p.cp_comment = &longer[0];
        CHECK(!j.setup());
        CHECK(j.status() == NJ2K_E_ARGUMENT);
        std::string longest(65531, 'c');  // the most a COM marker holds (Lcom 65535)
        Job k;
        k.p.cp_comment = &longest[0];
        CHECK(k.run());
    }
    // DP-19: the caller keeps its planes, unchanged (OpenJPEG takes them, and shifts them in place)
    {
        Job j;
        std::vector<std::vector<NJ2K_INT32>> before;
        for (std::uint32_t c = 0; c < j.image->numcomps; ++c) {
            before.emplace_back(j.image->comps[c].data, j.image->comps[c].data + 40 * 40);
        }
        CHECK(j.run());
        for (std::uint32_t c = 0; c < j.image->numcomps; ++c) {
            CHECK(j.image->comps[c].data != nullptr);
            CHECK(std::memcmp(j.image->comps[c].data, before[c].data(), 40 * 40 * sizeof(NJ2K_INT32)) == 0);
        }
    }
    // handlers and the destroy of an encoder that did nothing, and of one cut off
    {
        nj2k_codec_t* c = nj2k_create_compress(NJ2K_CODEC_J2K);
        CHECK(c != nullptr);
        CHECK(nj2k_get_status(c) == NJ2K_OK);
        nj2k_destroy_codec(c);
        Job j;
        CHECK(j.setup());
        CHECK(nj2k_start_compress(j.codec, j.image, j.stream));
    }
    // OpenJPEG's own defaults are taken: no layers asked is one lossless layer, written back
    {
        Job j;
        j.p.tcp_numlayers = 0;
        j.p.cp_disto_alloc = 0;
        CHECK(j.run());
        CHECK(j.p.tcp_numlayers == 1 && j.p.cp_disto_alloc == 1 && j.p.tcp_rates[0] == 0.0f);
    }
}

void test_threads()
{
    // Below the crossover, lending a pool must be free: no dispatch and identical bytes.
    ThreadPool pool(7);
    {
        Job one(128, 128, 3);
        Job many(128, 128, 3);
        const nj2k_executor_t ex = pool.executor(8);
        CHECK(nj2k_encoder_set_executor(many.codec, &ex));
        const std::uint64_t before = pool.batches();
        CHECK(one.run() && many.run());
        CHECK(many.sink.bytes == one.sink.bytes);
        CHECK(pool.batches() == before);
    }

    // At and above 256 x 256, component wavelets and Tier-1 code-block batches use the executor;
    // every supported concurrency must still produce the serial codestream byte for byte.
    Job serial(256, 256, 3);
    CHECK(serial.run());
    for (const std::uint32_t concurrency : {2U, 4U, 8U, 16U, 64U}) {
        Job many(256, 256, 3);
        const nj2k_executor_t ex = pool.executor(concurrency);
        CHECK(nj2k_encoder_set_executor(many.codec, &ex));
        const std::uint64_t before = pool.batches();
        CHECK_MSG(many.run(), "encoder with %u threads: %s", concurrency, nj2k_status_string(many.status()));
        CHECK_MSG(pool.batches() > before, "encoder with %u threads did not use its executor", concurrency);
        CHECK_MSG(many.sink.bytes == serial.sink.bytes, "encoder with %u threads changed the codestream", concurrency);
    }

    // The executor is copied, can be taken back, and malformed descriptions are refused.
    {
        Job j;
        nj2k_executor_t ex = pool.executor(4);
        CHECK(nj2k_encoder_set_executor(j.codec, &ex) && j.status() == NJ2K_OK);
        CHECK(nj2k_encoder_set_executor(j.codec, nullptr));
        ex.struct_size = sizeof(nj2k_executor_t) - 1;
        CHECK(!nj2k_encoder_set_executor(j.codec, &ex) && j.status() == NJ2K_E_ARGUMENT);
        ex = pool.executor(4);
        ex.parallel_for = nullptr;
        CHECK(!nj2k_encoder_set_executor(j.codec, &ex) && j.status() == NJ2K_E_ARGUMENT);
        CHECK(!nj2k_encoder_set_executor(nullptr, nullptr));
    }
}

}  // namespace

int main(int argc, char** argv)
{
    console_reports();
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 2) {
        std::fprintf(stderr, "usage: nj2k_encode_unit <corpus folder>\n");
        return 2;
    }
    g_corpus = argv[1];
    test_unsupported();
    test_limits_and_states();
    test_viewer_matrix();
    test_corpus_images();
    test_settings();
    test_round_trip();
    test_threads();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

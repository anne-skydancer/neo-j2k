// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// API unit tests: neo-j2k's types match OpenJPEG's layout, its defaults and image functions
// behave as OpenJPEG's, the version, engine string and CPU check hold their contracts, and the
// test helpers read real codestreams correctly. OpenJPEG here is the oracle, linked beside neo-j2k
// (whose exports are nj2k_* only).
#include <openjpeg.h>

#include <neoj2k/neoj2k.h>
#include <neoj2k/neoj2k.hpp>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common.h"
#include "decoder.h"
#include "j2k_scan.h"

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

// ---- layout: every public structure has OpenJPEG's size, and the members code touches sit at
// OpenJPEG's offsets (the compatibility header relies on source compatibility; the layout match
// also lets a host share structures with code built against OpenJPEG) ------------------------------

#define SAME_SIZE(T) static_assert(sizeof(nj2k_##T) == sizeof(opj_##T), "size of " #T);
SAME_SIZE(poc_t)
SAME_SIZE(cparameters_t)
SAME_SIZE(dparameters_t)
SAME_SIZE(image_comp_t)
SAME_SIZE(image_t)
SAME_SIZE(image_cmptparm_t)
SAME_SIZE(packet_info_t)
SAME_SIZE(marker_info_t)
SAME_SIZE(tp_info_t)
SAME_SIZE(tile_info_t)
SAME_SIZE(codestream_info_t)
SAME_SIZE(tccp_info_t)
SAME_SIZE(tile_info_v2_t)
SAME_SIZE(codestream_info_v2_t)
SAME_SIZE(tp_index_t)
SAME_SIZE(tile_index_t)
SAME_SIZE(codestream_index_t)
SAME_SIZE(jp2_metadata_t)
SAME_SIZE(jp2_index_t)
#undef SAME_SIZE

#define SAME_AT(T, m) static_assert(offsetof(nj2k_##T, m) == offsetof(opj_##T, m), "offset of " #T "." #m);
SAME_AT(image_comp_t, dx) SAME_AT(image_comp_t, w) SAME_AT(image_comp_t, prec) SAME_AT(image_comp_t, sgnd)
SAME_AT(image_comp_t, resno_decoded) SAME_AT(image_comp_t, factor) SAME_AT(image_comp_t, data)
SAME_AT(image_comp_t, alpha)
SAME_AT(image_t, x0) SAME_AT(image_t, y1) SAME_AT(image_t, numcomps) SAME_AT(image_t, color_space)
SAME_AT(image_t, comps) SAME_AT(image_t, icc_profile_buf) SAME_AT(image_t, icc_profile_len)
SAME_AT(image_cmptparm_t, dx) SAME_AT(image_cmptparm_t, w) SAME_AT(image_cmptparm_t, prec) SAME_AT(image_cmptparm_t, sgnd)
SAME_AT(dparameters_t, cp_reduce) SAME_AT(dparameters_t, cp_layer) SAME_AT(dparameters_t, infile)
SAME_AT(dparameters_t, decod_format) SAME_AT(dparameters_t, DA_x0) SAME_AT(dparameters_t, tile_index)
SAME_AT(dparameters_t, flags)
SAME_AT(cparameters_t, tile_size_on) SAME_AT(cparameters_t, prog_order) SAME_AT(cparameters_t, POC)
SAME_AT(cparameters_t, numpocs) SAME_AT(cparameters_t, tcp_numlayers) SAME_AT(cparameters_t, tcp_rates)
SAME_AT(cparameters_t, numresolution) SAME_AT(cparameters_t, mode) SAME_AT(cparameters_t, irreversible)
SAME_AT(cparameters_t, prcw_init) SAME_AT(cparameters_t, image_offset_x0) SAME_AT(cparameters_t, cp_cinema)
SAME_AT(cparameters_t, tcp_mct) SAME_AT(cparameters_t, mct_data) SAME_AT(cparameters_t, rsiz)
SAME_AT(codestream_info_v2_t, tx0) SAME_AT(codestream_info_v2_t, tw) SAME_AT(codestream_info_v2_t, nbcomps)
SAME_AT(codestream_info_v2_t, m_default_tile_info) SAME_AT(codestream_info_v2_t, tile_info)
SAME_AT(tccp_info_t, numresolutions) SAME_AT(tccp_info_t, qmfbid) SAME_AT(tccp_info_t, stepsizes_mant)
SAME_AT(tccp_info_t, roishift) SAME_AT(tccp_info_t, prch)
#undef SAME_AT

static_assert(int(NJ2K_CODEC_J2K) == int(OPJ_CODEC_J2K) && int(NJ2K_CODEC_JP2) == int(OPJ_CODEC_JP2), "codec enum");
static_assert(int(NJ2K_CPRL) == int(OPJ_CPRL) && int(NJ2K_CLRSPC_CMYK) == int(OPJ_CLRSPC_CMYK), "enums");
static_assert(NJ2K_DPARAMETERS_DUMP_FLAG == OPJ_DPARAMETERS_DUMP_FLAG && NJ2K_J2K_MAXRLVLS == OPJ_J2K_MAXRLVLS, "macros");

void test_version()
{
    const std::string v = nj2k_version();
    char want[32];
    std::snprintf(want, sizeof(want), "%d.%d.%d", NJ2K_VERSION_MAJOR, NJ2K_VERSION_MINOR, NJ2K_VERSION_PATCH);
    CHECK(v == want);
    const std::string e = nj2k_engine_string();
    CHECK(!e.empty() && e.size() <= NJ2K_ENGINE_STRING_MAX);
    CHECK(e.rfind("neo-j2k ", 0) == 0);
    CHECK(nj2k::engine_string() == e && nj2k::version() == v);
}

void test_cpu()
{
    // The test program runs, so the CPU runs this build: the check must say so.
    CHECK(nj2k_cpu_supported() == 1);
    CHECK(nj2k::cpu_supported());
}

void test_defaults()
{
    nj2k_dparameters_t nd;
    opj_dparameters_t od;
    std::memset(&nd, 0xA5, sizeof(nd));
    std::memset(&od, 0x5A, sizeof(od));
    nj2k_set_default_decoder_parameters(&nd);
    opj_set_default_decoder_parameters(&od);
    CHECK(std::memcmp(&nd, &od, sizeof(nd)) == 0);

    // the encoder parameters hold pointers and padding; memset first on both so padding agrees
    nj2k_cparameters_t nc;
    opj_cparameters_t oc;
    std::memset(&nc, 0, sizeof(nc));
    std::memset(&oc, 0, sizeof(oc));
    nj2k_set_default_encoder_parameters(&nc);
    opj_set_default_encoder_parameters(&oc);
    CHECK(std::memcmp(&nc, &oc, sizeof(nc)) == 0);
    nj2k_set_default_decoder_parameters(nullptr);
    nj2k_set_default_encoder_parameters(nullptr);
}

void test_image(std::uint32_t numcomps, std::uint32_t w, std::uint32_t h)
{
    std::vector<nj2k_image_cmptparm_t> np(numcomps ? numcomps : 1);
    std::vector<opj_image_cmptparm_t> op(numcomps ? numcomps : 1);
    for (std::uint32_t c = 0; c < np.size(); ++c) {
        std::memset(&np[c], 0, sizeof(np[c]));
        std::memset(&op[c], 0, sizeof(op[c]));
        np[c].dx = op[c].dx = 1 + c % 2;
        np[c].dy = op[c].dy = 1;
        np[c].w = op[c].w = w;
        np[c].h = op[c].h = h;
        np[c].x0 = op[c].x0 = c;
        np[c].prec = op[c].prec = 8 + c;
        np[c].sgnd = op[c].sgnd = c & 1;
    }
    for (int tile = 0; tile < 2; ++tile) {
        nj2k_image_t* ni = tile ? nj2k_image_tile_create(numcomps, np.data(), NJ2K_CLRSPC_SRGB)
                                : nj2k_image_create(numcomps, np.data(), NJ2K_CLRSPC_SRGB);
        opj_image_t* oi = tile ? opj_image_tile_create(numcomps, op.data(), OPJ_CLRSPC_SRGB)
                               : opj_image_create(numcomps, op.data(), OPJ_CLRSPC_SRGB);
        CHECK((ni == nullptr) == (oi == nullptr));
        if (ni && oi) {
            CHECK(ni->numcomps == oi->numcomps && int(ni->color_space) == int(oi->color_space));
            for (std::uint32_t c = 0; c < ni->numcomps; ++c) {
                const auto& a = ni->comps[c];
                const auto& b = oi->comps[c];
                CHECK(a.dx == b.dx && a.dy == b.dy && a.w == b.w && a.h == b.h && a.x0 == b.x0 && a.prec == b.prec &&
                      a.sgnd == b.sgnd && a.factor == b.factor && a.resno_decoded == b.resno_decoded);
                CHECK((a.data == nullptr) == (b.data == nullptr));
                if (a.data) {
                    bool zero = true;
                    for (std::size_t i = 0; i < std::size_t(a.w) * a.h; ++i) {
                        zero = zero && a.data[i] == 0;
                    }
                    CHECK(zero);
                    CHECK(reinterpret_cast<std::uintptr_t>(a.data) % 16 == 0);
                }
            }
        }
        nj2k_image_destroy(ni);
        opj_image_destroy(oi);
    }
}

void test_images()
{
    test_image(3, 17, 5);
    test_image(1, 1, 1);
    test_image(5, 64, 64);
    test_image(0, 4, 4);  // no components: OpenJPEG gives no image
    test_image(2, 0, 9);  // a component without samples: no image either
    nj2k_image_destroy(nullptr);
    void* p = nj2k_image_data_alloc(100);
    CHECK(p != nullptr && reinterpret_cast<std::uintptr_t>(p) % 64 == 0);
    nj2k_image_data_free(p);
    CHECK(nj2k_image_data_alloc(0) == nullptr);
    nj2k_image_data_free(nullptr);
}

// Encodes a small LRCP image with the oracle, with SOP markers and several layers.
Bytes encode_with_oracle(int w, int h, int comps, int layers, bool sop)
{
    std::vector<opj_image_cmptparm_t> cp(comps);
    for (auto& c : cp) {
        std::memset(&c, 0, sizeof(c));
        c.dx = c.dy = 1;
        c.w = std::uint32_t(w);
        c.h = std::uint32_t(h);
        c.prec = 8;
    }
    opj_image_t* image = opj_image_create(std::uint32_t(comps), cp.data(), comps >= 3 ? OPJ_CLRSPC_SRGB : OPJ_CLRSPC_GRAY);
    image->x1 = std::uint32_t(w);
    image->y1 = std::uint32_t(h);
    for (int c = 0; c < comps; ++c) {
        for (int i = 0; i < w * h; ++i) {
            image->comps[c].data[i] = (i * 7 + c * 31 + (i / w) * 3) & 255;
        }
    }
    opj_cparameters_t p;
    opj_set_default_encoder_parameters(&p);
    p.tcp_numlayers = layers;
    for (int l = 0; l < layers; ++l) {
        p.tcp_rates[l] = float(1 << (2 * (layers - 1 - l))) * 4.0f;
    }
    p.tcp_rates[layers - 1] = 0;  // last layer lossless
    p.cp_disto_alloc = 1;
    p.numresolution = 3;
    p.irreversible = 0;
    p.tcp_mct = comps >= 3 ? 1 : 0;
    if (sop) {
        p.csty |= 0x02;
    }
    opj_codec_t* codec = opj_create_compress(OPJ_CODEC_J2K);
    Bytes out;
    struct Sink { Bytes* b; std::size_t pos = 0; } sink{&out};
    opj_stream_t* st = opj_stream_create(1 << 16, OPJ_FALSE);
    opj_stream_set_user_data(st, &sink, nullptr);
    opj_stream_set_write_function(st, [](void* buf, OPJ_SIZE_T n, void* u) -> OPJ_SIZE_T {
        auto* s = static_cast<Sink*>(u);
        if (s->pos + n > s->b->size()) {
            s->b->resize(s->pos + n);
        }
        std::memcpy(s->b->data() + s->pos, buf, n);
        s->pos += n;
        return n;
    });
    opj_stream_set_skip_function(st, [](OPJ_OFF_T n, void* u) -> OPJ_OFF_T {
        auto* s = static_cast<Sink*>(u);
        s->pos += std::size_t(n);
        if (s->pos > s->b->size()) {
            s->b->resize(s->pos);
        }
        return n;
    });
    opj_stream_set_seek_function(st, [](OPJ_OFF_T n, void* u) -> OPJ_BOOL {
        static_cast<Sink*>(u)->pos = std::size_t(n);
        return OPJ_TRUE;
    });
    const bool ok = opj_setup_encoder(codec, &p, image) && opj_start_compress(codec, image, st) &&
                    opj_encode(codec, st) && opj_end_compress(codec, st);
    opj_stream_destroy(st);
    opj_destroy_codec(codec);
    opj_image_destroy(image);
    if (!ok) {
        out.clear();
    }
    return out;
}

void test_scan_and_backends()
{
    const Bytes j2k = encode_with_oracle(48, 40, 3, 4, true);
    CHECK(!j2k.empty());
    if (j2k.empty()) {
        return;
    }
    std::string why;
    const auto mh = parse_main_header(j2k, &why);
    CHECK(mh.has_value());
    if (!mh) {
        std::printf("  parse_main_header: %s\n", why.c_str());
        return;
    }
    CHECK(mh->width() == 48 && mh->height() == 40 && mh->csiz == 3 && mh->layers == 4 && mh->min_resolutions == 3);
    CHECK(mh->tiles_x() == 1 && mh->tiles_y() == 1 && (mh->scod & 2));
    const auto tps = scan_tile_parts(j2k, *mh);
    CHECK(tps.size() == 1);
    const auto bounds = layer_boundaries(j2k);
    CHECK(bounds && bounds->size() == 3);

    auto oracle = make_oracle_backend();
    Request rq;
    rq.data = j2k.data();
    rq.size = j2k.size();
    rq.capture = true;
    const Outcome whole = oracle->decode(rq);
    CHECK(whole.ok && whole.numcomps == 3 && whole.comps[0].w == 48 && whole.comps[0].h == 40);
    // the last layer is lossless: the whole decode gives the encoded samples back
    if (whole.ok) {
        bool exact = true;
        for (int i = 0; i < 48 * 40; ++i) {
            exact = exact && whole.planes[1][std::size_t(i)] == ((i * 7 + 31 + (i / 48) * 3) & 255);
        }
        CHECK(exact);
    }
    // viewer mode decodes the same samples
    rq.mode = Mode::Viewer;
    const Outcome view = oracle->decode(rq);
    CHECK(view.ok && view.hash == whole.hash && view.u8_hash != 0);
    // a cut at the start of layer 2 equals the whole file limited to one layer
    if (bounds) {
        Request cut;
        cut.data = j2k.data();
        cut.size = (*bounds)[0];
        Request one = cut;
        one.size = j2k.size();
        one.layers = 1;
        const Outcome a = oracle->decode(cut), b = oracle->decode(one);
        CHECK(a.ok && b.ok && a.hash == b.hash);
    }
    // strict mode refuses a cut stream that non-strict mode decodes
    Request cut;
    cut.data = j2k.data();
    cut.size = j2k.size() - 7;
    cut.mode = Mode::Strict;
    CHECK(!oracle->decode(cut).ok);
    cut.mode = Mode::Plain;
    CHECK(oracle->decode(cut).ok);

    // neo-j2k through the compatibility header: until stage 1c it refuses at codec creation
    auto neo = make_neoj2k_backend();
    rq.mode = Mode::Plain;
    const Outcome n = neo->decode(rq);
    CHECK(!n.ok && n.stage == "codec");
    CHECK(!nj2k::make_decoder());
}

void test_viewer_rules()
{
    // calcDataSizeJ2C: 600 header bytes plus the 64x64 base layer for a small texture at discard 5
    CHECK(viewer::data_size_j2c(32, 32, 5) == 600 + 2048);
    CHECK(viewer::data_size_j2c(1024, 1024, 0) > viewer::data_size_j2c(1024, 1024, 3));
    // the reduce clamp: a 16x16 texture with 5 resolutions asked for discard 5 decodes at 4
    CHECK(viewer::reduce_factor(5, 5, 16, 16) == 4);
    CHECK(viewer::reduce_factor(3, 5, 16, 16) == 3);
    CHECK(viewer::reduce_factor(5, 0, 16, 16) == 5);
    // the decode guard
    CHECK(viewer::decode_allowed(1024, 1024, 4, 0));
    CHECK(!viewer::decode_allowed(16384, 16384, 4, 0));
    CHECK(viewer::decode_allowed(16384, 16384, 4, 1));
    CHECK(!viewer::decode_allowed(64, 64, 9, 0));
}

}  // namespace

int main()
{
    console_reports();
    test_version();
    test_cpu();
    test_defaults();
    test_images();
    test_scan_and_backends();
    test_viewer_rules();
    std::printf("api_unit: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

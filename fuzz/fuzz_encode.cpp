// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// Fuzz target: the encoder's inputs (stage 6a). Sixteen bytes choose an image (sides from 1 to past
// the encoder's limits, 1 to 10 components, mostly 8 bits unsigned, sometimes another precision,
// signedness or subsampling) and the settings of OpenJPEG's encoder (resolutions, layers and rates,
// reversible or not, the colour transform, a comment, and now and then a setting the core encoder
// does not take: tiles, precincts, another progression, code-block size, mode switches, tile-parts,
// a region of interest; a size cap it takes); the rest are the samples. neo-j2k's encoder takes it through its
// own API: it must never crash or hang, a refusal must say why (NJ2K_E_UNSUPPORTED, NJ2K_E_LIMIT,
// NJ2K_E_ARGUMENT or, for the harness's own allocations, NJ2K_E_NO_MEMORY) and a refusal of the settings or
// the image write no byte (the stream has a one-byte buffer, so the sink sees every byte), and a
// codestream it writes must decode in the oracle to the image (exactly, when lossless). Where the settings are the viewer's (the supported set), the oracle's
// encoder takes the same image and the two must agree: the same bytes, or both refuse. The documented
// departures are kept out: the oracle reads components that are not there for a colour transform of
// fewer than three, and wraps samples outside the 8-bit range (DP-16). A failed check says why on
// stderr, then aborts.
#include <neoj2k/neoj2k.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common.h"
#include "decoder.h"
#include "fuzz_common.h"

namespace {

[[noreturn]] void fail(const char* format, ...)
{
    std::va_list args;
    va_start(args, format);
    std::fputs("encode: ", stderr);
    std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
    va_end(args);
    std::fflush(stderr);
    std::abort();
}

struct Sink {
    std::vector<std::uint8_t> bytes;
};

NJ2K_SIZE_T sink_write(void* buf, NJ2K_SIZE_T n, void* user)
{
    auto* s = static_cast<Sink*>(user);
    s->bytes.insert(s->bytes.end(), static_cast<std::uint8_t*>(buf), static_cast<std::uint8_t*>(buf) + n);
    return n;
}

void quiet(const char*, void*) {}

constexpr std::uint32_t kMaxSide = 4096;      // the encoder's limits (doc/api.md)
constexpr std::uint32_t kMaxComponents = 8;
constexpr std::uint32_t kMaxLayers = 100;
constexpr std::uint64_t kHarnessSamples = 1u << 20;  // what the harness allocates

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    nj2k::fuzz::fuzz_setup();
    if (size < 18) {
        return 0;
    }
    const std::uint8_t* b = data;
    std::uint32_t w = (b[1] & 0x80) ? 1 + ((b[0] | (b[1] & 0x7f) << 8) % 5000u) : 1 + b[0] % 80u;
    std::uint32_t h = (b[3] & 0x80) ? 1 + ((b[2] | (b[3] & 0x7f) << 8) % 5000u) : 1 + b[2] % 80u;
    const std::uint32_t comps = 1 + b[4] % 10u;
    const std::uint32_t prec = (b[5] & 0xF0) ? 8 : 1 + (b[5] & 0x0F);
    const bool reversible = b[6] & 1;
    const bool want_mct = b[6] & 2;
    const bool sgnd = b[6] & 0x80;
    const std::uint32_t dx = (b[6] & 0x40) ? 2 : 1;
    const bool offset = b[6] & 0x20;
    const int numres = (b[7] & 0x80) ? 1 + b[7] % 40 : 1 + b[7] % 9;
    const int layers = b[8] >= 200 ? 101 : b[8] % 9;
    static const float bases[] = {0.0f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 100.0f, 1e5f};
    const float base = bases[b[9] % 8];
    const bool weird_rates = b[10] & 1;
    const std::uint8_t toggles = b[11];
    const int comment_kind = b[12] % 5;
    const std::size_t comment_len = b[13] % 200;
    const bool out_of_range = b[14] & 1;
    const std::uint8_t* samples = data + 18;
    const std::size_t nsamples = size - 18;

    // the harness allocates at most kHarnessSamples a codec's image; sides over the encoder's limits
    // are handed over with a one-sample image, which the encoder must refuse before it reads anything
    const bool over_limits = w > kMaxSide || h > kMaxSide || comps > kMaxComponents ||
                             std::uint64_t(w) * h * comps > (std::uint64_t(1) << 26);
    std::uint32_t iw = w, ih = h;
    if (!over_limits) {
        while (std::uint64_t(iw) * ih * comps > kHarnessSamples && ih > 1) {
            ih = (ih + 1) / 2;
        }
        while (std::uint64_t(iw) * ih * comps > kHarnessSamples && iw > 1) {
            iw = (iw + 1) / 2;
        }
        w = iw;
        h = ih;
    }
    const std::uint32_t pw = over_limits ? 1 : w, ph = over_limits ? 1 : h;

    std::vector<nj2k_image_cmptparm_t> parms(comps);
    for (auto& c : parms) {
        std::memset(&c, 0, sizeof(c));
        c.dx = dx;
        c.dy = 1;
        c.w = pw;
        c.h = ph;
        c.prec = prec;
        c.sgnd = sgnd ? 1 : 0;
    }
    nj2k_image_t* image = nj2k_image_create(comps, parms.data(), NJ2K_CLRSPC_SRGB);
    if (!image) {
        return 0;
    }
    image->x1 = over_limits ? w : pw;
    image->y1 = over_limits ? h : ph;
    if (offset && !over_limits) {
        image->x0 = 1;
    }
    std::vector<std::vector<std::int32_t>> input(comps);
    bool in_range = true;
    for (std::uint32_t c = 0; c < comps; ++c) {
        const std::size_t n = std::size_t(pw) * ph;
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t k = std::size_t(c) * n + i;
            std::int32_t v = nsamples ? samples[k % nsamples] : 0;
            if (out_of_range && (k % 97) == 0) {
                v = (v & 1) ? -static_cast<std::int32_t>(1 + (k % 5)) : 256 + static_cast<std::int32_t>(k % 50);
                in_range = false;
            }
            image->comps[c].data[i] = v;
            input[c].push_back(v);
        }
    }

    nj2k_cparameters_t p;
    nj2k_set_default_encoder_parameters(&p);
    p.cod_format = NJ2K_CODEC_J2K;
    p.prog_order = (toggles & 4) ? NJ2K_RPCL : NJ2K_RLCP;
    p.cp_disto_alloc = 1;
    p.max_cs_size = (toggles & 0x80) ? 5000 : 0;
    p.irreversible = reversible ? 0 : 1;
    p.numresolution = numres;
    p.tcp_mct = static_cast<char>(want_mct ? 1 : 0);
    p.tcp_numlayers = layers;
    std::vector<float> rates;
    for (int l = 0; l < layers && l < 100; ++l) {
        float r = base * static_cast<float>(1u << (2 * ((layers - 1 - l) % 12)));
        if (weird_rates && (l & 1)) {
            r = base * static_cast<float>(l + 1);
        }
        p.tcp_rates[l] = r;
        rates.push_back(r);
    }
    if (toggles & 1) {
        p.tile_size_on = NJ2K_TRUE;
        p.cp_tdx = p.cp_tdy = 64;
    }
    if (toggles & 2) {
        p.csty = 1 + (b[15] % 7);
    }
    if (toggles & 8) {
        p.cblockw_init = 32;
        p.cblockh_init = 64;
    }
    if (toggles & 0x10) {
        p.mode = 1 + (b[15] % 63);
    }
    if (toggles & 0x20) {
        p.tp_on = 1;
        p.tp_flag = 'R';
    }
    if (toggles & 0x40) {
        p.roi_compno = 0;
        p.roi_shift = 3;
    }
    std::string comment;
    switch (comment_kind) {
    case 0:
        p.cp_comment = nullptr;
        break;
    case 1:
        comment = "";
        p.cp_comment = &comment[0];
        break;
    case 2:
        comment = "no comment";
        p.cp_comment = &comment[0];
        break;
    case 3:
        comment.assign(comment_len, 'x');
        p.cp_comment = &comment[0];
        break;
    default:
        comment.assign(comment_len + 65532, 'y');  // longer than a marker segment holds
        p.cp_comment = &comment[0];
        break;
    }

    // the supported set the oracle can be asked the same: the viewer's settings with 8-bit unsigned
    // components of the sizes the viewer sends, in range; the oracle is kept from what it cannot take
    const bool viewer_set = !over_limits && prec == 8 && !sgnd && dx == 1 && !offset && (toggles & 0x7F) == 0 &&
                            layers >= 1 && layers <= int(kMaxLayers) && w >= 4 && h >= 4 && in_range &&
                            !(want_mct && comps < 3) && comment_kind < 4 && numres <= 32;

    // what the oracle's encoder is asked, for the viewer's settings (the image's rows are the raw's bottom row first)
    std::vector<std::uint8_t> raw;
    nj2k::test::UploadSpec us;
    if (viewer_set) {
        raw.resize(std::size_t(w) * h * comps);
        for (std::uint32_t y = 0; y < h; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
                for (std::uint32_t c = 0; c < comps; ++c) {
                    raw[(std::size_t(h - 1 - y) * w + x) * comps + c] =
                        static_cast<std::uint8_t>(input[c][std::size_t(y) * w + x]);
                }
            }
        }
        us.raw = raw.data();
        us.width = static_cast<int>(w);
        us.height = static_cast<int>(h);
        us.comps = static_cast<int>(comps);
        us.reversible = reversible;
        us.layers = layers;
        us.rates = rates;
        us.numres = numres;
        us.mct = want_mct ? 1 : 0;
        us.comment = comment_kind == 0 ? nullptr : p.cp_comment;
        us.max_cs_size = (toggles & 0x80) ? 5000 : 0;
    }

    Sink sink;
    nj2k_codec_t* enc = nj2k_create_compress(NJ2K_CODEC_J2K);
    nj2k_stream_t* st = nj2k_stream_create(1, NJ2K_FALSE);  // every byte reaches the sink as it is written
    if (!enc || !st) {
        fail("no memory for the harness");
    }
    nj2k_set_error_handler(enc, quiet, nullptr);
    nj2k_set_warning_handler(enc, quiet, nullptr);
    nj2k_stream_set_user_data(st, &sink, nullptr);
    nj2k_stream_set_write_function(st, sink_write);
    const bool ok = nj2k_setup_encoder(enc, &p, image) && nj2k_start_compress(enc, image, st) &&
                    nj2k_encode(enc, st) && nj2k_end_compress(enc, st);
    const nj2k_status_t status = nj2k_get_status(enc);
    nj2k_stream_destroy(st);
    nj2k_destroy_codec(enc);

    if (!ok) {
        if (status != NJ2K_E_UNSUPPORTED && status != NJ2K_E_LIMIT && status != NJ2K_E_ARGUMENT &&
            status != NJ2K_E_NO_MEMORY) {  // memory: the harness's own allocations are small
            fail("a refusal with status %d (%s)", status, nj2k_status_string(status));
        }
        // a refusal of the settings or the image writes nothing (a tile that does not fit, NJ2K_E_LIMIT after
        // the header, is the one refusal that follows the main header)
        if (!sink.bytes.empty() && (status == NJ2K_E_UNSUPPORTED || status == NJ2K_E_ARGUMENT || over_limits)) {
            fail("%zu bytes written before a refusal (%s)", sink.bytes.size(), nj2k_status_string(status));
        }
        if (over_limits && status != NJ2K_E_LIMIT && status != NJ2K_E_UNSUPPORTED) {
            fail("an image over the limits (%ux%u, %u components) is refused with %s", w, h, comps,
                 nj2k_status_string(status));
        }
        if (viewer_set) {
            // the oracle must refuse what the core encoder refuses of the supported set
            std::vector<std::uint8_t> theirs;
            if (nj2k::test::oracle_upload(us, theirs)) {
                fail("neo-j2k refuses (%s) what the oracle encodes: %ux%u, %u components, %s, %d layers, %d resolutions",
                     nj2k_status_string(status), w, h, comps, reversible ? "5/3" : "9/7", layers, numres);
            }
        }
        nj2k_image_destroy(image);
        return 0;
    }

    ++nj2k::fuzz::exercised();
    if (sink.bytes.size() < 20 || sink.bytes[0] != 0xFF || sink.bytes[1] != 0x4F) {
        fail("a codestream without SOC");
    }
    static const auto oracle = nj2k::test::make_oracle_backend();
    nj2k::test::Request rq;
    rq.data = sink.bytes.data();
    rq.size = sink.bytes.size();
    rq.capture = true;
    const nj2k::test::Outcome o = oracle->decode(rq);
    if (!o.ok) {
        fail("the oracle cannot decode what neo-j2k's encoder wrote (%s: %s)", o.stage.c_str(), o.last_error.c_str());
    }
    if (o.numcomps != comps || o.comps.empty() || o.comps[0].w != w || o.comps[0].h != h) {
        fail("the oracle decodes %u components of %ux%u, not %u of %ux%u", o.numcomps,
             o.comps.empty() ? 0 : o.comps[0].w, o.comps.empty() ? 0 : o.comps[0].h, comps, w, h);
    }
    // lossless: the 5/3 wavelet with a last layer that is not cut by a rate (0 or at most 1, which
    // OpenJPEG takes as lossless). The setup wrote its layers and rates back into the parameters: no
    // layers asked is one layer at rate 0, and a size cap lifts the rates to the ratio it implies,
    // which cuts that layer too
    const bool exact = reversible && p.tcp_rates[p.tcp_numlayers - 1] <= 1.0f;
    if (exact) {
        for (std::uint32_t c = 0; c < comps; ++c) {
            // the planes are the image's own
            for (std::size_t i = 0; i < input[c].size(); ++i) {
                if (o.planes[c][i] != input[c][i]) {
                    fail("lossless: component %u sample %zu is %d, was %d (%ux%u)", c, i, o.planes[c][i], input[c][i], w,
                         h);
                }
            }
        }
    }
    if (viewer_set) {
        std::vector<std::uint8_t> theirs;
        if (!nj2k::test::oracle_upload(us, theirs)) {
            fail("the oracle refuses what neo-j2k encodes: %ux%u, %u components, %s, %d layers, %d resolutions", w, h,
                 comps, reversible ? "5/3" : "9/7", layers, numres);
        }
        if (theirs != sink.bytes) {
            std::size_t first = 0;
            while (first < theirs.size() && first < sink.bytes.size() && theirs[first] == sink.bytes[first]) {
                ++first;
            }
            fail("not the oracle's bytes (%zu against %zu, the first difference at %zu): %ux%u, %u components, %s, %d layers, "
                 "%d resolutions",
                 sink.bytes.size(), theirs.size(), first, w, h, comps, reversible ? "5/3" : "9/7", layers, numres);
        }
    }
    nj2k_image_destroy(image);
    return 0;
}

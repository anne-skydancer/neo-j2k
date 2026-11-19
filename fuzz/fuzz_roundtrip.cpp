// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// Fuzz target: lossless round trip. The input chooses a small image (1-64 px a side, 1-5
// components, 1-16 bits, signed or not) and its coding (resolutions, code-block size, progression,
// mode switches, layers, SOP, EPH, the colour transform); the remaining bytes are the samples. The
// image is encoded reversibly (5/3) and decoded: every sample must come back exactly. Today both
// ends are the oracle; stage 6a encodes with neo-j2k and decodes with both.
#include <openjpeg.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "decoder.h"

namespace {

struct Sink {
    std::vector<std::uint8_t> bytes;
    std::size_t pos = 0;
};

OPJ_SIZE_T sink_write(void* buf, OPJ_SIZE_T n, void* user)
{
    auto* s = static_cast<Sink*>(user);
    if (s->pos + n > s->bytes.size()) {
        s->bytes.resize(s->pos + n);
    }
    std::memcpy(s->bytes.data() + s->pos, buf, n);
    s->pos += n;
    return n;
}

OPJ_OFF_T sink_skip(OPJ_OFF_T n, void* user)
{
    auto* s = static_cast<Sink*>(user);
    s->pos = std::size_t(OPJ_OFF_T(s->pos) + n);
    if (s->pos > s->bytes.size()) {
        s->bytes.resize(s->pos);
    }
    return n;
}

OPJ_BOOL sink_seek(OPJ_OFF_T to, void* user)
{
    auto* s = static_cast<Sink*>(user);
    s->pos = std::size_t(to);
    if (s->pos > s->bytes.size()) {
        s->bytes.resize(s->pos);
    }
    return OPJ_TRUE;
}

void quiet(const char*, void*) {}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < 12) {
        return 0;
    }
    const int w = 1 + data[0] % 64, h = 1 + data[1] % 64, comps = 1 + data[2] % 5, prec = 1 + data[3] % 16;
    const bool sgnd = data[4] & 1;
    int numres = 1 + data[5] % 6;
    const int cbw = 2 + data[6] % 5, cbh = 2 + (data[6] / 5) % 5;  // exponents 2-6: 4..64 samples
    const OPJ_PROG_ORDER prog = OPJ_PROG_ORDER(data[7] % 5);
    const int mode = data[8] & 63;
    const int layers = 1 + data[9] % 4;
    const bool sop = data[10] & 1, eph = data[10] & 2, mct = (data[10] & 4) && comps >= 3;
    const std::uint8_t* samples = data + 12;
    const std::size_t nsamples = size - 12;
    while (numres > 1 && ((w >> (numres - 1)) == 0 || (h >> (numres - 1)) == 0)) {
        --numres;
    }

    std::vector<opj_image_cmptparm_t> cp(static_cast<std::size_t>(comps));
    for (auto& c : cp) {
        std::memset(&c, 0, sizeof(c));
        c.dx = c.dy = 1;
        c.w = OPJ_UINT32(w);
        c.h = OPJ_UINT32(h);
        c.prec = OPJ_UINT32(prec);
        c.sgnd = sgnd ? 1 : 0;
    }
    opj_image_t* image = opj_image_create(OPJ_UINT32(comps), cp.data(), comps >= 3 ? OPJ_CLRSPC_SRGB : OPJ_CLRSPC_GRAY);
    if (!image) {
        return 0;
    }
    image->x1 = OPJ_UINT32(w);
    image->y1 = OPJ_UINT32(h);
    // OpenJPEG's encoder transforms a single tile in the image's own buffers, so the samples to
    // compare with are kept aside before it runs
    std::vector<std::vector<std::int32_t>> expected(static_cast<std::size_t>(comps));
    std::size_t k = 0;
    for (int c = 0; c < comps; ++c) {
        for (int i = 0; i < w * h; ++i, ++k) {
            const std::uint32_t raw = nsamples ? std::uint32_t(samples[k % nsamples]) << 8 | samples[(k * 7 + 1) % nsamples] : 0;
            std::int32_t v = std::int32_t(raw & ((1u << prec) - 1));
            if (sgnd) {
                v -= std::int32_t(1u << (prec - 1));
            }
            image->comps[c].data[i] = v;
            expected[static_cast<std::size_t>(c)].push_back(v);
        }
    }

    opj_cparameters_t p;
    opj_set_default_encoder_parameters(&p);
    p.numresolution = numres;
    p.cblockw_init = 1 << cbw;
    p.cblockh_init = 1 << cbh;
    p.prog_order = prog;
    p.mode = mode;
    p.irreversible = 0;
    p.tcp_mct = mct ? 1 : 0;
    p.tcp_numlayers = layers;
    for (int l = 0; l < layers; ++l) {
        p.tcp_rates[l] = float((layers - 1 - l) * 8);  // the last layer is lossless (0)
    }
    p.cp_disto_alloc = 1;
    if (sop) {
        p.csty |= 0x02;
    }
    if (eph) {
        p.csty |= 0x04;
    }
    opj_codec_t* enc = opj_create_compress(OPJ_CODEC_J2K);
    opj_set_info_handler(enc, quiet, nullptr);
    opj_set_warning_handler(enc, quiet, nullptr);
    opj_set_error_handler(enc, quiet, nullptr);
    Sink sink;
    opj_stream_t* st = opj_stream_create(1 << 16, OPJ_FALSE);
    opj_stream_set_user_data(st, &sink, nullptr);
    opj_stream_set_write_function(st, sink_write);
    opj_stream_set_skip_function(st, sink_skip);
    opj_stream_set_seek_function(st, sink_seek);
    const bool encoded = opj_setup_encoder(enc, &p, image) && opj_start_compress(enc, image, st) &&
                         opj_encode(enc, st) && opj_end_compress(enc, st);
    opj_stream_destroy(st);
    opj_destroy_codec(enc);
    if (encoded) {
        static const auto oracle = nj2k::test::make_oracle_backend();
        nj2k::test::Request rq;
        rq.data = sink.bytes.data();
        rq.size = sink.pos;
        rq.capture = true;
        const nj2k::test::Outcome o = oracle->decode(rq);
        // a stream the encoder wrote must decode, and losslessly
        if (!o.ok || o.numcomps != OPJ_UINT32(comps)) {
            std::abort();
        }
        for (int c = 0; c < comps; ++c) {
            if (o.planes[std::size_t(c)] != expected[std::size_t(c)]) {
                std::abort();
            }
        }
    }
    opj_image_destroy(image);
    return 0;
}

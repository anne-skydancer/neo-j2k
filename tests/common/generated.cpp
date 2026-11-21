// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)

#include "generated.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <memory>

namespace nj2k::test {
namespace {

// ---- synthetic images -------------------------------------------------------------------------------

struct Rng {
    std::uint64_t s;
    std::uint32_t next()
    {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return std::uint32_t(s >> 11);
    }
};

int ceil_div(int a, int b)
{
    return (a + b - 1) / b;
}

opj_image_t* make_image(const GeneratedSpec& s)
{
    std::vector<opj_image_cmptparm_t> cp(std::size_t(s.comps));
    const int x1 = s.x0 + s.w, y1 = s.y0 + s.h;
    for (int c = 0; c < s.comps; ++c) {
        auto& p = cp[std::size_t(c)];
        std::memset(&p, 0, sizeof(p));
        const int d = (c == 1 || c == 2) ? s.sub : 1;
        p.dx = p.dy = OPJ_UINT32(d);
        p.x0 = OPJ_UINT32(ceil_div(s.x0, d));
        p.y0 = OPJ_UINT32(ceil_div(s.y0, d));
        p.w = OPJ_UINT32(ceil_div(x1, d) - ceil_div(s.x0, d));
        p.h = OPJ_UINT32(ceil_div(y1, d) - ceil_div(s.y0, d));
        p.prec = OPJ_UINT32(s.prec);
        p.sgnd = s.sgnd ? 1 : 0;
    }
    const OPJ_COLOR_SPACE cs = s.comps >= 3 ? OPJ_CLRSPC_SRGB : OPJ_CLRSPC_GRAY;
    opj_image_t* img = opj_image_create(OPJ_UINT32(s.comps), cp.data(), cs);
    if (!img) {
        return nullptr;
    }
    img->x0 = OPJ_UINT32(s.x0);
    img->y0 = OPJ_UINT32(s.y0);
    img->x1 = OPJ_UINT32(x1);
    img->y1 = OPJ_UINT32(y1);
    // integer arithmetic only, so every platform makes the same image
    Fnv64 seed;
    seed.bytes(s.name.data(), s.name.size());
    seed.u32(s.seed);
    Rng rng{seed.value() | 1};
    const std::int64_t maxval = (std::int64_t(1) << s.prec) - 1;
    for (int c = 0; c < s.comps; ++c) {
        const auto& comp = img->comps[c];
        for (OPJ_UINT32 y = 0; y < comp.h; ++y) {
            for (OPJ_UINT32 x = 0; x < comp.w; ++x) {
                // two triangle waves that differ per component, an interference term, seeded texture
                const std::int64_t fx = comp.w > 1 ? std::int64_t(x) * 1024 / (comp.w - 1) : 512;
                const std::int64_t fy = comp.h > 1 ? std::int64_t(y) * 1024 / (comp.h - 1) : 512;
                const std::int64_t t1 = (fx * 3 + fy * 5 + c * 400) % 2048, t2 = (fx * 7 - fy * 2 + 4096 + c * 900) % 2048;
                const std::int64_t tri1 = t1 < 1024 ? t1 : 2047 - t1, tri2 = t2 < 1024 ? t2 : 2047 - t2;
                std::int64_t v = 20 + (tri1 * 150 >> 10) + (tri2 * 60 >> 10) + ((fx * fy >> 12) & 15);
                v += std::int64_t(rng.next() % unsigned(2 * s.texture + 1)) - s.texture;
                v = std::clamp<std::int64_t>(v, 0, 255) * maxval / 255;
                if (s.sgnd) {
                    v -= std::int64_t(1) << (s.prec - 1);
                }
                comp.data[std::size_t(y) * comp.w + x] = OPJ_INT32(v);
            }
        }
    }
    return img;
}

// ---- encoding ------------------------------------------------------------------------------------------

struct Sink {
    Bytes* out;
    std::size_t pos = 0;
};

OPJ_SIZE_T sink_write(void* buf, OPJ_SIZE_T n, void* user)
{
    auto* s = static_cast<Sink*>(user);
    if (s->pos + n > s->out->size()) {
        s->out->resize(s->pos + n);
    }
    std::memcpy(s->out->data() + s->pos, buf, n);
    s->pos += n;
    return n;
}

OPJ_OFF_T sink_skip(OPJ_OFF_T n, void* user)
{
    auto* s = static_cast<Sink*>(user);
    s->pos = std::size_t(OPJ_OFF_T(s->pos) + n);
    if (s->pos > s->out->size()) {
        s->out->resize(s->pos);
    }
    return n;
}

OPJ_BOOL sink_seek(OPJ_OFF_T to, void* user)
{
    auto* s = static_cast<Sink*>(user);
    s->pos = std::size_t(to);
    if (s->pos > s->out->size()) {
        s->out->resize(s->pos);
    }
    return OPJ_TRUE;
}

void on_message(const char* msg, void* user)
{
    if (user) {
        *static_cast<std::string*>(user) += msg;
    }
}

}  // namespace

bool encode_generated(const GeneratedSpec& s, bool sop, bool eph, Bytes& out, std::string& err)
{
    const std::unique_ptr<opj_image_t, decltype(&opj_image_destroy)> img(make_image(s), opj_image_destroy);
    if (!img) {
        err = "image";
        return false;
    }
    opj_cparameters_t p;
    opj_set_default_encoder_parameters(&p);
    p.numresolution = s.numres;
    p.irreversible = s.irreversible ? 1 : 0;
    p.cblockw_init = s.cbw;
    p.cblockh_init = s.cbh;
    p.prog_order = s.prog;
    p.mode = s.mode;
    const bool equal_comps = s.sub == 1;
    p.tcp_mct = char(s.mct >= 0 ? s.mct : (s.comps >= 3 && equal_comps ? 1 : 0));
    if (s.rates.empty()) {
        p.tcp_numlayers = 1;
        p.tcp_rates[0] = 0;
    } else {
        p.tcp_numlayers = int(s.rates.size());
        for (std::size_t i = 0; i < s.rates.size(); ++i) {
            p.tcp_rates[i] = s.rates[i];
        }
    }
    p.cp_disto_alloc = 1;
    if (sop) {
        p.csty |= 0x02;
    }
    if (eph) {
        p.csty |= 0x04;
    }
    if (!s.precincts.empty()) {
        p.csty |= 0x01;
        p.res_spec = int(s.precincts.size());
        // OpenJPEG lists precinct sizes from the finest resolution down
        for (std::size_t i = 0; i < s.precincts.size(); ++i) {
            const int e = s.precincts[s.precincts.size() - 1 - i];
            p.prcw_init[i] = 1 << e;
            p.prch_init[i] = 1 << e;
        }
    }
    if (s.tw) {
        p.tile_size_on = OPJ_TRUE;
        p.cp_tdx = s.tw;
        p.cp_tdy = s.th;
        p.cp_tx0 = s.tx0;
        p.cp_ty0 = s.ty0;
    }
    if (s.poc) {
        // resolutions 0..2 layer by layer, then the rest resolution by resolution
        const int layers = p.tcp_numlayers;
        p.numpocs = 2;
        p.POC[0].tile = 1;
        p.POC[0].resno0 = 0;
        p.POC[0].compno0 = 0;
        p.POC[0].layno1 = OPJ_UINT32(layers);
        p.POC[0].resno1 = 3;
        p.POC[0].compno1 = OPJ_UINT32(s.comps);
        p.POC[0].prg1 = OPJ_LRCP;
        p.POC[1].tile = 1;
        p.POC[1].resno0 = 0;
        p.POC[1].compno0 = 0;
        p.POC[1].layno1 = OPJ_UINT32(layers);
        p.POC[1].resno1 = OPJ_UINT32(s.numres);
        p.POC[1].compno1 = OPJ_UINT32(s.comps);
        p.POC[1].prg1 = OPJ_RLCP;
    }
    char comment[] = "neo-j2k test corpus";
    p.cp_comment = comment;

    const std::unique_ptr<opj_codec_t, decltype(&opj_destroy_codec)> codec(
        opj_create_compress(OPJ_CODEC_J2K), opj_destroy_codec);
    out.clear();
    Sink sink{&out};
    const std::unique_ptr<opj_stream_t, decltype(&opj_stream_destroy)> stream(
        opj_stream_create(1 << 20, OPJ_FALSE), opj_stream_destroy);
    if (!codec || !stream) {
        err = "codec or output stream";
        return false;
    }
    opj_set_error_handler(codec.get(), on_message, &err);
    auto* st = stream.get();
    opj_stream_set_user_data(st, &sink, nullptr);
    opj_stream_set_write_function(st, sink_write);
    opj_stream_set_skip_function(st, sink_skip);
    opj_stream_set_seek_function(st, sink_seek);
    bool ok = opj_setup_encoder(codec.get(), &p, img.get()) != 0;
    if (ok && (s.plt || s.tlm)) {
        const char* opts[3] = {nullptr, nullptr, nullptr};
        int n = 0;
        if (s.plt) opts[n++] = "PLT=YES";
        if (s.tlm) opts[n++] = "TLM=YES";
        ok = opj_encoder_set_extra_options(codec.get(), opts) != 0;
    }
    ok = ok && opj_start_compress(codec.get(), img.get(), st) && opj_encode(codec.get(), st) &&
         opj_end_compress(codec.get(), st);
    out.resize(sink.pos);
    return ok;
}

GeneratedSpec generated_case(std::uint32_t seed, std::uint32_t index)
{
    Rng rng{(std::uint64_t(seed) << 32) | (std::uint64_t(index) + 1)};
    GeneratedSpec spec;
    spec.name = "matrix";
    spec.seed = rng.next();
    spec.w = 17 + int(rng.next() % 284);
    spec.h = 17 + int(rng.next() % 284);
    spec.comps = 1 + int(index / 2 % 5);
    spec.prec = 1 + int(index % 16);
    spec.sgnd = (index / 7 % 2) != 0;
    spec.irreversible = (index / 2 % 2) != 0;
    spec.sub = index % 7 == 2 || index % 7 == 5 ? 2 : 1;
    spec.x0 = int(rng.next() % 8);
    spec.y0 = int(rng.next() % 8);
    if (index % 17 < 2) {
        auto& thin = index % 17 == 0 ? spec.w : spec.h;
        thin = 1;
        spec.sub = 1;
        // OpenJPEG's packet output allocation is tight for these one-sample axes.
        // Exercise their geometry with a single lossless layer and no optional markers.
        spec.irreversible = false;
        spec.numres = 1;
        return spec;
    }
    if (spec.prec < 8) {
        // The pinned encoder sizes its tile output from the uncompressed bit count. Keep
        // the low-precision cases to one layer so packet headers do not exhaust that buffer.
        spec.numres = 1 + int(index % 3);
        spec.mct = 0;
        spec.irreversible = spec.irreversible && spec.prec >= 5;
        spec.rates = {spec.irreversible ? 2.0f : 0.0f};
        return spec;
    }
    // Leave enough raw samples per tile for OpenJPEG's packet-output buffer estimate.
    if (index % 6 == 1 && std::min(spec.w, spec.h) >= 64) {
        spec.tx0 = std::max(0, spec.x0 - 1);
        spec.ty0 = std::max(0, spec.y0 - 1);
        spec.tw = ceil_div(spec.w + spec.x0 - spec.tx0, 2);
        spec.th = ceil_div(spec.h + spec.y0 - spec.ty0, 2);
    }

    const unsigned side = static_cast<unsigned>(spec.tw ? std::min(spec.tw, spec.th) : std::min(spec.w, spec.h));
    spec.numres = std::min(1 + int(index % 7 * 3 % 7), std::bit_width(side));
    constexpr int blocks[] = {4, 8, 16, 32, 64};
    spec.cbw = blocks[index / 7 % 5];
    spec.cbh = blocks[index / 4 % 5];
    constexpr OPJ_PROG_ORDER orders[] = {OPJ_LRCP, OPJ_RLCP, OPJ_RPCL, OPJ_PCRL, OPJ_CPRL};
    spec.prog = orders[index % 5];
    constexpr int modes[] = {0, 1, 2, 4, 8, 16, 32, 1 | 4, 63};
    spec.mode = modes[index % 9];
    if (spec.mode & 4) {
        spec.texture = 4;
    }
    spec.sop = (index & 1) != 0;
    spec.eph = (index & 2) != 0;
    spec.plt = (index & 4) != 0;
    spec.tlm = (index & 8) != 0;
    if (index % 3 != 0) {
        for (int resolution = 0; resolution < spec.numres; ++resolution) {
            spec.precincts.push_back(3 + std::min(resolution, 3));
        }
    }
    spec.poc = index % 11 == 4 && !spec.tw && spec.numres > 3;
    if (spec.poc) {
        // Use the corpus generator's POC progression. The pinned encoder can omit EPH markers
        // when it switches progression, so exercise EPH on the cases with a single progression.
        spec.prog = OPJ_LRCP;
        spec.eph = false;
    }
    // The encoder's per-tile packet buffer derives from the raw tile size. Keeping tiled
    // images to one layer avoids exhausting that buffer on packet headers in small tiles.
    const int layers = spec.tw ? 1 : 1 + int(index / 3 % 8);
    for (int layer = layers; layer > 0; --layer) {
        spec.rates.push_back(layer == 1 && !spec.irreversible ? 0.0f : float(3 * layer));
    }
    return spec;
}

}  // namespace nj2k::test

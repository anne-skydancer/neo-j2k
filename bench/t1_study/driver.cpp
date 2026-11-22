// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
#include "driver.h"

#include <neoj2k/neoj2k_openjpeg.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace study {

namespace {

struct Grow {
    std::vector<std::uint8_t>* out;
    std::size_t pos;
};

OPJ_SIZE_T grow_write(void* buf, OPJ_SIZE_T n, void* user)
{
    auto* g = static_cast<Grow*>(user);
    if (g->pos + n > g->out->size()) {
        g->out->resize(g->pos + n);
    }
    std::memcpy(g->out->data() + g->pos, buf, n);
    g->pos += n;
    return n;
}

OPJ_OFF_T grow_skip(OPJ_OFF_T n, void* user)
{
    auto* g = static_cast<Grow*>(user);
    const OPJ_OFF_T to = static_cast<OPJ_OFF_T>(g->pos) + n;
    if (to < 0) {
        return static_cast<OPJ_OFF_T>(-1);
    }
    g->pos = static_cast<std::size_t>(to);
    if (g->pos > g->out->size()) {
        g->out->resize(g->pos);
    }
    return n;
}

OPJ_BOOL grow_seek(OPJ_OFF_T to, void* user)
{
    auto* g = static_cast<Grow*>(user);
    if (to < 0) {
        return OPJ_FALSE;
    }
    g->pos = static_cast<std::size_t>(to);
    if (g->pos > g->out->size()) {
        g->out->resize(g->pos);
    }
    return OPJ_TRUE;
}

void on_info(const char*, void*) {}

}  // namespace

std::vector<std::uint8_t> texture(int width, int height, int comps)
{
    // bench/nj2k_encode_bench.cpp's texture: gradients, a few edges and noise
    std::vector<std::uint8_t> raw(std::size_t(width) * height * comps);
    std::uint32_t x_state = 0x2545F491u;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < comps; ++c) {
                x_state ^= x_state << 13;
                x_state ^= x_state >> 17;
                x_state ^= x_state << 5;
                const int edge = ((x / 97 + y / 61 + c) & 1) * 60;
                const int v = (x * (c + 1) / 9 + y / (c + 2) + edge + static_cast<int>(x_state % 23)) & 0xFF;
                raw[(std::size_t(y) * width + x) * comps + c] = static_cast<std::uint8_t>(v);
            }
        }
    }
    return raw;
}

bool upload(const std::vector<std::uint8_t>& rawv, int width, int height, int comps, bool reversible,
            std::vector<std::uint8_t>& out, UploadTimes* times)
{
    const std::uint64_t t0 = __rdtsc();
    const std::uint8_t* raw = rawv.data();
    out.clear();
    opj_cparameters_t p;
    std::memset(&p, 0, sizeof(p));
    opj_set_default_encoder_parameters(&p);
    p.cod_format = OPJ_CODEC_J2K;
    p.prog_order = OPJ_RLCP;
    p.cp_disto_alloc = 1;
    if (reversible) {
        p.irreversible = 0;
        p.tcp_numlayers = 1;
        p.tcp_rates[0] = 0.0f;
    } else {
        p.irreversible = 1;
    }
    char comment_text[] = "no comment";  // the viewer's (tests/common/decoder.h UploadSpec)
    p.cp_comment = comment_text;
    std::vector<opj_image_cmptparm_t> cp(static_cast<std::size_t>(comps));
    for (auto& c : cp) {
        std::memset(&c, 0, sizeof(c));
        c.prec = 8;
        c.dx = static_cast<OPJ_UINT32>(p.subsampling_dx);
        c.dy = static_cast<OPJ_UINT32>(p.subsampling_dy);
        c.w = static_cast<OPJ_UINT32>(width);
        c.h = static_cast<OPJ_UINT32>(height);
    }
    opj_image_t* image = opj_image_create(static_cast<OPJ_UINT32>(comps), cp.data(), OPJ_CLRSPC_SRGB);
    if (!image) {
        return false;
    }
    image->x1 = static_cast<OPJ_UINT32>(width);
    image->y1 = static_cast<OPJ_UINT32>(height);
    std::size_t i = 0;
    for (int y = height - 1; y >= 0; --y) {
        for (int x = 0; x < width; ++x, ++i) {
            const std::uint8_t* px = raw + (static_cast<std::size_t>(y) * width + x) * comps;
            for (int c = 0; c < comps; ++c) {
                image->comps[c].data[i] = px[c];
            }
        }
    }
    opj_codec_t* enc = opj_create_compress(OPJ_CODEC_J2K);
    if (!enc) {
        opj_image_destroy(image);
        return false;
    }
    opj_set_error_handler(enc, on_info, nullptr);
    opj_set_warning_handler(enc, on_info, nullptr);
    opj_set_info_handler(enc, on_info, nullptr);
    p.tcp_mct = static_cast<char>(image->numcomps >= 3 ? 1 : 0);
    if (p.irreversible) {
        const std::uint32_t surface = static_cast<std::uint32_t>(width) * static_cast<std::uint32_t>(height);
        const int layers = surface <= 1024 ? 2 : surface <= 16384 ? 3 : surface <= 262144 ? 4 : surface <= 1048576 ? 5 : 6;
        p.tcp_numlayers = layers;
        for (int k = layers - 1; k >= 0; --k) {
            p.tcp_rates[layers - 1 - k] = 8.0f * static_cast<float>(1 << (k << 1));
        }
    }
    std::uint32_t wt = static_cast<std::uint32_t>(width) >> 6, ht = static_cast<std::uint32_t>(height) >> 6;
    if (wt == 0 && width >= 4) {
        wt = 1;
    }
    if (ht == 0 && height >= 4) {
        ht = 1;
    }
    if (wt == 1 || ht == 1) {
        const int min_dim = width < height ? width : height;
        int levels = 0;
        while ((2 << levels) <= min_dim) {
            ++levels;
        }
        p.numresolution = 1 + levels;
    }
    const std::uint64_t t1 = __rdtsc();
    bool ok = opj_setup_encoder(enc, &p, image) != 0;
    const std::uint64_t t2 = __rdtsc();
    opj_stream_t* st = nullptr;
    Grow sink{&out, 0};
    std::uint64_t t3 = t2, t4 = t2, t5 = t2;
    if (ok) {
        const std::size_t guess = static_cast<std::size_t>(wt) * ht * 64 * 64 * 3;
        out.assign(guess, 0);
        st = opj_stream_create(guess, OPJ_FALSE);
        ok = st != nullptr;
    }
    if (ok) {
        opj_stream_set_user_data(st, &sink, nullptr);
        opj_stream_set_user_data_length(st, out.size());
        opj_stream_set_write_function(st, grow_write);
        opj_stream_set_skip_function(st, grow_skip);
        opj_stream_set_seek_function(st, grow_seek);
        const std::uint64_t ts = __rdtsc();
        ok = opj_start_compress(enc, image, st);
        t3 = __rdtsc();
        ok = ok && opj_encode(enc, st);
        t4 = __rdtsc();
        ok = ok && opj_end_compress(enc, st);
        t5 = __rdtsc();
        if (times) {
            times->start += t3 - ts;
        }
    }
    out.resize(ok ? sink.pos : 0);
    if (st) {
        opj_stream_destroy(st);
    }
    opj_destroy_codec(enc);
    opj_image_destroy(image);
    const std::uint64_t t6 = __rdtsc();
    if (times) {
        times->setup += t2 - t1;
        times->encode += t4 - t3;
        times->end += t5 - t4;
        times->total += t6 - t0;
    }
    return ok;
}

std::uint64_t fnv64(const std::vector<std::uint8_t>& v)
{
    std::uint64_t h = 1469598103934665603ull;
    for (std::uint8_t b : v) {
        h ^= b;
        h *= 1099511628211ull;
    }
    return h;
}

double tsc_ghz()
{
    const auto c0 = std::chrono::steady_clock::now();
    const std::uint64_t t0 = __rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto c1 = std::chrono::steady_clock::now();
    const std::uint64_t t1 = __rdtsc();
    return double(t1 - t0) / std::chrono::duration<double, std::nano>(c1 - c0).count();
}

}  // namespace study

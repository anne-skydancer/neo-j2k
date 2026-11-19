// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// Fuzz target: lossless round trip. The input chooses a small image (1-64 px a side, 1-5
// components, 1-16 bits, signed or not) and its coding (resolutions, code-block size, progression,
// mode switches, layers, SOP, EPH, the colour transform); the remaining bytes are the samples. The
// image is encoded reversibly (5/3) and decoded: every sample must come back exactly, unless the
// oracle's encoder writes a stream that cannot hold them (OD-6: too few guard bits), which is still
// encoded and decoded. Today both ends are the oracle; stage 6a encodes with neo-j2k and decodes
// with both. A failed check says why on stderr, then aborts.
#include <openjpeg.h>

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "fuzz_common.h"

namespace {

// The known-defect checks of OD-4 (AddressSanitizer builds) and OD-6 (tests in fuzz/CMakeLists.txt)
// replay their inputs with this set and expect the defect's report; nothing else sets it.
bool unguarded()
{
    static const bool on = nj2k::test::env_value("NJ2K_FUZZ_UNGUARDED") == "1";
    return on;
}

[[noreturn]] void fail(const char* format, ...)
{
    std::va_list args;
    va_start(args, format);
    std::fputs("round trip: ", stderr);
    std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
    va_end(args);
    std::fflush(stderr);
    std::abort();
}

// One level of OpenJPEG's forward 5/3 lifting (opj_dwt_encode_and_deinterleave_h_one_row and _v
// with an even origin: low samples at the even places, an index past either end taking the nearest
// sample of its kind) on n samples `stride` apart, written back with the low samples first. One
// sample stays as it is.
void lift53(std::int32_t* a, int n, int stride, std::vector<std::int32_t>& x)
{
    if (n < 2) {
        return;
    }
    const int sn = (n + 1) / 2, dn = n / 2;
    x.resize(std::size_t(n));
    for (int i = 0; i < n; ++i) {
        x[std::size_t(i)] = a[std::ptrdiff_t(i) * stride];
    }
    auto s = [&](int i) -> std::int32_t& { return x[std::size_t(2 * std::clamp(i, 0, sn - 1))]; };
    auto d = [&](int i) -> std::int32_t& { return x[std::size_t(2 * std::clamp(i, 0, dn - 1) + 1)]; };
    for (int i = 0; i < dn; ++i) {
        d(i) -= (s(i) + s(i + 1)) >> 1;
    }
    for (int i = 0; i < sn; ++i) {
        s(i) += (d(i - 1) + d(i) + 2) >> 2;
    }
    for (int i = 0; i < sn; ++i) {
        a[std::ptrdiff_t(i) * stride] = s(i);
    }
    for (int i = 0; i < dn; ++i) {
        a[std::ptrdiff_t(sn + i) * stride] = d(i);
    }
}

// OD-6 (doc/oracle.md): OpenJPEG's encoder gives every band two guard bits and never checks that
// the code-blocks fit. A band holds Mb = precision + gain + guard bits - 1 bit planes (gain 0 for
// LL, 1 for HL and LH, 2 for HH); a code-block that needs more gets a negative zero-bit-plane count
// (t2.c), which the tag tree writes as 0, so its samples decode to other values without a word.
// True when every band of every component fits, by the transform the encoder applies to this
// image (one tile at the origin): the DC level shift, the RCT when asked, then the 5/3 levels,
// columns before rows.
bool guard_bits_suffice(std::vector<std::vector<std::int32_t>> planes, int w, int h, int prec, bool sgnd, bool mct,
                        int numres)
{
    constexpr int kGuardBits = 2;  // opj_j2k_setup_encoder, outside the Cinema 2K profile
    if (!sgnd) {
        for (auto& plane : planes) {
            for (auto& v : plane) {
                v -= 1 << (prec - 1);
            }
        }
    }
    if (mct) {
        for (std::size_t i = 0; i < planes[0].size(); ++i) {
            const std::int32_t r = planes[0][i], g = planes[1][i], b = planes[2][i];
            planes[0][i] = (r + 2 * g + b) >> 2;
            planes[1][i] = b - g;
            planes[2][i] = r - g;
        }
    }
    std::vector<std::int32_t> line;
    for (auto& plane : planes) {
        // the largest magnitude in [x0, x1) x [y0, y1) within the band's bit planes
        auto fits = [&](int x0, int x1, int y0, int y1, int gain) {
            std::int32_t most = 0;
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    const std::int32_t v = plane[std::size_t(y) * std::size_t(w) + std::size_t(x)];
                    most = std::max(most, v < 0 ? -v : v);
                }
            }
            return most < (std::int32_t(1) << (prec + gain + kGuardBits - 1));
        };
        int rw = w, rh = h;
        for (int level = 1; level < numres; ++level) {
            for (int x = 0; x < rw; ++x) {
                lift53(plane.data() + x, rh, w, line);
            }
            for (int y = 0; y < rh; ++y) {
                lift53(plane.data() + std::ptrdiff_t(y) * w, rw, 1, line);
            }
            const int lw = (rw + 1) / 2, lh = (rh + 1) / 2;
            if (!fits(lw, rw, 0, lh, 1) || !fits(0, lw, lh, rh, 1) || !fits(lw, rw, lh, rh, 2)) {
                return false;
            }
            rw = lw;
            rh = lh;
        }
        if (!fits(0, rw, 0, rh, 0)) {
            return false;
        }
    }
    return true;
}

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
    nj2k::fuzz::fuzz_setup();
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
    // OpenJPEG's MQ encoder writes past the code-block buffer when every pass is terminated
    // (TERMALL) on samples deeper than 8 bits (oracle defect OD-4, doc/oracle.md): kept away from
    // the oracle until neo-j2k's bounded encoder (stage 6a) takes this target over
    if ((mode & 4) && prec > 8 && !unguarded()) {
        return 0;
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
        ++nj2k::fuzz::exercised();
        static const auto oracle = nj2k::test::make_oracle_backend();
        nj2k::test::Request rq;
        rq.data = sink.bytes.data();
        rq.size = sink.pos;
        rq.capture = true;
        const nj2k::test::Outcome o = oracle->decode(rq);
        // a stream the encoder wrote must decode, and losslessly, where its bands hold the image
        if (unguarded() || guard_bits_suffice(expected, w, h, prec, sgnd, mct, numres)) {
            if (!o.ok) {
                fail("the oracle cannot decode what its encoder wrote (%s: %s)", o.stage.c_str(), o.last_error.c_str());
            }
            if (o.numcomps != OPJ_UINT32(comps)) {
                fail("%u components decoded for %d", o.numcomps, comps);
            }
            for (int c = 0; c < comps; ++c) {
                const auto& got = o.planes[std::size_t(c)];
                const auto& want = expected[std::size_t(c)];
                if (got.size() != want.size()) {
                    fail("component %d: %zu samples decoded for %zu", c, got.size(), want.size());
                }
                std::size_t wrong = 0, first = 0;
                for (std::size_t i = 0; i < want.size(); ++i) {
                    if (got[i] != want[i] && wrong++ == 0) {
                        first = i;
                    }
                }
                if (wrong) {
                    fail("component %d: %zu of %zu samples differ, the first at (%zu, %zu): %d for %d", c, wrong,
                         want.size(), first % std::size_t(w), first / std::size_t(w), got[first], want[first]);
                }
            }
        }
    }
    opj_image_destroy(image);
    return 0;
}

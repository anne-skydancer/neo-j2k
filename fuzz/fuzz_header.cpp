// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// Fuzz target: header parsing only. The test scanner (j2k_scan) reads the main header, the
// tile-parts and the packet positions, and the oracle reads the main header and its codestream
// information, non-strict, as the viewer's metadata pass does. Stage 1b points it at neo-j2k's
// header model.
#include <openjpeg.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "fuzz_common.h"

namespace {

struct Mem {
    const std::uint8_t* p;
    std::size_t n, at;
};

OPJ_SIZE_T mem_read(void* out, OPJ_SIZE_T bytes, void* user)
{
    auto* m = static_cast<Mem*>(user);
    if (m->at >= m->n) {
        return static_cast<OPJ_SIZE_T>(-1);
    }
    const std::size_t k = bytes < m->n - m->at ? bytes : m->n - m->at;
    std::memcpy(out, m->p + m->at, k);
    m->at += k;
    return k;
}

OPJ_OFF_T mem_skip(OPJ_OFF_T bytes, void* user)
{
    auto* m = static_cast<Mem*>(user);
    const OPJ_OFF_T to = OPJ_OFF_T(m->at) + bytes;
    if (to < 0 || to > OPJ_OFF_T(m->n)) {
        m->at = to < 0 ? 0 : m->n;
        return -1;
    }
    m->at = std::size_t(to);
    return bytes;
}

OPJ_BOOL mem_seek(OPJ_OFF_T to, void* user)
{
    auto* m = static_cast<Mem*>(user);
    if (to < 0 || to > OPJ_OFF_T(m->n)) {
        return OPJ_FALSE;
    }
    m->at = std::size_t(to);
    return OPJ_TRUE;
}

void quiet(const char*, void*) {}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::span<const std::uint8_t> bytes(data, size);
    if (const auto mh = nj2k::test::parse_main_header(bytes)) {
        for (const auto& tp : nj2k::test::scan_tile_parts(bytes, *mh)) {
            (void)nj2k::test::packet_starts(bytes, *mh, tp);
        }
        (void)nj2k::test::layer_boundaries(bytes);
    }
    if (size == 0 || !nj2k::fuzz::oracle_may_decode(bytes, 0)) {
        return 0;
    }
    opj_dparameters_t params;
    opj_set_default_decoder_parameters(&params);
    params.flags |= OPJ_DPARAMETERS_DUMP_FLAG;
    opj_codec_t* codec = opj_create_decompress(OPJ_CODEC_J2K);
    opj_set_info_handler(codec, quiet, nullptr);
    opj_set_warning_handler(codec, quiet, nullptr);
    opj_set_error_handler(codec, quiet, nullptr);
    opj_setup_decoder(codec, &params);
    opj_decoder_set_strict_mode(codec, OPJ_FALSE);
    Mem mem{data, size, 0};
    opj_stream_t* stream = opj_stream_create(size, OPJ_TRUE);
    opj_stream_set_user_data(stream, &mem, nullptr);
    opj_stream_set_user_data_length(stream, size);
    opj_stream_set_read_function(stream, mem_read);
    opj_stream_set_skip_function(stream, mem_skip);
    opj_stream_set_seek_function(stream, mem_seek);
    opj_image_t* image = nullptr;
    if (opj_read_header(stream, codec, &image)) {
        opj_codestream_info_v2_t* info = opj_get_cstr_info(codec);
        if (info) {
            opj_destroy_cstr_info(&info);
        }
    }
    opj_image_destroy(image);
    opj_stream_destroy(stream);
    opj_destroy_codec(codec);
    return 0;
}

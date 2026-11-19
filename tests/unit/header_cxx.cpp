// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The public headers, C++ wrapper included, compile as strict C++20 with every warning as an
// error. Nothing here runs.
#include <neoj2k/neoj2k.hpp>
#include <neoj2k/neoj2k_openjpeg.h>

int nj2k_header_check_cxx();

int nj2k_header_check_cxx()
{
    const std::uint8_t bytes[4] = {0xFF, 0x4F, 0xFF, 0x51};
    nj2k::MemoryReader reader(bytes);
    nj2k::Stream stream = reader.make_stream();
    nj2k::Codec codec = nj2k::make_decoder();
    nj2k::Image image;
    opj_dparameters_t parameters = nj2k::default_decoder_parameters();
    return static_cast<int>(parameters.cp_layer) + (stream ? 1 : 0) + (codec ? 1 : 0) + (image ? 1 : 0) +
           static_cast<int>(nj2k::engine_string().size());
}

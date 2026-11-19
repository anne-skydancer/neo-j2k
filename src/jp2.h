// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// A read-only reader of the JP2 and JPH file formats (ISO 15444-1 Annex I, 15444-15): the signature
// and file type boxes, the JP2 header box (image header, bits per component, colour specification,
// palette, component mapping, channel definitions), and where the codestream box starts. It reads
// the boxes before the codestream only, with OpenJPEG 2.5.4's checks (jp2.c); the palette and the
// channel definitions are applied to decoded images (stage 1c). No box is written.
#pragma once

#include <cstdint>

#include <neoj2k/neoj2k.h>

#include "arena.h"
#include "header.h"
#include "messages.h"

namespace nj2k::detail {

struct Jp2CmapEntry {
    std::uint16_t cmp;
    std::uint8_t mtyp, pcol;
};

struct Jp2CdefEntry {
    std::uint16_t cn, typ, asoc;
};

struct Jp2Header {
    // image header (ihdr)
    std::uint32_t w, h;
    std::uint16_t numcomps;
    std::uint8_t bpc, c, unkc, ipr;
    const std::uint8_t* bpcc;  // numcomps bytes, or null
    // the colour specification that counts: the first one of method 1 or 2
    bool has_colr;
    std::uint8_t meth, precedence, approx;
    std::uint32_t enumcs;
    const std::uint8_t* icc;  // method 2: the ICC profile
    std::uint32_t icc_len;
    bool cielab;  // enumerated colour space 14, with its 9 values (OpenJPEG's layout)
    std::uint32_t cielab_values[9];
    // palette
    bool has_pclr;
    std::uint16_t pclr_entries;
    std::uint8_t pclr_channels;
    const std::uint8_t* pclr_bits;     // per channel: Bi as read (depth less one, sign in bit 7)
    const std::uint32_t* pclr_values;  // pclr_entries x pclr_channels
    // component mapping and channel definitions
    const Jp2CmapEntry* cmap;  // pclr_channels entries, or null
    const Jp2CdefEntry* cdef;
    std::uint16_t cdef_count;
    // the codestream box's contents: where they start, and their length (0: to the end)
    std::uint64_t codestream, codestream_length;
};

// Reads the boxes up to and including the codestream box's header; the source is then at the
// codestream's first byte.
nj2k_status_t read_jp2_boxes(Source& src, Arena& arena, const Messages& msg, Jp2Header*& out) noexcept;

// The image colour space OpenJPEG gives a JP2 file (opj_jp2_read_header).
NJ2K_COLOR_SPACE jp2_color_space(const Jp2Header& jp2) noexcept;

}  // namespace nj2k::detail

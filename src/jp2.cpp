// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The JP2 box reader. Its checks and messages derive from OpenJPEG's jp2.c, whose notice follows.
//
/*
 * The copyright in this software is being made available under the 2-clauses
 * BSD License, included below. This software may be subject to other third
 * party and contributor rights, including patent rights, and no such rights
 * are granted under this license.
 *
 * Copyright (c) 2002-2014, Universite catholique de Louvain (UCL), Belgium
 * Copyright (c) 2002-2014, Professor Benoit Macq
 * Copyright (c) 2001-2003, David Janssens
 * Copyright (c) 2002-2003, Yannick Verschueren
 * Copyright (c) 2003-2007, Francois-Olivier Devaux
 * Copyright (c) 2003-2014, Antonin Descampe
 * Copyright (c) 2005, Herve Drolon, FreeImage Team
 * Copyright (c) 2010-2011, Kaori Hagihara
 * Copyright (c) 2008, 2011-2012, Centre National d'Etudes Spatiales (CNES), FR
 * Copyright (c) 2012, CS Systemes d'Information, France
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS `AS IS'
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include "jp2.h"

#include <cstdarg>
#include <cstring>

namespace nj2k::detail {

namespace {

constexpr std::uint32_t box(char a, char b, char c, char d) noexcept
{
    return std::uint32_t(std::uint8_t(a)) << 24 | std::uint32_t(std::uint8_t(b)) << 16 | std::uint32_t(std::uint8_t(c)) << 8 |
           std::uint8_t(d);
}

constexpr std::uint32_t kJP = box('j', 'P', ' ', ' '), kFTYP = box('f', 't', 'y', 'p'), kJP2H = box('j', 'p', '2', 'h'),
                        kJP2C = box('j', 'p', '2', 'c'), kIHDR = box('i', 'h', 'd', 'r'), kCOLR = box('c', 'o', 'l', 'r'),
                        kBPCC = box('b', 'p', 'c', 'c'), kPCLR = box('p', 'c', 'l', 'r'), kCMAP = box('c', 'm', 'a', 'p'),
                        kCDEF = box('c', 'd', 'e', 'f');

// The signature box: length 12, type 'jP  ', contents 0D 0A 87 0A.
constexpr std::uint8_t kSignature[12] = {0x00, 0x00, 0x00, 0x0C, 0x6A, 0x50, 0x20, 0x20, 0x0D, 0x0A, 0x87, 0x0A};

std::uint16_t be16(const std::uint8_t* p) noexcept
{
    return std::uint16_t(std::uint32_t(p[0]) << 8 | p[1]);
}

std::uint32_t be32(const std::uint8_t* p) noexcept
{
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3];
}

std::uint32_t be_n(const std::uint8_t* p, std::uint32_t n) noexcept
{
    std::uint32_t v = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        v = v << 8 | p[i];
    }
    return v;
}

nj2k_status_t fail(const Messages& msg, nj2k_status_t status, const char* format, ...) noexcept NJ2K_PRINTF_LIKE(3, 4);

nj2k_status_t fail(const Messages& msg, nj2k_status_t status, const char* format, ...) noexcept
{
    va_list args;
    va_start(args, format);
    msg.vemit(Messages::Error, format, args);
    va_end(args);
    return status;
}

nj2k_status_t need_data(const Messages& msg) noexcept
{
    return fail(msg, NJ2K_E_NEED_DATA, "Stream too short: the bytes end before the JP2 codestream box\n");
}

nj2k_status_t arena_failure(const Messages& msg, const Arena& arena) noexcept
{
    if (arena.failure() == Arena::Failure::budget) {
        return fail(msg, NJ2K_E_LIMIT, "The JP2 boxes need more memory than the decoder's limit\n");
    }
    return fail(msg, NJ2K_E_NO_MEMORY, "Not enough memory to read the JP2 boxes\n");
}

struct Reader {
    Jp2Header& jp2;
    Arena& arena;
    const Messages& msg;
    bool has_ihdr = false;

    nj2k_status_t ihdr(const std::uint8_t* p, std::uint32_t n) noexcept
    {
        if (has_ihdr) {
            msg.emit(Messages::Warning, "Ignoring ihdr box. First ihdr box already read\n");
            return NJ2K_OK;
        }
        if (n != 14) {
            return fail(msg, NJ2K_E_HEADER, "Bad image header box (bad size)\n");
        }
        const std::uint32_t h = be32(p), w = be32(p + 4), nc = be16(p + 8);
        if (h < 1 || w < 1 || nc < 1) {
            return fail(msg, NJ2K_E_HEADER, "Wrong values for: w(%u) h(%u) numcomps(%u) (ihdr)\n", w, h, nc);
        }
        if (nc - 1 >= 16384) {
            return fail(msg, NJ2K_E_HEADER, "Invalid number of components (ihdr)\n");
        }
        jp2.h = h;
        jp2.w = w;
        jp2.numcomps = std::uint16_t(nc);
        jp2.bpc = p[10];
        jp2.c = p[11];
        jp2.unkc = p[12];
        jp2.ipr = p[13];
        if (jp2.c != 7) {
            msg.emit(Messages::Info,
                     "JP2 IHDR box: compression type indicate that the file is not a conforming JP2 file (%u) \n",
                     unsigned(jp2.c));
        }
        has_ihdr = true;
        return NJ2K_OK;
    }

    nj2k_status_t bpcc(const std::uint8_t* p, std::uint32_t n) noexcept
    {
        if (jp2.bpc != 255) {
            msg.emit(Messages::Warning,
                     "A BPCC header box is available although BPC given by the IHDR box (%u) indicate components bit "
                     "depth is constant\n",
                     unsigned(jp2.bpc));
        }
        if (n != jp2.numcomps) {
            return fail(msg, NJ2K_E_HEADER, "Bad BPCC header box (bad size)\n");
        }
        // an empty box (before the image header, which sets numcomps) gives no values: the image
        // header then sets numcomps, and the pointer would promise bytes the box never had
        jp2.bpcc = n ? p : nullptr;
        return NJ2K_OK;
    }

    nj2k_status_t colr(const std::uint8_t* p, std::uint32_t n) noexcept
    {
        if (n < 3) {
            return fail(msg, NJ2K_E_HEADER, "Bad COLR header box (bad size)\n");
        }
        // ISO 15444-1 I.5.3.3: a reader ignores every colour specification after the first
        if (jp2.has_colr) {
            msg.emit(Messages::Info,
                     "A conforming JP2 reader shall ignore all Colour Specification boxes after the first, so we "
                     "ignore this one.\n");
            return NJ2K_OK;
        }
        const std::uint8_t meth = p[0];
        if (meth == 1) {
            if (n < 7) {
                return fail(msg, NJ2K_E_HEADER, "Bad COLR header box (bad size: %u)\n", n);
            }
            const std::uint32_t enumcs = be32(p + 3);
            if (n > 7 && enumcs != 14) {
                msg.emit(Messages::Warning, "Bad COLR header box (bad size: %u)\n", n);
            }
            jp2.enumcs = enumcs;
            if (enumcs == 14) {
                // CIELab: OpenJPEG keeps its parameters where the ICC profile goes, with length 0
                std::uint32_t v[7] = {0, 0, 0, 0, 0, 0, 0x00443530u};  // rl ol ra oa rb ob, illuminant D50
                std::uint32_t def = 0x44454600u;                          // "DEF": default ranges
                if (n == 35) {
                    for (int i = 0; i < 7; ++i) {
                        v[i] = be32(p + 7 + 4 * i);
                    }
                    def = 0;
                } else if (n != 7) {
                    msg.emit(Messages::Warning, "Bad COLR header box (CIELab, bad size: %u)\n", n);
                }
                const std::uint32_t values[9] = {14, def, v[0], v[1], v[2], v[3], v[4], v[5], v[6]};
                std::memcpy(jp2.cielab_values, values, sizeof(values));
                jp2.cielab = true;
            }
            jp2.has_colr = true;
        } else if (meth == 2) {
            if (n == 3) {
                return fail(msg, NJ2K_E_HEADER, "Bad COLR header box (empty ICC profile)\n");
            }
            jp2.icc = p + 3;
            jp2.icc_len = n - 3;
            jp2.has_colr = true;
        } else {
            msg.emit(Messages::Info,
                     "COLR BOX meth value is not a regular value (%u), so we will ignore the entire Colour "
                     "Specification box. \n",
                     unsigned(meth));
            return NJ2K_OK;
        }
        jp2.meth = meth;
        jp2.precedence = p[1];
        jp2.approx = p[2];
        return NJ2K_OK;
    }

    nj2k_status_t pclr(const std::uint8_t* p, std::uint32_t n) noexcept
    {
        if (jp2.has_pclr) {
            return fail(msg, NJ2K_E_HEADER, "Only one PCLR box is allowed.\n");
        }
        if (n < 3) {
            return fail(msg, NJ2K_E_HEADER, "Bad PCLR box (bad size)\n");
        }
        const std::uint32_t entries = be16(p), channels = p[2];
        if (entries == 0 || entries > 1024) {
            return fail(msg, NJ2K_E_HEADER, "Invalid PCLR box. Reports %u entries\n", entries);
        }
        if (channels == 0) {
            return fail(msg, NJ2K_E_HEADER, "Invalid PCLR box. Reports 0 palette columns\n");
        }
        if (n < 3 + channels) {
            return fail(msg, NJ2K_E_HEADER, "Bad PCLR box (bad size)\n");
        }
        // the box must hold every entry before the values are allocated: 1,024 entries of 255
        // columns would otherwise take 1 MiB from a box of a few bytes (OpenJPEG allocates first and
        // fails at the first entry the box lacks)
        const std::uint8_t* bits = p + 3;
        std::uint64_t entry_bytes = 0;
        for (std::uint32_t i = 0; i < channels; ++i) {
            const std::uint32_t bytes = ((bits[i] & 0x7Fu) + 1 + 7) >> 3;
            entry_bytes += bytes > 4 ? 4 : bytes;
        }
        if (std::uint64_t(n - 3 - channels) < entry_bytes * entries) {
            return fail(msg, NJ2K_E_HEADER, "Bad PCLR box (bad size)\n");
        }
        auto* values = arena.make_array<std::uint32_t>(std::size_t(entries) * channels);
        if (!values) {
            return arena_failure(msg, arena);
        }
        std::uint32_t at = 3 + channels;
        for (std::uint32_t j = 0; j < entries; ++j) {
            for (std::uint32_t i = 0; i < channels; ++i) {
                std::uint32_t bytes = ((bits[i] & 0x7Fu) + 1 + 7) >> 3;
                bytes = bytes > 4 ? 4 : bytes;
                values[std::size_t(j) * channels + i] = be_n(p + at, bytes);
                at += bytes;
            }
        }
        jp2.has_pclr = true;
        jp2.pclr_entries = std::uint16_t(entries);
        jp2.pclr_channels = std::uint8_t(channels);
        jp2.pclr_bits = bits;
        jp2.pclr_values = values;
        return NJ2K_OK;
    }

    nj2k_status_t cmap(const std::uint8_t* p, std::uint32_t n) noexcept
    {
        if (!jp2.has_pclr) {
            return fail(msg, NJ2K_E_HEADER, "Need to read a PCLR box before the CMAP box.\n");
        }
        if (jp2.cmap) {
            return fail(msg, NJ2K_E_HEADER, "Only one CMAP box is allowed.\n");
        }
        const std::uint32_t channels = jp2.pclr_channels;
        if (n < channels * 4) {
            return fail(msg, NJ2K_E_HEADER, "Insufficient data for CMAP box.\n");
        }
        auto* entries = arena.make_array<Jp2CmapEntry>(channels);
        if (!entries) {
            return arena_failure(msg, arena);
        }
        for (std::uint32_t i = 0; i < channels; ++i) {
            entries[i] = Jp2CmapEntry{be16(p + 4 * i), p[4 * i + 2], p[4 * i + 3]};
        }
        jp2.cmap = entries;
        return NJ2K_OK;
    }

    nj2k_status_t cdef(const std::uint8_t* p, std::uint32_t n) noexcept
    {
        if (jp2.cdef) {
            return fail(msg, NJ2K_E_HEADER, "Only one CDEF box is allowed.\n");
        }
        if (n < 2) {
            return fail(msg, NJ2K_E_HEADER, "Insufficient data for CDEF box.\n");
        }
        const std::uint32_t count = be16(p);
        if (count == 0) {
            return fail(msg, NJ2K_E_HEADER, "Number of channel description is equal to zero in CDEF box.\n");
        }
        if (n < 2 + count * 6) {
            return fail(msg, NJ2K_E_HEADER, "Insufficient data for CDEF box.\n");
        }
        auto* entries = arena.make_array<Jp2CdefEntry>(count);
        if (!entries) {
            return arena_failure(msg, arena);
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint8_t* e = p + 2 + 6 * i;
            entries[i] = Jp2CdefEntry{be16(e), be16(e + 2), be16(e + 4)};
        }
        jp2.cdef = entries;
        jp2.cdef_count = std::uint16_t(count);
        return NJ2K_OK;
    }

    // a box of the JP2 header, or null when the type is not one
    nj2k_status_t image_box(std::uint32_t type, const std::uint8_t* p, std::uint32_t n, bool& known) noexcept
    {
        known = true;
        switch (type) {
        case kIHDR:
            return ihdr(p, n);
        case kCOLR:
            return colr(p, n);
        case kBPCC:
            return bpcc(p, n);
        case kPCLR:
            return pclr(p, n);
        case kCMAP:
            return cmap(p, n);
        case kCDEF:
            return cdef(p, n);
        default:
            known = false;
            return NJ2K_OK;
        }
    }

    // The JP2 header box: a superbox of image boxes; it must hold an image header.
    nj2k_status_t jp2h(const std::uint8_t* p, std::uint32_t n) noexcept
    {
        bool saw_ihdr = false;
        while (n > 0) {
            if (n < 8) {
                return fail(msg, NJ2K_E_HEADER, "Cannot handle box of less than 8 bytes\n");
            }
            std::uint32_t length = be32(p), header = 8;
            const std::uint32_t type = be32(p + 4);
            if (length == 1) {
                if (n < 16) {
                    return fail(msg, NJ2K_E_HEADER, "Cannot handle XL box of less than 16 bytes\n");
                }
                if (be32(p + 8) != 0) {
                    return fail(msg, NJ2K_E_HEADER, "Cannot handle box sizes higher than 2^32\n");
                }
                length = be32(p + 12);
                header = 16;
            }
            if (length == 0) {
                return fail(msg, NJ2K_E_HEADER, "Cannot handle box of undefined sizes\n");
            }
            if (length < header) {
                return fail(msg, NJ2K_E_HEADER, "Box length is inconsistent.\n");
            }
            if (length > n) {
                return fail(msg, NJ2K_E_HEADER,
                            "Stream error while reading JP2 Header box: box length is inconsistent.\n");
            }
            bool known = false;
            if (const nj2k_status_t st = image_box(type, p + header, length - header, known); st != NJ2K_OK) {
                return st;
            }
            saw_ihdr = saw_ihdr || type == kIHDR;
            p += length;
            n -= length;
        }
        if (!saw_ihdr) {
            return fail(msg, NJ2K_E_HEADER, "Stream error while reading JP2 Header box: no 'ihdr' box.\n");
        }
        return NJ2K_OK;
    }
};

}  // namespace

nj2k_status_t read_jp2_boxes(Source& src, Arena& arena, const Messages& msg, Jp2Header*& out) noexcept
{
    auto* jp2 = arena.make<Jp2Header>();
    if (!jp2) {
        return arena_failure(msg, arena);
    }
    Reader rd{*jp2, arena, msg};
    bool signature = false, file_type = false, header = false;
    for (;;) {
        std::uint8_t h[16];
        const std::size_t got = src.read(h, 8);
        if (!signature) {
            // a JP2 file starts with its signature box: anything else is no JP2 file
            if (got == 0 || std::memcmp(h, kSignature, got) != 0) {
                return got == 0 ? need_data(msg) : fail(msg, NJ2K_E_NOT_JPEG2000, "Malformed JP2 file format: first box must be JPEG 2000 signature box\n");
            }
        }
        if (got != 8) {
            return need_data(msg);
        }
        std::uint64_t length = be32(h), header_size = 8;
        const std::uint32_t type = be32(h + 4);
        if (length == 1) {
            if (src.read(h + 8, 8) != 8) {
                return need_data(msg);
            }
            if (be32(h + 8) != 0) {
                if (!header) {
                    return fail(msg, NJ2K_E_HEADER, "Cannot handle box sizes higher than 2^32\n");
                }
                // OpenJPEG stops reading boxes here and, the JP2 header read, reads the codestream
                // from this point (jp2.c): a codestream box of 4 GiB or more is read that way
                msg.emit(Messages::Warning, "Cannot handle box sizes higher than 2^32\n");
                jp2->codestream = src.tell();
                jp2->codestream_length = 0;
                out = jp2;
                return NJ2K_OK;
            }
            length = be32(h + 12);
            header_size = 16;
        }
        if (type == kJP2C) {
            if (!header) {
                return fail(msg, NJ2K_E_HEADER, "bad placed jpeg codestream\n");
            }
            // OpenJPEG takes the codestream box before it looks at the box's length and reads the
            // codestream to the end of the stream (jp2.c), so a length that cannot even hold the
            // box's own header is read as 0, to the end
            jp2->codestream = src.tell();
            jp2->codestream_length = length >= header_size ? length - header_size : 0;
            out = jp2;
            return NJ2K_OK;
        }
        if (length == 0) {
            return fail(msg, NJ2K_E_HEADER, "Cannot handle box of undefined sizes\n");
        }
        if (length < header_size) {
            return fail(msg, NJ2K_E_HEADER, "invalid box size %llu (%x)\n", static_cast<unsigned long long>(length), type);
        }
        const std::uint64_t size = length - header_size;
        bool image_box = type == kIHDR || type == kCOLR || type == kBPCC || type == kPCLR || type == kCMAP || type == kCDEF;
        const bool handled = type == kJP || type == kFTYP || type == kJP2H || (image_box && header);
        if (image_box && !header) {
            msg.emit(Messages::Warning, "Found a misplaced '%c%c%c%c' box outside jp2h box\n", char(type >> 24),
                     char(type >> 16), char(type >> 8), char(type));
            msg.emit(Messages::Warning, "JPEG2000 Header box not read yet, '%c%c%c%c' box will be ignored\n",
                     char(type >> 24), char(type >> 16), char(type >> 8), char(type));
        } else if (image_box) {
            msg.emit(Messages::Warning, "Found a misplaced '%c%c%c%c' box outside jp2h box\n", char(type >> 24),
                     char(type >> 16), char(type >> 8), char(type));
        }
        if (!handled) {
            if (!signature) {
                return fail(msg, NJ2K_E_NOT_JPEG2000, "Malformed JP2 file format: first box must be JPEG 2000 signature box\n");
            }
            if (!file_type) {
                return fail(msg, NJ2K_E_HEADER, "Malformed JP2 file format: second box must be file type box\n");
            }
            if (!src.skip(size)) {
                return need_data(msg);
            }
            continue;
        }
        const std::int64_t left = src.bytes_left();
        if (left < 0 && size > 0) {
            // Without the stream's length a box's size is all there is to go by, and it may say up
            // to 4 GiB in a few bytes: no allocation on its word. OpenJPEG reads no box from such a
            // stream either (its bytes left are then 0: "do not even try to malloc if we can't
            // read", jp2.c), so a JP2 file needs the length (nj2k_stream_set_user_data_length).
            return fail(msg, NJ2K_E_UNSUPPORTED,
                        "Cannot read the JP2 boxes: the stream's length is not given (nj2k_stream_set_user_data_length)\n");
        }
        if (left >= 0 && std::uint64_t(left) < size) {
            src.skip(std::uint64_t(left));
            return need_data(msg);
        }
        std::uint8_t* data = nullptr;
        if (size) {
            data = static_cast<std::uint8_t*>(arena.allocate(std::size_t(size), 4));
            if (!data) {
                return arena_failure(msg, arena);
            }
            if (src.read(data, std::size_t(size)) != size) {
                return need_data(msg);
            }
        }
        const std::uint32_t n = std::uint32_t(size);
        nj2k_status_t st = NJ2K_OK;
        if (type == kJP) {
            if (signature || file_type || header) {
                st = fail(msg, NJ2K_E_HEADER, "The signature box must be the first box in the file.\n");
            } else if (n != 4) {
                st = fail(msg, NJ2K_E_HEADER, "Error with JP signature Box size\n");
            } else if (be32(data) != 0x0D0A870Au) {
                st = fail(msg, NJ2K_E_NOT_JPEG2000, "Error with JP Signature : bad magic number\n");
            }
            signature = true;
        } else if (type == kFTYP) {
            if (!signature || file_type || header) {
                st = fail(msg, NJ2K_E_HEADER, "The ftyp box must be the second box in the file.\n");
            } else if (n < 8 || (n - 8) % 4 != 0) {
                st = fail(msg, NJ2K_E_HEADER, "Error with FTYP signature Box size\n");
            }
            // the brand (jp2, jph) and compatibility list do not change how the codestream is read
            file_type = true;
        } else if (type == kJP2H) {
            if (!file_type) {
                st = fail(msg, NJ2K_E_HEADER, "The  box must be the first box in the file.\n");
            } else {
                st = rd.jp2h(data, n);
            }
            header = true;
        } else {
            bool known = false;
            st = rd.image_box(type, data, n, known);
        }
        if (st != NJ2K_OK) {
            return st;
        }
    }
}

NJ2K_COLOR_SPACE jp2_color_space(const Jp2Header& jp2) noexcept
{
    switch (jp2.enumcs) {
    case 16:
        return NJ2K_CLRSPC_SRGB;
    case 17:
        return NJ2K_CLRSPC_GRAY;
    case 18:
        return NJ2K_CLRSPC_SYCC;
    case 24:
        return NJ2K_CLRSPC_EYCC;
    case 12:
        return NJ2K_CLRSPC_CMYK;
    default:
        return NJ2K_CLRSPC_UNKNOWN;
    }
}

}  // namespace nj2k::detail

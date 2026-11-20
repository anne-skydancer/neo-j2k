// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The decoder's side of the API: codec creation with or without the host's allocator, message
// handlers, settings, limits, reading the header (from a stream or from memory), the reduce factor
// and policy, component selection, the decoded dimensions, the codestream information, and the
// decode (nj2k_decode into an image, nj2k_decode_memory into the caller's buffers; decode.h does
// the work). Every call that can fail records why (nj2k_get_status). The image and codestream
// information it fills, the decode's set-up and hand-over (opj_j2k_decode,
// opj_j2k_move_data_from_codec_to_output_image) and the messages derive from OpenJPEG's j2k.c and
// openjpeg.c, whose notice follows.
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
 * Copyright (c) 2008, Jerome Fimes, Communications & Systemes <jerome.fimes@c-s.fr>
 * Copyright (c) 2006-2007, Parvatha Elangovan
 * Copyright (c) 2010-2011, Kaori Hagihara
 * Copyright (c) 2011-2012, Centre National d'Etudes Spatiales (CNES), France
 * Copyright (c) 2012, CS Systemes d'Information, France
 * Copyright (c) 2017, IntoPIX SA <support@intopix.com>
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
#include "codec.h"

#include <cstdarg>
#include <cstring>
#include <new>

#include "decode.h"
#include "limit_profiles.h"

using nj2k::detail::Allocator;
using nj2k::detail::Decoder;
using nj2k::detail::Messages;
using nj2k::detail::Stream;

namespace {

NJ2K_BOOL finish(Decoder* d, nj2k_status_t status) noexcept
{
    d->status = status;
    return status >= 0 ? NJ2K_TRUE : NJ2K_FALSE;
}

NJ2K_BOOL fail(Decoder* d, nj2k_status_t status, const char* format, ...) noexcept NJ2K_PRINTF_LIKE(3, 4);

NJ2K_BOOL fail(Decoder* d, nj2k_status_t status, const char* format, ...) noexcept
{
    va_list args;
    va_start(args, format);
    d->messages.vemit(Messages::Error, format, args);
    va_end(args);
    return finish(d, status);
}

nj2k_codec_t* create(NJ2K_CODEC_FORMAT format, const Allocator& allocator) noexcept
{
    if (format != NJ2K_CODEC_J2K && format != NJ2K_CODEC_JP2) {
        return nullptr;
    }
    void* mem = allocator.allocate(sizeof(Decoder), alignof(Decoder) < 16 ? 16 : alignof(Decoder));
    if (!mem) {
        return nullptr;
    }
    auto* d = new (mem) Decoder();
    d->allocator = allocator;
    d->arena.init(&d->allocator);
    d->tile_arena.init(&d->allocator);
    d->work.init(&d->allocator);
    d->format = format;
    return reinterpret_cast<nj2k_codec_t*>(d);
}

// The component with the fewest resolutions in the main header.
std::uint32_t fewest_resolutions_component(const nj2k::detail::MainHeader& mh) noexcept
{
    for (std::uint32_t c = 0; c < mh.numcomps; ++c) {
        if (mh.main_style(c).numresolutions == mh.min_resolutions) {
            return c;
        }
    }
    return 0;
}

// The reduce factor in effect under the decoder's policy, or E_LIMIT with OpenJPEG's message for
// where the factor was given (before or after the header).
nj2k_status_t choose_factor(Decoder& d, std::uint32_t reduce, bool at_header, std::uint32_t& factor) noexcept
{
    const auto& mh = *d.header;
    if (reduce < mh.min_resolutions) {
        factor = reduce;
        return NJ2K_OK;
    }
    if (d.reduce_policy == NJ2K_REDUCE_CLAMP) {
        factor = mh.min_resolutions - 1;
        d.messages.emit(Messages::Warning,
                        "The reduce factor %u is lowered to %u, the coarsest level this codestream has\n", reduce,
                        factor);
        return NJ2K_OK;
    }
    if (at_header) {
        d.messages.emit(Messages::Error,
                        "Error decoding component %u.\nThe number of resolutions to remove (%u) is greater or equal "
                        "than the number of resolutions of this component (%u)\nModify the cp_reduce parameter.\n\n",
                        fewest_resolutions_component(mh), reduce, mh.min_resolutions);
    } else {
        d.messages.emit(Messages::Error,
                        "Resolution factor is greater than the maximum resolution in the component.\n");
    }
    return NJ2K_E_LIMIT;
}

// The working memory of a decode at the factor in effect (decode_working_memory): kept with the
// header for the next decode at the same factor when every component is decoded.
std::uint64_t working_memory(Decoder& d) noexcept
{
    if (!d.selected_mask && d.estimate_factor == d.factor) {
        return d.estimate;
    }
    const std::uint64_t need = nj2k::detail::decode_working_memory(*d.header, d.factor, d.selected_mask);
    if (!d.selected_mask) {
        d.estimate = need;
        d.estimate_factor = d.factor;
    }
    return need;
}

// The image nj2k_read_header returns, as OpenJPEG's opj_read_header fills it.
nj2k_image_t* header_image(const Decoder& d) noexcept
{
    using namespace nj2k::detail;
    const MainHeader& mh = *d.header;
    auto* img = static_cast<nj2k_image_t*>(heap_calloc(1, sizeof(nj2k_image_t)));
    if (!img) {
        return nullptr;
    }
    img->x0 = mh.x0;
    img->y0 = mh.y0;
    img->x1 = mh.x1;
    img->y1 = mh.y1;
    img->numcomps = mh.numcomps;
    img->color_space = d.jp2 ? jp2_color_space(*d.jp2) : NJ2K_CLRSPC_UNSPECIFIED;
    img->comps = static_cast<nj2k_image_comp_t*>(heap_calloc(mh.numcomps, sizeof(nj2k_image_comp_t)));
    if (!img->comps) {
        nj2k_image_destroy(img);
        return nullptr;
    }
    for (std::uint32_t c = 0; c < mh.numcomps; ++c) {
        nj2k_image_comp_t& ic = img->comps[c];
        ic.dx = mh.comps[c].dx;
        ic.dy = mh.comps[c].dy;
        ic.prec = mh.comps[c].prec;
        ic.sgnd = mh.comps[c].sgnd;
        ic.factor = d.factor;
        header_component_geometry(mh, c, d.factor, ic.x0, ic.y0, ic.w, ic.h);
    }
    if (d.jp2 && d.jp2->icc_len) {
        img->icc_profile_buf = static_cast<NJ2K_BYTE*>(heap_calloc(1, d.jp2->icc_len));
        if (!img->icc_profile_buf) {
            nj2k_image_destroy(img);
            return nullptr;
        }
        std::memcpy(img->icc_profile_buf, d.jp2->icc, d.jp2->icc_len);
        img->icc_profile_len = d.jp2->icc_len;
    } else if (d.jp2 && d.jp2->cielab) {
        // OpenJPEG hands CIELab's parameters over where the ICC profile goes, with length 0
        img->icc_profile_buf = static_cast<NJ2K_BYTE*>(heap_calloc(1, sizeof(d.jp2->cielab_values)));
        if (!img->icc_profile_buf) {
            nj2k_image_destroy(img);
            return nullptr;
        }
        std::memcpy(img->icc_profile_buf, d.jp2->cielab_values, sizeof(d.jp2->cielab_values));
        img->icc_profile_len = 0;
    }
    return img;
}

// The JP2 boxes (a JP2 codec) and the main header from src into the header arena, then the reduce
// factor in effect.
nj2k_status_t read_header_from(Decoder& d, nj2k::detail::Source& src) noexcept
{
    using namespace nj2k::detail;
    d.arena.set_budget(d.limits.max_memory);
    HeaderSettings hs{};
    hs.limits = d.limits;
    hs.dump = d.dump;
    nj2k_status_t st = NJ2K_OK;
    d.jp2 = nullptr;
    d.header = nullptr;
    d.estimate_factor = UINT32_MAX;
    if (d.format == NJ2K_CODEC_JP2) {
        st = read_jp2_boxes(src, d.arena, d.messages, d.jp2);
        if (st == NJ2K_OK) {
            hs.ihdr_w = d.jp2->w;
            hs.ihdr_h = d.jp2->h;
            // OpenJPEG's J2K decoder takes components of any depth (j2k.c); a JP2 file whose image
            // header gives one depth for all (BPC != 255) has differing ones warned about (jp2.c)
            hs.allow_varying_depth = d.jp2->bpc == 255;
        }
    }
    if (st == NJ2K_OK) {
        st = read_main_header(src, d.arena, d.messages, hs, d.header);
    }
    if (st == NJ2K_OK) {
        st = choose_factor(d, d.reduce, true, d.factor);
    }
    return st;
}

// The components of the image a header gives, at the factor in effect, as OpenJPEG's
// opj_read_header gives them (header_image): into the codec's own array
void fill_image_comps(Decoder& d) noexcept
{
    using namespace nj2k::detail;
    const MainHeader& mh = *d.header;
    for (std::uint32_t c = 0; c < mh.numcomps; ++c) {
        nj2k_image_comp_t& ic = d.image_comps[c];
        ic = nj2k_image_comp_t{};
        ic.dx = mh.comps[c].dx;
        ic.dy = mh.comps[c].dy;
        ic.prec = mh.comps[c].prec;
        ic.sgnd = mh.comps[c].sgnd;
        ic.factor = d.factor;
        header_component_geometry(mh, c, d.factor, ic.x0, ic.y0, ic.w, ic.h);
    }
}

// A header from memory: the one the codec holds when the bytes start with the same main header
// (and the codec's limits are those it was read under), else a new one. The codec then starts a
// new codestream: the reduce factor in effect from the one asked. A component selection
// (nj2k_set_decoded_components() after the header read) holds for the decodes of the header held
// (keep_selection: nj2k_decode_memory()); nj2k_read_header_memory() starts without one, and so does
// a new header.
nj2k_status_t memory_header(Decoder& d, const std::uint8_t* data, std::size_t size, bool keep_selection) noexcept
{
    using namespace nj2k::detail;
    const bool held = d.header && d.header_from_memory && d.header_bytes &&
                      (d.state == Decoder::State::header || d.state == Decoder::State::decoded) &&
                      size >= d.header_bytes_size && std::memcmp(data, d.header_bytes, d.header_bytes_size) == 0 &&
                      std::memcmp(&d.header_limits, &d.limits, sizeof d.limits) == 0;
    if (held) {
        if (!keep_selection) {
            // the selection's arrays stay, for the next selection of this header
            d.selected = nullptr;
            d.selected_mask = nullptr;
            d.nselected = 0;
        }
        if (const nj2k_status_t st = choose_factor(d, d.reduce, true, d.factor); st != NJ2K_OK) {
            d.state = Decoder::State::failed;
            return st;
        }
        fill_image_comps(d);
        d.state = Decoder::State::header;
        return NJ2K_OK;
    }
    d.arena.reset(d.retain);
    d.header = nullptr;
    d.jp2 = nullptr;
    d.selected = nullptr;
    d.selected_mask = nullptr;
    d.nselected = 0;
    d.selection_mask = nullptr;
    d.selection_order = nullptr;
    d.header_bytes = nullptr;
    d.header_bytes_size = 0;
    d.image_comps = nullptr;
    d.header_from_memory = true;
    Source src(data, size);
    nj2k_status_t st = read_header_from(d, src);
    if (st == NJ2K_OK) {
        d.image_comps = d.arena.make_array<nj2k_image_comp_t>(d.header->numcomps);
        if (!d.image_comps) {
            d.messages.emit(Messages::Error, "Not enough memory for the image header\n");
            st = d.arena.failure() == Arena::Failure::budget ? NJ2K_E_LIMIT : NJ2K_E_NO_MEMORY;
        }
    }
    if (st != NJ2K_OK) {
        d.header = nullptr;
        d.state = Decoder::State::failed;
        return st;
    }
    // the bytes up to the first tile-part, kept to know the same header again (none kept when the
    // budget refuses them: the header is then read every time)
    const std::size_t n = static_cast<std::size_t>(d.header->end + 2);
    d.header_bytes = d.arena.copy_bytes(data, n);
    d.header_bytes_size = d.header_bytes ? n : 0;
    d.header_limits = d.limits;
    fill_image_comps(d);
    d.state = Decoder::State::header;
    return NJ2K_OK;
}

// The image a memory header gives, its components the codec's own
void describe_image(const Decoder& d, nj2k_image_t& image) noexcept
{
    using namespace nj2k::detail;
    const MainHeader& mh = *d.header;
    image = nj2k_image_t{};
    image.x0 = mh.x0;
    image.y0 = mh.y0;
    image.x1 = mh.x1;
    image.y1 = mh.y1;
    image.numcomps = mh.numcomps;
    image.color_space = d.jp2 ? jp2_color_space(*d.jp2) : NJ2K_CLRSPC_UNSPECIFIED;
    image.comps = d.image_comps;
    if (d.jp2 && d.jp2->icc_len) {
        image.icc_profile_buf = const_cast<NJ2K_BYTE*>(d.jp2->icc);
        image.icc_profile_len = d.jp2->icc_len;
    } else if (d.jp2 && d.jp2->cielab) {
        image.icc_profile_buf = reinterpret_cast<NJ2K_BYTE*>(const_cast<std::uint32_t*>(d.jp2->cielab_values));
        image.icc_profile_len = 0;
    }
}

NJ2K_BOOL set_handler(nj2k_codec_t* p_codec, Messages::Kind kind, nj2k_msg_callback callback, void* user) noexcept
{
    Decoder* d = Decoder::from(p_codec);
    if (!d) {
        return NJ2K_FALSE;
    }
    d->messages.set(kind, callback, user);
    return NJ2K_TRUE;
}

NJ2K_BOOL not_yet(nj2k_codec_t* p_codec, const char* what) noexcept
{
    Decoder* d = Decoder::from(p_codec);
    if (!d) {
        return NJ2K_FALSE;
    }
    return fail(d, NJ2K_E_UNSUPPORTED,
                "%s: not supported yet (decoding by tile and decode areas come in a later stage)\n", what);
}

}  // namespace

namespace nj2k::detail {

std::size_t Decoder::kept_bytes() const noexcept
{
    return std::size_t(work.reserved() + tile_arena.reserved()) + t1.bytes() + planes.size + dwt.size + input.size;
}

void Decoder::trim_after_decode() noexcept
{
    // Both arenas are merged into one block here, at the end of the decode, not when the next one
    // starts: a decode that grew either (a tile-part header with more markers than any before it, a
    // larger layout) then leaves memory the same decode fits in, so even the first repeat of a request
    // makes no heap call. Merging at the next start would free the blocks and take the merged one
    // in that repeat instead (one allocation, two frees).
    work.reset(retain);
    tile_arena.reset(retain);
    if (kept_bytes() <= retain) {
        return;
    }
    // the largest first: a stream decode's bytes, a tile's samples, the wavelet's lines
    input.release(allocator);
    if (kept_bytes() > retain) {
        planes.release(allocator);
    }
    if (kept_bytes() > retain) {
        dwt.release(allocator);
    }
    if (kept_bytes() > retain) {
        t1.release(allocator);
    }
    if (kept_bytes() > retain) {
        work.release_all();
        tile_arena.release_all();
    }
}

void Decoder::release_all() noexcept
{
    arena.release_all();
    tile_arena.release_all();
    work.release_all();
    t1.release(allocator);
    planes.release(allocator);
    dwt.release(allocator);
    input.release(allocator);
}

}  // namespace nj2k::detail

extern "C" {

NJ2K_API nj2k_codec_t* NJ2K_CALLCONV nj2k_create_decompress(NJ2K_CODEC_FORMAT format)
{
    return create(format, Allocator::system());
}

NJ2K_API nj2k_codec_t* NJ2K_CALLCONV nj2k_create_decompress_with_allocator(NJ2K_CODEC_FORMAT format,
                                                                          const nj2k_allocator_t* allocator)
{
    Allocator a;
    if (!Allocator::from_host(allocator, a)) {
        return nullptr;
    }
    return create(format, a);
}

NJ2K_API void NJ2K_CALLCONV nj2k_destroy_codec(nj2k_codec_t* p_codec)
{
    Decoder* d = Decoder::from(p_codec);
    if (!d) {
        return;
    }
    const Allocator allocator = d->allocator;
    d->release_all();
    d->magic = 0;
    d->~Decoder();
    allocator.release(d, sizeof(Decoder));
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_set_info_handler(nj2k_codec_t* p_codec, nj2k_msg_callback p_callback,
                                                      void* p_user_data)
{
    return set_handler(p_codec, Messages::Info, p_callback, p_user_data);
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_set_warning_handler(nj2k_codec_t* p_codec, nj2k_msg_callback p_callback,
                                                         void* p_user_data)
{
    return set_handler(p_codec, Messages::Warning, p_callback, p_user_data);
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_set_error_handler(nj2k_codec_t* p_codec, nj2k_msg_callback p_callback,
                                                       void* p_user_data)
{
    return set_handler(p_codec, Messages::Error, p_callback, p_user_data);
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_setup_decoder(nj2k_codec_t* p_codec, nj2k_dparameters_t* parameters)
{
    Decoder* d = Decoder::from(p_codec);
    if (!d) {
        return NJ2K_FALSE;
    }
    if (!parameters) {
        return fail(d, NJ2K_E_ARGUMENT, "nj2k_setup_decoder: no parameters\n");
    }
    if (d->state != Decoder::State::created) {
        return fail(d, NJ2K_E_STATE, "nj2k_setup_decoder: call it before nj2k_read_header\n");
    }
    d->reduce = parameters->cp_reduce;
    d->layers = parameters->cp_layer;
    d->dump = (parameters->flags & NJ2K_DPARAMETERS_DUMP_FLAG) != 0;
    d->ignore_pclr = (parameters->flags & NJ2K_DPARAMETERS_IGNORE_PCLR_CMAP_CDEF_FLAG) != 0;
    return finish(d, NJ2K_OK);
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_decoder_set_strict_mode(nj2k_codec_t* p_codec, NJ2K_BOOL strict)
{
    Decoder* d = Decoder::from(p_codec);
    if (!d) {
        return NJ2K_FALSE;
    }
    d->strict = strict != 0;
    if (d->strict) {
        d->tnsot_check_off = true;  // as opj_j2k_decoder_set_strict_mode
    }
    return finish(d, NJ2K_OK);
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_decoder_set_limits(nj2k_codec_t* p_codec, const nj2k_decoder_limits_t* p_limits)
{
    Decoder* d = Decoder::from(p_codec);
    if (!d) {
        return NJ2K_FALSE;
    }
    if (!p_limits) {
        return fail(d, NJ2K_E_ARGUMENT, "nj2k_decoder_set_limits: no limits\n");
    }
    if (d->state != Decoder::State::created && !d->header_from_memory) {
        // the header's limits were checked (or skipped) when it was read: as the fork's 1a
        // (6bad83c0), a later call changes nothing. A codec that reads from memory starts a new
        // codestream at its next nj2k_read_header_memory() or nj2k_decode_memory(), which reads the
        // header again under the new limits
        return fail(d, NJ2K_E_STATE, "nj2k_decoder_set_limits must be called before nj2k_read_header.\n");
    }
    d->limits = *p_limits;
    return finish(d, NJ2K_OK);
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_decoder_set_reduce_policy(nj2k_codec_t* p_codec, NJ2K_UINT32 policy)
{
    Decoder* d = Decoder::from(p_codec);
    if (!d) {
        return NJ2K_FALSE;
    }
    if (policy != NJ2K_REDUCE_EXACT && policy != NJ2K_REDUCE_CLAMP) {
        return fail(d, NJ2K_E_ARGUMENT, "nj2k_decoder_set_reduce_policy: unknown policy %u\n", policy);
    }
    d->reduce_policy = policy;
    return finish(d, NJ2K_OK);
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_read_header(nj2k_stream_t* p_stream, nj2k_codec_t* p_codec,
                                                 nj2k_image_t** p_image)
{
    using namespace nj2k::detail;
    Decoder* d = Decoder::from(p_codec);
    if (!d) {
        return NJ2K_FALSE;
    }
    Stream* s = Stream::from(p_stream);
    if (!s || !s->input || !p_image) {
        return fail(d, NJ2K_E_ARGUMENT, "nj2k_read_header: no input stream or no image pointer\n");
    }
    if (d->state != Decoder::State::created) {
        return fail(d, NJ2K_E_STATE, "nj2k_read_header: the codec has read a header already, or failed\n");
    }
    Source src(*s);
    nj2k_status_t st = read_header_from(*d, src);
    nj2k_image_t* img = nullptr;
    if (st == NJ2K_OK) {
        img = header_image(*d);
        if (!img) {
            d->messages.emit(Messages::Error, "Not enough memory for the image header\n");
            st = NJ2K_E_NO_MEMORY;
        }
    }
    if (st != NJ2K_OK) {
        d->state = Decoder::State::failed;
        return finish(d, st);
    }
    d->state = Decoder::State::header;
    *p_image = img;
    return finish(d, NJ2K_OK);
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_set_decoded_resolution_factor(nj2k_codec_t* p_codec, NJ2K_UINT32 res_factor)
{
    Decoder* d = Decoder::from(p_codec);
    if (!d) {
        return NJ2K_FALSE;
    }
    if (d->state == Decoder::State::failed && !d->header_from_memory) {
        return fail(d, NJ2K_E_STATE, "nj2k_set_decoded_resolution_factor: the codec failed\n");
    }
    if (d->state == Decoder::State::created || d->state == Decoder::State::failed ||
        (d->state == Decoder::State::decoded && !d->header_from_memory)) {
        // kept, and checked when the header is read (a codec decoding from memory reads its next
        // header afresh after a failure); OpenJPEG returns false here too
        d->reduce = res_factor;
        return finish(d, NJ2K_E_STATE);
    }
    std::uint32_t factor = 0;
    if (const nj2k_status_t st = choose_factor(*d, res_factor, false, factor); st != NJ2K_OK) {
        return finish(d, st);
    }
    d->reduce = res_factor;
    d->factor = factor;
    return finish(d, NJ2K_OK);
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_set_decoded_components(nj2k_codec_t* p_codec, NJ2K_UINT32 numcomps,
                                                            const NJ2K_UINT32* comps_indices,
                                                            NJ2K_BOOL apply_color_transforms)
{
    Decoder* d = Decoder::from(p_codec);
    if (!d) {
        return NJ2K_FALSE;
    }
    if (apply_color_transforms) {
        return fail(d, NJ2K_E_ARGUMENT, "apply_color_transforms = OPJ_TRUE is not supported.\n");
    }
    if (d->state != Decoder::State::header) {
        return fail(d, NJ2K_E_STATE, "opj_read_header() should be called before opj_set_decoded_components().\n");
    }
    if (numcomps && !comps_indices) {
        return fail(d, NJ2K_E_ARGUMENT, "nj2k_set_decoded_components: no component indices\n");
    }
    const std::uint32_t total = d->header->numcomps;
    if (numcomps == 0) {
        d->selected = nullptr;
        d->selected_mask = nullptr;
        d->nselected = 0;
        return finish(d, NJ2K_OK);
    }
    if (numcomps > total) {
        return fail(d, NJ2K_E_ARGUMENT, "nj2k_set_decoded_components: %u components, the image has %u\n", numcomps, total);
    }
    if (!d->selection_mask) {
        bool* mask = d->arena.make_array<bool>(total);
        auto* order = d->arena.make_array<std::uint32_t>(total);
        if (!mask || !order) {
            return fail(d, d->arena.failure() == nj2k::detail::Arena::Failure::budget ? NJ2K_E_LIMIT : NJ2K_E_NO_MEMORY,
                        "Not enough memory to select the components\n");
        }
        d->selection_mask = mask;
        d->selection_order = order;
    }
    bool* mask = d->selection_mask;
    std::memset(mask, 0, total);
    for (std::uint32_t i = 0; i < numcomps; ++i) {
        const std::uint32_t c = comps_indices[i];
        if (c >= total || mask[c]) {
            // a refused selection leaves the earlier one in place, as OpenJPEG does
            std::memset(mask, 0, total);
            for (std::uint32_t j = 0; j < d->nselected; ++j) {
                mask[d->selected[j]] = true;
            }
            if (c >= total) {
                return fail(d, NJ2K_E_ARGUMENT, "Invalid component index: %u\n", c);
            }
            return fail(d, NJ2K_E_ARGUMENT, "Component index %u used several times\n", c);
        }
        mask[c] = true;
    }
    std::memcpy(d->selection_order, comps_indices, std::size_t(numcomps) * sizeof(std::uint32_t));
    d->selected = d->selection_order;
    d->selected_mask = mask;
    d->nselected = numcomps;
    return finish(d, NJ2K_OK);
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_get_decoded_dimensions(nj2k_codec_t* p_codec, nj2k_image_t* p_image)
{
    using namespace nj2k::detail;
    Decoder* d = Decoder::from(p_codec);
    if (!d) {
        return NJ2K_FALSE;
    }
    if (d->state != Decoder::State::header) {
        return fail(d, NJ2K_E_STATE, "nj2k_get_decoded_dimensions: call it after nj2k_read_header\n");
    }
    const MainHeader& mh = *d->header;
    if (!p_image || !p_image->comps || p_image->numcomps != mh.numcomps) {
        return fail(d, NJ2K_E_ARGUMENT, "nj2k_get_decoded_dimensions: not the image nj2k_read_header gave\n");
    }
    // OpenJPEG's decode: a factor set after the header resizes an image the header gave with factor 0
    const bool resize = d->factor > 0 && p_image->comps[0].factor == 0 && p_image->comps[0].data == nullptr;
    const std::uint32_t side = d->limits.max_decoded_side;
    for (std::uint32_t c = 0; c < mh.numcomps; ++c) {
        std::uint32_t x0 = p_image->comps[c].x0, y0 = p_image->comps[c].y0;
        std::uint32_t w = p_image->comps[c].w, h = p_image->comps[c].h;
        if (resize && !decoded_component_geometry(mh, c, d->factor, x0, y0, w, h)) {
            return fail(d, NJ2K_E_UNSUPPORTED, "Image coordinates above INT_MAX are not supported\n");
        }
        if (side != 0 && (w > side || h > side)) {
            return fail(d, NJ2K_E_LIMIT,
                        "Component %u is %u x %u at the decoded resolution, above the decoder's limit of %u\n", c, w, h,
                        side);
        }
    }
    if (d->limits.max_memory != 0) {
        const std::uint64_t need = working_memory(*d);
        if (need > d->limits.max_memory) {
            return fail(d, NJ2K_E_LIMIT,
                        "The decode needs %llu bytes of working memory, above the decoder's limit of %llu\n",
                        static_cast<unsigned long long>(need), static_cast<unsigned long long>(d->limits.max_memory));
        }
    }
    if (resize) {
        for (std::uint32_t c = 0; c < mh.numcomps; ++c) {
            nj2k_image_comp_t& ic = p_image->comps[c];
            ic.factor = d->factor;
            decoded_component_geometry(mh, c, d->factor, ic.x0, ic.y0, ic.w, ic.h);
        }
    }
    return finish(d, NJ2K_OK);
}

NJ2K_API nj2k_codestream_info_v2_t* NJ2K_CALLCONV nj2k_get_cstr_info(nj2k_codec_t* p_codec)
{
    using namespace nj2k::detail;
    Decoder* d = Decoder::from(p_codec);
    if (!d) {
        return nullptr;
    }
    if (d->state != Decoder::State::header) {
        fail(d, NJ2K_E_STATE, "nj2k_get_cstr_info: call it after nj2k_read_header\n");
        return nullptr;
    }
    const MainHeader& mh = *d->header;
    auto* info = static_cast<nj2k_codestream_info_v2_t*>(heap_calloc(1, sizeof(nj2k_codestream_info_v2_t)));
    auto* tccp = static_cast<nj2k_tccp_info_t*>(heap_calloc(mh.numcomps, sizeof(nj2k_tccp_info_t)));
    if (!info || !tccp) {
        heap_free(info);
        heap_free(tccp);
        fail(d, NJ2K_E_NO_MEMORY, "Not enough memory for the codestream information\n");
        return nullptr;
    }
    info->nbcomps = mh.numcomps;
    info->tx0 = mh.tx0;
    info->ty0 = mh.ty0;
    info->tdx = mh.tdx;
    info->tdy = mh.tdy;
    info->tw = mh.tw;
    info->th = mh.th;
    info->tile_info = nullptr;  // as OpenJPEG: not filled from the main header
    info->m_default_tile_info.csty = mh.cod.csty;
    info->m_default_tile_info.prg = static_cast<NJ2K_PROG_ORDER>(mh.cod.prg);
    info->m_default_tile_info.numlayers = mh.cod.numlayers;
    info->m_default_tile_info.mct = mh.cod.mct;
    info->m_default_tile_info.tccp_info = tccp;
    for (std::uint32_t c = 0; c < mh.numcomps; ++c) {
        const CodingStyle& s = mh.main_style(c);
        const Quantization& q = mh.main_quant(c);
        nj2k_tccp_info_t& t = tccp[c];
        t.csty = s.csty;
        t.numresolutions = s.numresolutions;
        t.cblkw = s.cblkw;
        t.cblkh = s.cblkh;
        t.cblksty = s.cblksty;
        t.qmfbid = s.qmfbid;
        // every resolution's precinct size (OpenJPEG copies only the first bytes: doc/departures.md)
        if (s.numresolutions < kMaxResolutions) {
            for (std::uint32_t r = 0; r < s.numresolutions; ++r) {
                t.prcw[r] = s.precincts[r] & 0xFu;
                t.prch[r] = s.precincts[r] >> 4;
            }
        }
        t.qntsty = q.qntsty;
        t.numgbits = q.numgbits;
        const std::uint32_t bands = q.qntsty == kQntDerived ? 1 : 3u * s.numresolutions - 2;
        if (bands < kMaxBands) {
            for (std::uint32_t b = 0; b < bands; ++b) {
                q.step(b, t.stepsizes_expn[b], t.stepsizes_mant[b]);
            }
        }
        t.roishift = mh.main_roishift(c);
    }
    finish(d, NJ2K_OK);
    return info;
}

NJ2K_API void NJ2K_CALLCONV nj2k_destroy_cstr_info(nj2k_codestream_info_v2_t** cstr_info)
{
    if (!cstr_info || !*cstr_info) {
        return;
    }
    nj2k::detail::heap_free((*cstr_info)->m_default_tile_info.tccp_info);
    nj2k::detail::heap_free((*cstr_info)->tile_info);
    nj2k::detail::heap_free(*cstr_info);
    *cstr_info = nullptr;
}

NJ2K_API nj2k_status_t NJ2K_CALLCONV nj2k_get_status(nj2k_codec_t* p_codec)
{
    const Decoder* d = Decoder::from(p_codec);
    return d ? d->status : NJ2K_E_ARGUMENT;
}

NJ2K_API const char* NJ2K_CALLCONV nj2k_status_string(nj2k_status_t status)
{
    switch (status) {
    case NJ2K_OK:
        return "done";
    case NJ2K_TRUNCATED:
        return "decoded from the bytes given; the codestream goes on";
    case NJ2K_E_NEED_DATA:
        return "need more data";
    case NJ2K_E_ARGUMENT:
        return "invalid argument";
    case NJ2K_E_NOT_JPEG2000:
        return "not a JPEG 2000 codestream or file";
    case NJ2K_E_HEADER:
        return "invalid header";
    case NJ2K_E_UNSUPPORTED:
        return "not supported";
    case NJ2K_E_LIMIT:
        return "refused by a decoder limit";
    case NJ2K_E_NO_MEMORY:
        return "out of memory";
    case NJ2K_E_DATA:
        return "damaged data";
    case NJ2K_E_OUTPUT:
        return "the output does not fit";
    case NJ2K_E_STATE:
        return "call out of order";
    default:
        return status >= 0 ? "success" : "failure";
    }
}

// ---- decoding --------------------------------------------------------------------------------------

namespace {

using nj2k::detail::CodestreamBytes;
using nj2k::detail::OutComp;
using nj2k::detail::OutSink;

// opj_j2k_decode's resize of an image the header gave at factor 0 when the factor came after the
// header (opj_j2k_update_image_dimensions); false (an error given) above INT_MAX
bool apply_decode_rule(Decoder& d, nj2k_image_comp_t* comps) noexcept
{
    using namespace nj2k::detail;
    if (!(d.factor > 0 && comps[0].factor == 0 && comps[0].data == nullptr)) {
        return true;
    }
    const MainHeader& mh = *d.header;
    for (std::uint32_t c = 0; c < mh.numcomps; ++c) {
        comps[c].factor = d.factor;
        if (!decoded_component_geometry(mh, c, d.factor, comps[c].x0, comps[c].y0, comps[c].w, comps[c].h)) {
            d.messages.emit(Messages::Error, "Image coordinates above INT_MAX are not supported\n");
            return false;
        }
    }
    return true;
}

// opj_j2k_start_limited_decode, and the working memory the header model computes: checked before
// anything of the decode is allocated
nj2k_status_t check_decode_limits(Decoder& d, const nj2k_image_comp_t* comps) noexcept
{
    using namespace nj2k::detail;
    const MainHeader& mh = *d.header;
    const std::uint32_t side = d.limits.max_decoded_side;
    for (std::uint32_t c = 0; side && c < mh.numcomps; ++c) {
        if (comps[c].w > side || comps[c].h > side) {
            d.messages.emit(Messages::Error,
                            "Component %u decodes to %u x %u samples, more than the decoder's limit of %u per side\n", c,
                            comps[c].w, comps[c].h, side);
            return NJ2K_E_LIMIT;
        }
    }
    if (d.limits.max_memory) {
        const std::uint64_t need = working_memory(d);
        if (need > d.limits.max_memory) {
            d.messages.emit(Messages::Error,
                            "The decode needs %llu bytes of working memory, above the decoder's limit of %llu\n",
                            static_cast<unsigned long long>(need), static_cast<unsigned long long>(d.limits.max_memory));
            return NJ2K_E_LIMIT;
        }
    }
    return NJ2K_OK;
}

// Where a JP2 file's codestream ends, as an offset in its bytes, when the codestream box gives its
// length (ISO 15444-1 I.5.4: the codestream is the box's contents, DP-15); 0 when that is not known
// (a codestream, or a box that runs to the end of the file).
std::uint64_t codestream_end(const Decoder& d) noexcept
{
    if (!d.jp2 || d.jp2->codestream_length == 0 || d.jp2->codestream_length > UINT64_MAX - d.jp2->codestream) {
        return 0;
    }
    return d.jp2->codestream + d.jp2->codestream_length;
}

// The rest of a stream, into the decoder's input buffer: up to the codestream's end when it is
// known (end, a stream offset; 0 when not), the length the host gave, or the stream's end, whichever
// comes first. When the host gave the stream's length the buffer is taken once at the size to
// read; otherwise it grows by doubling. These bytes are the decode's input, outside the decoder
// limits' working memory (doc/api.md).
bool read_rest(Decoder& d, nj2k::detail::Stream& s, std::uint64_t end, std::uint64_t& got) noexcept
{
    got = 0;
    std::uint64_t cap = s.length ? (s.length > s.offset ? s.length - s.offset : 0) : UINT64_MAX;
    if (end) {
        const std::uint64_t left = end > s.offset ? end - s.offset : 0;
        cap = left < cap ? left : cap;
    }
    if (s.length && cap > d.input.size) {
        // the size is the host's, not the codestream's: one buffer, no copies while it fills
        if (cap > SIZE_MAX / 2 || !d.input.ensure(d.allocator, std::size_t(cap))) {
            return false;
        }
    }
    for (;;) {
        if (got >= cap) {
            return true;
        }
        if (got >= d.input.size) {
            // grow, keeping what was read
            std::size_t bigger = d.input.size ? d.input.size * 2 : std::size_t(64) * 1024;
            if (cap != UINT64_MAX && bigger > cap) {
                bigger = std::size_t(cap);
            }
            void* p = d.allocator.allocate(bigger, 64);
            if (!p) {
                return false;
            }
            if (got) {
                std::memcpy(p, d.input.p, std::size_t(got));
            }
            d.input.release(d.allocator);
            d.input.p = p;
            d.input.size = bigger;
        }
        const std::uint64_t room = d.input.size - got;
        const std::size_t n = std::size_t(room < cap - got ? room : cap - got);
        const std::size_t r = s.read(static_cast<std::uint8_t*>(d.input.p) + got, n);
        got += r;
        if (r < n) {
            return true;
        }
    }
}

// opj_jp2_apply_color_postprocessing on the image of a JP2 decode
nj2k_status_t jp2_colour(Decoder& d, nj2k_image_t* image) noexcept
{
    using namespace nj2k::detail;
    if (!d.jp2 || d.nselected || d.ignore_pclr) {
        return NJ2K_OK;
    }
    return jp2_apply_colour(*d.jp2, image, d.limits, d.messages);
}

}  // namespace

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_set_decode_area(nj2k_codec_t* p_codec, nj2k_image_t*, NJ2K_INT32, NJ2K_INT32,
                                                     NJ2K_INT32, NJ2K_INT32)
{
    return not_yet(p_codec, "nj2k_set_decode_area");
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_decode(nj2k_codec_t* p_codec, nj2k_stream_t* p_stream, nj2k_image_t* p_image)
{
    using namespace nj2k::detail;
    Decoder* d = Decoder::from(p_codec);
    if (!d) {
        return NJ2K_FALSE;
    }
    Stream* s = Stream::from(p_stream);
    if (!s || !s->input || !p_image) {
        return fail(d, NJ2K_E_ARGUMENT, "nj2k_decode: no input stream or no image\n");
    }
    if (d->state != Decoder::State::header || !d->header || d->header_from_memory) {
        return fail(d, NJ2K_E_STATE, "nj2k_decode: call it once, after nj2k_read_header\n");
    }
    const MainHeader& mh = *d->header;
    if (p_image->numcomps != mh.numcomps || !p_image->comps) {
        return fail(d, NJ2K_E_ARGUMENT, "nj2k_decode: not the image nj2k_read_header gave\n");
    }
    if (!apply_decode_rule(*d, p_image->comps)) {
        d->state = Decoder::State::failed;
        return finish(d, NJ2K_E_UNSUPPORTED);
    }
    if (const nj2k_status_t st = check_decode_limits(*d, p_image->comps); st != NJ2K_OK) {
        d->state = Decoder::State::failed;
        return finish(d, st);
    }
    d->work.reset(d->retain);
    auto* comps = d->work.make_array<OutComp>(mh.numcomps);
    std::uint64_t got = 0;
    const std::uint64_t origin = s->offset;
    const std::uint64_t end = codestream_end(*d);
    if (!comps || !read_rest(*d, *s, end, got)) {
        d->messages.emit(Messages::Error, "Not enough memory to decode\n");
        d->state = Decoder::State::failed;
        d->trim_after_decode();
        return finish(d, NJ2K_E_NO_MEMORY);
    }
    for (std::uint32_t c = 0; c < mh.numcomps; ++c) {
        const nj2k_image_comp_t& ic = p_image->comps[c];
        comps[c] = OutComp{ic.dx, ic.dy, ic.w, ic.h, ic.x0, ic.y0, ic.prec, ic.sgnd, ic.factor, ic.resno_decoded, nullptr,
                           false};
    }
    // the length the host gave, or the codestream box's end when that comes first (DP-15)
    const std::uint64_t claimed = end && s->length > end ? end : s->length;
    const CodestreamBytes bytes{static_cast<const std::uint8_t*>(d->input.p), origin, origin + got, claimed, origin,
                                s->seek_fn != nullptr};
    const OutSink sink{true, nullptr};
    bool truncated = false;
    nj2k_status_t st = decode_codestream(*d, bytes, comps, sink, truncated);
    if (st == NJ2K_OK) {
        // opj_j2k_move_data_from_codec_to_output_image
        if (d->nselected) {
            auto* newcomps = static_cast<nj2k_image_comp_t*>(heap_calloc(d->nselected, sizeof(nj2k_image_comp_t)));
            if (!newcomps) {
                d->messages.emit(Messages::Error, "Not enough memory for the image\n");
                st = NJ2K_E_NO_MEMORY;
            } else {
                for (std::uint32_t c = 0; c < p_image->numcomps; ++c) {
                    plane_free(p_image->comps[c].data);
                    p_image->comps[c].data = nullptr;
                }
                for (std::uint32_t i = 0; i < d->nselected; ++i) {
                    const std::uint32_t src = d->selected[i];
                    newcomps[i] = p_image->comps[src];
                    newcomps[i].resno_decoded = comps[src].resno_decoded;
                    newcomps[i].data = comps[src].data;
                    comps[src].data = nullptr;
                }
                heap_free(p_image->comps);
                p_image->comps = newcomps;
                p_image->numcomps = d->nselected;
            }
        } else {
            for (std::uint32_t c = 0; c < mh.numcomps; ++c) {
                p_image->comps[c].resno_decoded = comps[c].resno_decoded;
                plane_free(p_image->comps[c].data);
                p_image->comps[c].data = comps[c].data;
                comps[c].data = nullptr;
            }
        }
    }
    if (st == NJ2K_OK) {
        st = jp2_colour(*d, p_image);
        if (st != NJ2K_OK) {
            d->messages.emit(Messages::Error, "Failed to decode the codestream in the JP2 file\n");
        }
    }
    for (std::uint32_t c = 0; c < mh.numcomps; ++c) {
        plane_free(comps[c].data);  // what a failed decode made
    }
    d->trim_after_decode();
    if (st != NJ2K_OK) {
        d->state = Decoder::State::failed;
        return finish(d, st);
    }
    d->state = Decoder::State::decoded;
    return finish(d, truncated ? NJ2K_TRUNCATED : NJ2K_OK);
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_end_decompress(nj2k_codec_t* p_codec, nj2k_stream_t* p_stream)
{
    using namespace nj2k::detail;
    Decoder* d = Decoder::from(p_codec);
    if (!d) {
        return NJ2K_FALSE;
    }
    if (!Stream::from(p_stream)) {
        return fail(d, NJ2K_E_ARGUMENT, "nj2k_end_decompress: no stream\n");
    }
    // opj_j2k_end_decompress has nothing to do; for a JP2 file OpenJPEG reads the boxes after the
    // codestream too, which neo-j2k does not (the decoded image does not depend on them)
    return NJ2K_TRUE;
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_read_header_memory(nj2k_codec_t* p_codec, const NJ2K_BYTE* p_data,
                                                        NJ2K_SIZE_T p_size, nj2k_image_t* p_image)
{
    using namespace nj2k::detail;
    Decoder* d = Decoder::from(p_codec);
    if (!d) {
        return NJ2K_FALSE;
    }
    if (!p_data && p_size) {
        return fail(d, NJ2K_E_ARGUMENT, "nj2k_read_header_memory: no bytes\n");
    }
    if (const nj2k_status_t st = memory_header(*d, p_data, p_size, false); st != NJ2K_OK) {
        return finish(d, st);
    }
    if (p_image) {
        describe_image(*d, *p_image);
    }
    return finish(d, NJ2K_OK);
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_get_codestream_layout(nj2k_codec_t* p_codec, nj2k_codestream_layout_t* p_layout)
{
    using namespace nj2k::detail;
    Decoder* d = Decoder::from(p_codec);
    if (!d) {
        return NJ2K_FALSE;
    }
    if (!p_layout || p_layout->struct_size < sizeof(nj2k_codestream_layout_t)) {
        return fail(d, NJ2K_E_ARGUMENT, "nj2k_get_codestream_layout: no layout, or its struct_size is too small\n");
    }
    if (!d->header || (d->state != Decoder::State::header && d->state != Decoder::State::decoded)) {
        return fail(d, NJ2K_E_STATE, "nj2k_get_codestream_layout: no header read\n");
    }
    const MainHeader& mh = *d->header;
    p_layout->tx0 = mh.tx0;
    p_layout->ty0 = mh.ty0;
    p_layout->tdx = mh.tdx;
    p_layout->tdy = mh.tdy;
    p_layout->tw = mh.tw;
    p_layout->th = mh.th;
    p_layout->numlayers = mh.cod.numlayers;
    p_layout->min_resolutions = mh.min_resolutions;
    return finish(d, NJ2K_OK);
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_decode_memory(nj2k_codec_t* p_codec, const NJ2K_BYTE* p_data, NJ2K_SIZE_T p_size,
                                                  const nj2k_output_t* p_output)
{
    using namespace nj2k::detail;
    Decoder* d = Decoder::from(p_codec);
    if (!d) {
        return NJ2K_FALSE;
    }
    if ((!p_data && p_size) || !p_output || p_output->struct_size < sizeof(nj2k_output_t) ||
        (p_output->numplanes && !p_output->planes) || p_output->sample_type > NJ2K_SAMPLE_U16) {
        return fail(d, NJ2K_E_ARGUMENT, "nj2k_decode_memory: no bytes, or an invalid output\n");
    }
    // the header: the one the codec holds when the bytes start with its main header (with the
    // component selection made after the header read), else the bytes' own
    if (const nj2k_status_t st = memory_header(*d, p_data, p_size, true); st != NJ2K_OK) {
        return finish(d, st);
    }
    const MainHeader& mh = *d->header;
    const std::uint32_t size_of =
        p_output->sample_type == NJ2K_SAMPLE_U8 ? 1u : (p_output->sample_type == NJ2K_SAMPLE_U16 ? 2u : 4u);
    for (std::uint32_t k = 0; k < p_output->numplanes; ++k) {
        const nj2k_plane_t& pl = p_output->planes[k];
        if (pl.component >= mh.numcomps || !pl.data || pl.sample_stride < size_of ||
            (d->selected_mask && !d->selected_mask[pl.component])) {
            return fail(d, NJ2K_E_OUTPUT, "nj2k_decode_memory: plane %u does not fit the image\n", k);
        }
    }
    if (d->jp2 && !d->ignore_pclr && !d->nselected && ((d->jp2->has_pclr && d->jp2->cmap) || d->jp2->cdef_count)) {
        // (a component selection decodes codestream components, which nj2k_decode() leaves as they are)
        return fail(d, NJ2K_E_UNSUPPORTED,
                    "nj2k_decode_memory: the JP2 file's palette or channel definitions change the components; "
                    "nj2k_decode() applies them\n");
    }
    nj2k_image_comp_t* ic = d->image_comps;
    if (!apply_decode_rule(*d, ic)) {
        return finish(d, NJ2K_E_UNSUPPORTED);
    }
    if (const nj2k_status_t st = check_decode_limits(*d, ic); st != NJ2K_OK) {
        return finish(d, st);
    }
    d->work.reset(d->retain);
    auto* comps = d->work.make_array<OutComp>(mh.numcomps);
    if (!comps) {
        d->trim_after_decode();
        return fail(d, NJ2K_E_NO_MEMORY, "Not enough memory to decode\n");
    }
    for (std::uint32_t c = 0; c < mh.numcomps; ++c) {
        comps[c] = OutComp{ic[c].dx,     ic[c].dy,     ic[c].w,      ic[c].h,
                           ic[c].x0,     ic[c].y0,     ic[c].prec,   ic[c].sgnd,
                           ic[c].factor, ic[c].resno_decoded, nullptr, false};
    }
    // a JP2 file's codestream ends with its box when the box gives its length (DP-15)
    const std::uint64_t end = codestream_end(*d);
    const std::uint64_t size = end && end < p_size ? end : p_size;
    const CodestreamBytes bytes{p_data, 0, size, size, mh.end + 2, true};
    const OutSink sink{false, p_output};
    bool truncated = false;
    const nj2k_status_t st = decode_codestream(*d, bytes, comps, sink, truncated);
    for (std::uint32_t c = 0; c < mh.numcomps; ++c) {
        ic[c].resno_decoded = comps[c].resno_decoded;
    }
    d->trim_after_decode();
    d->state = Decoder::State::decoded;
    if (st != NJ2K_OK) {
        return finish(d, st);
    }
    return finish(d, truncated ? NJ2K_TRUNCATED : NJ2K_OK);
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_decoder_set_retained_bytes(nj2k_codec_t* p_codec, NJ2K_SIZE_T bytes)
{
    Decoder* d = Decoder::from(p_codec);
    if (!d) {
        return NJ2K_FALSE;
    }
    d->retain = bytes;
    return finish(d, NJ2K_OK);
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_get_decoded_tile(nj2k_codec_t* p_codec, nj2k_stream_t*, nj2k_image_t*,
                                                      NJ2K_UINT32)
{
    return not_yet(p_codec, "nj2k_get_decoded_tile");
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_read_tile_header(nj2k_codec_t* p_codec, nj2k_stream_t*, NJ2K_UINT32*,
                                                      NJ2K_UINT32*, NJ2K_INT32*, NJ2K_INT32*, NJ2K_INT32*,
                                                      NJ2K_INT32*, NJ2K_UINT32*, NJ2K_BOOL*)
{
    return not_yet(p_codec, "nj2k_read_tile_header");
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_decode_tile_data(nj2k_codec_t* p_codec, NJ2K_UINT32, NJ2K_BYTE*, NJ2K_UINT32,
                                                      nj2k_stream_t*)
{
    return not_yet(p_codec, "nj2k_decode_tile_data");
}

}  // extern "C"

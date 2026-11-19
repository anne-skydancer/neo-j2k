// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The decoder's side of the API up to the decode itself: codec creation with or without the host's
// allocator, message handlers, settings, limits, reading the header, the reduce factor and policy,
// component selection, the decoded dimensions and the codestream information. Every call that can
// fail records why (nj2k_get_status). Decoding comes in stage 1c; until then nj2k_decode and its
// siblings fail with NJ2K_E_UNSUPPORTED.
#include "codec.h"

#include <cstdarg>
#include <cstring>
#include <new>

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
    return fail(d, NJ2K_E_UNSUPPORTED, "%s: decoding comes with neo-j2k's decode core (stage 1c)\n", what);
}

}  // namespace

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
    d->arena.release_all();
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
    return finish(d, NJ2K_OK);
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_decoder_set_strict_mode(nj2k_codec_t* p_codec, NJ2K_BOOL strict)
{
    Decoder* d = Decoder::from(p_codec);
    if (!d) {
        return NJ2K_FALSE;
    }
    d->strict = strict != 0;
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
    d->arena.set_budget(d->limits.max_memory);
    Source src(*s);
    HeaderSettings hs{};
    hs.limits = d->limits;
    hs.dump = d->dump;
    nj2k_status_t st = NJ2K_OK;
    if (d->format == NJ2K_CODEC_JP2) {
        st = read_jp2_boxes(src, d->arena, d->messages, d->jp2);
        if (st == NJ2K_OK) {
            hs.ihdr_w = d->jp2->w;
            hs.ihdr_h = d->jp2->h;
            // OpenJPEG's J2K decoder takes components of any depth (j2k.c); a JP2 file whose image
            // header gives one depth for all (BPC != 255) has differing ones warned about (jp2.c)
            hs.allow_varying_depth = d->jp2->bpc == 255;
        }
    }
    if (st == NJ2K_OK) {
        st = read_main_header(src, d->arena, d->messages, hs, d->header);
    }
    if (st == NJ2K_OK) {
        st = choose_factor(*d, d->reduce, true, d->factor);
    }
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
    if (d->state == Decoder::State::failed) {
        return fail(d, NJ2K_E_STATE, "nj2k_set_decoded_resolution_factor: the codec failed\n");
    }
    if (d->state == Decoder::State::created) {
        // kept, and checked when the header is read; OpenJPEG returns false here too
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
    const std::uint64_t need = decode_working_memory(mh, d->factor, d->selected_mask);
    if (d->limits.max_memory != 0 && need > d->limits.max_memory) {
        return fail(d, NJ2K_E_LIMIT, "The decode needs %llu bytes of working memory, above the decoder's limit of %llu\n",
                    static_cast<unsigned long long>(need), static_cast<unsigned long long>(d->limits.max_memory));
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

// ---- decoding: stage 1c ----------------------------------------------------------------------------

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_set_decode_area(nj2k_codec_t* p_codec, nj2k_image_t*, NJ2K_INT32, NJ2K_INT32,
                                                     NJ2K_INT32, NJ2K_INT32)
{
    return not_yet(p_codec, "nj2k_set_decode_area");
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_decode(nj2k_codec_t* p_codec, nj2k_stream_t*, nj2k_image_t*)
{
    return not_yet(p_codec, "nj2k_decode");
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_end_decompress(nj2k_codec_t* p_codec, nj2k_stream_t*)
{
    return not_yet(p_codec, "nj2k_end_decompress");
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

// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// One request, written once against OpenJPEG's names and compiled once per API: with the oracle's
// openjpeg.h (the oracle and OpenJPEG-ABI libraries) and with neo-j2k's compatibility header (every
// opj_* name maps to nj2k_*). That second compilation is also the proof that OpenJPEG source builds
// against neo-j2k unchanged.
//
// The includer defines REPLAY_NS and has included the API header and "decoder.h". Every call goes
// through an Api table, so a library loaded at run time is driven exactly like a linked one.
//
// The viewer request follows indra/llimagej2coj/llimagej2coj.cpp of the viewer's release line
// (behaviour only, no viewer code): LLImageJ2COJ::getMetadata's header pass and its component
// check, then decodeImpl's comment scan and reduce clamp, a second codec with the reduce factor set
// before the header, non-strict mode, LLJ2CDecodeGuard, opj_decode, opj_end_decompress, the
// geometry checks and the flipped 8-bit copy of up to four components.

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#define REPLAY_FUNCS(X)                                                                            \
    X(opj_set_default_decoder_parameters) X(opj_create_decompress) X(opj_setup_decoder)            \
    X(opj_set_info_handler) X(opj_set_warning_handler) X(opj_set_error_handler)                    \
    X(opj_stream_create) X(opj_stream_destroy) X(opj_stream_set_user_data)                         \
    X(opj_stream_set_user_data_length) X(opj_stream_set_read_function)                             \
    X(opj_stream_set_write_function) X(opj_stream_set_skip_function)                               \
    X(opj_stream_set_seek_function) X(opj_decoder_set_strict_mode) X(opj_read_header)              \
    X(opj_get_cstr_info) X(opj_destroy_cstr_info) X(opj_set_decoded_resolution_factor)             \
    X(opj_decode) X(opj_end_decompress) X(opj_destroy_codec) X(opj_image_destroy) X(opj_version)   \
    X(opj_set_default_encoder_parameters) X(opj_create_compress) X(opj_setup_encoder)               \
    X(opj_start_compress) X(opj_encode) X(opj_end_compress) X(opj_image_create)                    \
    X(opj_codec_set_threads)

// The decoder limits API of the fork's 1a hotfix, which OpenJPEG 2.5.4 as released lacks: a library
// loaded at run time may not have it, and then takes no request with limits.
#define REPLAY_LIMIT_FUNCS(X) X(opj_decoder_limits_init) X(opj_decoder_set_limits)

namespace REPLAY_NS {

struct Api {
#define REPLAY_DECL(f) decltype(&::f) f = nullptr;
    REPLAY_FUNCS(REPLAY_DECL)
    REPLAY_LIMIT_FUNCS(REPLAY_DECL)
#undef REPLAY_DECL
};

inline Api linked_api()
{
    Api a;
#define REPLAY_LINK(f) a.f = &::f;
    REPLAY_FUNCS(REPLAY_LINK)
    REPLAY_LIMIT_FUNCS(REPLAY_LINK)
#undef REPLAY_LINK
    return a;
}

// ---- the memory stream, with the viewer's read/skip/seek semantics -----------------------------

struct Memory {
    const OPJ_BYTE* buffer = nullptr;
    OPJ_SIZE_T size = 0;
    OPJ_OFF_T offset = 0;
};

inline OPJ_SIZE_T mem_read(void* out, OPJ_SIZE_T bytes, void* user)
{
    auto* m = static_cast<Memory*>(user);
    if (m->offset < 0 || static_cast<OPJ_SIZE_T>(m->offset) >= m->size) {
        m->offset = static_cast<OPJ_OFF_T>(m->size);
        return static_cast<OPJ_SIZE_T>(-1);
    }
    const OPJ_SIZE_T left = m->size - static_cast<OPJ_SIZE_T>(m->offset);
    const OPJ_SIZE_T n = bytes < left ? bytes : left;
    std::memcpy(out, m->buffer + m->offset, n);
    m->offset += static_cast<OPJ_OFF_T>(n);
    return n;
}

inline OPJ_SIZE_T mem_write(void*, OPJ_SIZE_T, void*)
{
    return 0;  // decode streams are read-only
}

inline OPJ_OFF_T mem_skip(OPJ_OFF_T bytes, void* user)
{
    auto* m = static_cast<Memory*>(user);
    const OPJ_OFF_T to = m->offset + bytes;
    if (to < 0 || to > static_cast<OPJ_OFF_T>(m->size)) {
        m->offset = std::clamp<OPJ_OFF_T>(to, 0, static_cast<OPJ_OFF_T>(m->size));
        return static_cast<OPJ_OFF_T>(-1);
    }
    m->offset = to;
    return bytes;
}

inline OPJ_BOOL mem_seek(OPJ_OFF_T to, void* user)
{
    auto* m = static_cast<Memory*>(user);
    if (to < 0 || to > static_cast<OPJ_OFF_T>(m->size)) {
        return OPJ_FALSE;
    }
    m->offset = to;
    return OPJ_TRUE;
}

inline void mem_release(void* user)
{
    auto* m = static_cast<Memory*>(user);
    m->buffer = nullptr;
    m->size = 0;
    m->offset = 0;
}

struct Messages {
    std::uint32_t warnings = 0, errors = 0;
    std::string last_error, warning_kinds;
};

inline void on_info(const char*, void*) {}
inline void on_warning(const char* msg, void* user)
{
    if (user) {
        // each kind of warning once, in the order they first came
        auto* m = static_cast<Messages*>(user);
        ++m->warnings;
        std::string text = msg ? msg : "";
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        const auto paren = text.find(" (");  // drop per-packet details
        if (paren != std::string::npos) {
            text.resize(paren);
        }
        if (m->warning_kinds.size() < 400 && m->warning_kinds.find(text) == std::string::npos) {
            m->warning_kinds += m->warning_kinds.empty() ? text : " | " + text;
        }
    }
}
inline void on_error(const char* msg, void* user)
{
    if (user) {
        // every error, oldest first: the first one usually names the cause
        auto* m = static_cast<Messages*>(user);
        ++m->errors;
        std::string text = msg ? msg : "";
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        if (m->last_error.size() < 400) {
            m->last_error += m->last_error.empty() ? text : " | " + text;
        }
    }
}

// Owns everything one codec use creates, and releases it in the viewer's order.
struct Session {
    const Api& A;
    opj_codec_t* codec = nullptr;
    opj_stream_t* stream = nullptr;
    opj_image_t* image = nullptr;
    opj_codestream_info_v2_t* info = nullptr;
    Memory memory;
    Messages messages;

    explicit Session(const Api& api) : A(api) {}
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session()
    {
        if (codec) {
            A.opj_destroy_codec(codec);
        }
        if (image) {
            A.opj_image_destroy(image);
        }
        if (stream) {
            A.opj_stream_destroy(stream);
        }
        if (info) {
            A.opj_destroy_cstr_info(&info);
        }
    }

    bool open(const nj2k::test::Request& rq, opj_dparameters_t& params, bool count_messages)
    {
        codec = A.opj_create_decompress(rq.jp2 ? OPJ_CODEC_JP2 : OPJ_CODEC_J2K);
        if (!codec) {
            return false;
        }
        void* user = count_messages ? &messages : nullptr;
        A.opj_set_info_handler(codec, on_info, user);
        A.opj_set_warning_handler(codec, on_warning, user);
        A.opj_set_error_handler(codec, on_error, user);
        if (!A.opj_setup_decoder(codec, &params)) {
            return false;
        }
        if (rq.threads != 0 && !A.opj_codec_set_threads(codec, rq.threads)) {
            return false;
        }
        if (rq.limits) {
            // before the header: opj_decoder_set_limits refuses them after it
            opj_decoder_limits_t limits;
            if (!A.opj_decoder_limits_init || !A.opj_decoder_set_limits ||
                !A.opj_decoder_limits_init(&limits, OPJ_DECODER_LIMITS_SL_TEXTURE)) {
                return false;
            }
            if (rq.max_memory) {
                limits.max_memory = static_cast<OPJ_UINT64>(rq.max_memory);
            }
            if (!A.opj_decoder_set_limits(codec, &limits)) {
                return false;
            }
        }
        stream = A.opj_stream_create(static_cast<OPJ_SIZE_T>(rq.size), OPJ_TRUE);
        if (!stream) {
            return false;
        }
        memory.buffer = rq.data;
        memory.size = static_cast<OPJ_SIZE_T>(rq.size);
        memory.offset = 0;
        A.opj_stream_set_user_data(stream, &memory, mem_release);
        A.opj_stream_set_user_data_length(stream, static_cast<OPJ_UINT64>(rq.size));
        A.opj_stream_set_read_function(stream, mem_read);
        A.opj_stream_set_write_function(stream, mem_write);
        A.opj_stream_set_skip_function(stream, mem_skip);
        A.opj_stream_set_seek_function(stream, mem_seek);
        return true;
    }
};

inline void fail(nj2k::test::Outcome& out, const char* stage, const Messages& m)
{
    out.ok = false;
    out.stage = stage;
    out.warnings = m.warnings;
    out.errors = m.errors;
    out.last_error = m.last_error;
    out.warning_kinds = m.warning_kinds;
}

// Geometry, samples and (when the request inspects) the identity hash of a decoded image.
inline bool finish(nj2k::test::Outcome& out, const opj_image_t* img, const nj2k::test::Request& rq, const Messages& m)
{
    out.warnings = m.warnings;
    out.errors = m.errors;
    out.last_error = m.last_error;
    out.warning_kinds = m.warning_kinds;
    if (!img || !img->comps || img->numcomps == 0) {
        fail(out, "decode", m);
        return false;
    }
    out.x0 = img->x0;
    out.y0 = img->y0;
    out.x1 = img->x1;
    out.y1 = img->y1;
    out.numcomps = img->numcomps;
    out.color_space = static_cast<int>(img->color_space);
    nj2k::test::Fnv64 h;
    h.u32(img->x0);
    h.u32(img->y0);
    h.u32(img->x1);
    h.u32(img->y1);
    h.u32(img->numcomps);
    h.u32(static_cast<std::uint32_t>(img->color_space));
    out.comps.clear();
    out.planes.clear();
    for (OPJ_UINT32 c = 0; c < img->numcomps; ++c) {
        const opj_image_comp_t& ic = img->comps[c];
        nj2k::test::Component comp;
        comp.w = ic.w;
        comp.h = ic.h;
        comp.dx = ic.dx;
        comp.dy = ic.dy;
        comp.x0 = ic.x0;
        comp.y0 = ic.y0;
        comp.prec = ic.prec;
        comp.sgnd = ic.sgnd;
        comp.factor = ic.factor;
        comp.resno_decoded = ic.resno_decoded;
        out.comps.push_back(comp);
        h.u32(ic.w);
        h.u32(ic.h);
        h.u32(ic.dx);
        h.u32(ic.dy);
        h.u32(ic.x0);
        h.u32(ic.y0);
        h.u32(ic.prec);
        h.u32(ic.sgnd);
        h.u32(ic.factor);
        h.u32(ic.resno_decoded);
        const std::size_t n = static_cast<std::size_t>(ic.w) * ic.h;
        if (n && !ic.data) {
            fail(out, "geometry", m);
            return false;
        }
        if (rq.inspect) {
            for (std::size_t i = 0; i < n; ++i) {
                h.u32(static_cast<std::uint32_t>(ic.data[i]));
            }
        }
        if (rq.capture) {
            out.planes.emplace_back(ic.data, ic.data + n);
        }
    }
    out.ok = true;
    out.stage.clear();
    out.hash = rq.inspect ? h.value() : 0;
    return true;
}

// One codec: reduce factor and layer limit in the parameters, strict or not.
inline nj2k::test::Outcome replay_plain(const Api& A, const nj2k::test::Request& rq)
{
    nj2k::test::Outcome out;
    opj_dparameters_t params;
    A.opj_set_default_decoder_parameters(&params);
    params.cp_reduce = static_cast<OPJ_UINT32>(rq.reduce_after_header ? 0 : rq.reduce);
    params.cp_layer = static_cast<OPJ_UINT32>(rq.layers);
    Session s(A);
    if (!s.open(rq, params, rq.inspect)) {
        fail(out, "codec", s.messages);
        return out;
    }
    A.opj_decoder_set_strict_mode(s.codec, rq.mode == nj2k::test::Mode::Strict ? OPJ_TRUE : OPJ_FALSE);
    if (!A.opj_read_header(s.stream, s.codec, &s.image)) {
        fail(out, "header", s.messages);
        return out;
    }
    if (rq.reduce_after_header && rq.reduce > 0 &&
        !A.opj_set_decoded_resolution_factor(s.codec, static_cast<OPJ_UINT32>(rq.reduce))) {
        fail(out, "header", s.messages);
        return out;
    }
    const bool decoded = A.opj_decode(s.codec, s.stream, s.image) != 0;
    const bool ended = decoded && A.opj_end_decompress(s.codec, s.stream) != 0;
    if (!decoded || !ended) {
        fail(out, "decode", s.messages);
        return out;
    }
    finish(out, s.image, rq, s.messages);
    return out;
}

// The viewer's request.
inline nj2k::test::Outcome replay_viewer(const Api& A, const nj2k::test::Request& rq)
{
    namespace viewer = nj2k::test::viewer;
    nj2k::test::Outcome out;

    // getMetadata: header pass on its own codec, with the dump flag
    int width = 0, height = 0, components = 0, resolutions = 0;
    {
        opj_dparameters_t params;
        std::memset(&params, 0, sizeof(params));
        A.opj_set_default_decoder_parameters(&params);
        params.flags |= OPJ_DPARAMETERS_DUMP_FLAG;
        Session meta(A);
        if (!meta.open(rq, params, false)) {
            fail(out, "header", meta.messages);
            return out;
        }
        A.opj_decoder_set_strict_mode(meta.codec, OPJ_FALSE);
        if (!A.opj_read_header(meta.stream, meta.codec, &meta.image)) {
            fail(out, "header", meta.messages);
            return out;
        }
        meta.info = A.opj_get_cstr_info(meta.codec);
        if (!meta.info) {
            fail(out, "header", meta.messages);
            return out;
        }
        width = viewer::grid_side(meta.info->tw, meta.info->tdx);
        height = viewer::grid_side(meta.info->th, meta.info->tdy);
        components = static_cast<int>(meta.info->nbcomps);
        const std::uint64_t gw = std::uint64_t(meta.info->tw) * meta.info->tdx;
        const std::uint64_t gh = std::uint64_t(meta.info->th) * meta.info->tdy;
        if (meta.info->m_default_tile_info.tccp_info && gw <= 0xffff && gh <= 0xffff) {
            for (OPJ_UINT32 c = 0; c < meta.info->nbcomps; ++c) {
                const int r = static_cast<int>(meta.info->m_default_tile_info.tccp_info[c].numresolutions);
                resolutions = c == 0 ? r : std::min(resolutions, r);
            }
        }
        // getMetadata refuses a component count the viewer does not take right after the header,
        // before it stores anything, as a header that does not read
        if (!viewer::components_supported(static_cast<std::uint64_t>(meta.info->nbcomps))) {
            fail(out, "header", meta.messages);
            return out;
        }
    }
    (void)components;

    // decodeImpl: the comment scan reads the first 1024 bytes; its result does not reach the pixels
    const int reduce = viewer::reduce_factor(rq.reduce, resolutions, width, height);
    opj_dparameters_t params;
    std::memset(&params, 0, sizeof(params));
    A.opj_set_default_decoder_parameters(&params);
    params.cp_reduce = 0;
    Session s(A);
    if (!s.open(rq, params, rq.inspect)) {
        fail(out, "codec", s.messages);
        return out;
    }
    A.opj_set_decoded_resolution_factor(s.codec, static_cast<OPJ_UINT32>(reduce));
    A.opj_decoder_set_strict_mode(s.codec, OPJ_FALSE);
    if (!A.opj_read_header(s.stream, s.codec, &s.image)) {
        fail(out, "header", s.messages);
        return out;
    }
    {
        const opj_image_t* im = s.image;
        const std::uint64_t ref_w = im->x1 > im->x0 ? im->x1 - im->x0 : 0;
        const std::uint64_t ref_h = im->y1 > im->y0 ? im->y1 - im->y0 : 0;
        if (!viewer::decode_allowed(ref_w, ref_h, im->numcomps, static_cast<unsigned>(reduce))) {
            fail(out, "guard", s.messages);
            return out;
        }
    }
    const bool decoded = A.opj_decode(s.codec, s.stream, s.image) != 0;
    A.opj_end_decompress(s.codec, s.stream);
    if (!decoded || !s.image || !s.image->numcomps) {
        fail(out, "decode", s.messages);
        return out;
    }

    // the viewer's checks and its copy: up to four components, flipped, the low byte of each sample
    const opj_image_t* im = s.image;
    const int channels = std::min(static_cast<int>(im->numcomps), 4);
    const OPJ_UINT32 w = im->comps[0].w, h = im->comps[0].h, f = im->comps[0].factor;
    bool geometry = channels >= 1 && w > 0 && h > 0 && w <= 65535u && h <= 65535u;
    for (int c = 0; geometry && c < channels; ++c) {
        geometry = im->comps[c].w == w && im->comps[c].h == h && im->comps[c].factor == f && im->comps[c].data;
    }
    if (!geometry) {
        fail(out, "geometry", s.messages);
        return out;
    }
    if (!finish(out, im, rq, s.messages)) {
        return out;
    }
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(w) * h * channels);
    for (int c = 0; c < channels; ++c) {
        const OPJ_INT32* src = im->comps[c].data;
        std::size_t o = static_cast<std::size_t>(c);
        for (std::int64_t y = static_cast<std::int64_t>(h) - 1; y >= 0; --y) {
            for (OPJ_UINT32 x = 0; x < w; ++x) {
                raw[o] = static_cast<std::uint8_t>(src[static_cast<std::size_t>(y) * w + x]);
                o += static_cast<std::size_t>(channels);
            }
        }
    }
    if (rq.inspect) {
        nj2k::test::Fnv64 h8;
        h8.bytes(raw.data(), raw.size());
        out.u8_hash = h8.value();
    } else {
        nj2k::test::keep_alive(raw.data(), raw.size());  // the copy is the viewer's work: keep it timed
    }
    return out;
}

inline nj2k::test::Outcome replay(const Api& A, const nj2k::test::Request& rq)
{
    return rq.mode == nj2k::test::Mode::Viewer ? replay_viewer(A, rq) : replay_plain(A, rq);
}

// The header read alone: the image opj_read_header gives and opj_get_cstr_info's information.
inline nj2k::test::HeaderFacts replay_header(const Api& A, const nj2k::test::Request& rq)
{
    nj2k::test::HeaderFacts f;
    opj_dparameters_t params;
    std::memset(&params, 0, sizeof(params));
    A.opj_set_default_decoder_parameters(&params);
    const bool viewer = rq.mode == nj2k::test::Mode::Viewer;
    if (!viewer) {
        params.cp_reduce = static_cast<OPJ_UINT32>(rq.reduce);
        params.cp_layer = static_cast<OPJ_UINT32>(rq.layers);
    }
    Session s(A);
    if (!s.open(rq, params, rq.inspect)) {
        f.stage = "codec";
        f.last_error = s.messages.last_error;
        return f;
    }
    if (viewer) {
        A.opj_set_decoded_resolution_factor(s.codec, static_cast<OPJ_UINT32>(rq.reduce));
    }
    A.opj_decoder_set_strict_mode(s.codec, rq.mode == nj2k::test::Mode::Strict ? OPJ_TRUE : OPJ_FALSE);
    if (!A.opj_read_header(s.stream, s.codec, &s.image) || !s.image) {
        f.stage = "header";
        f.last_error = s.messages.last_error;
        return f;
    }
    s.info = A.opj_get_cstr_info(s.codec);
    if (!s.info) {
        f.stage = "info";
        f.last_error = s.messages.last_error;
        return f;
    }
    const opj_image_t* im = s.image;
    f.ok = true;
    f.x0 = im->x0;
    f.y0 = im->y0;
    f.x1 = im->x1;
    f.y1 = im->y1;
    f.numcomps = im->numcomps;
    f.color_space = static_cast<int>(im->color_space);
    for (OPJ_UINT32 c = 0; c < im->numcomps; ++c) {
        const opj_image_comp_t& ic = im->comps[c];
        nj2k::test::HeaderFacts::Comp comp;
        comp.dx = ic.dx;
        comp.dy = ic.dy;
        comp.w = ic.w;
        comp.h = ic.h;
        comp.x0 = ic.x0;
        comp.y0 = ic.y0;
        comp.prec = ic.prec;
        comp.sgnd = ic.sgnd;
        comp.factor = ic.factor;
        comp.resno_decoded = ic.resno_decoded;
        comp.alpha = ic.alpha;
        f.comps.push_back(comp);
    }
    f.icc_buffer = im->icc_profile_buf != nullptr;
    if (im->icc_profile_buf && im->icc_profile_len) {
        f.icc.assign(im->icc_profile_buf, im->icc_profile_buf + im->icc_profile_len);
    }
    const opj_codestream_info_v2_t* info = s.info;
    f.tx0 = info->tx0;
    f.ty0 = info->ty0;
    f.tdx = info->tdx;
    f.tdy = info->tdy;
    f.tw = info->tw;
    f.th = info->th;
    f.nbcomps = info->nbcomps;
    f.csty = info->m_default_tile_info.csty;
    f.prg = static_cast<int>(info->m_default_tile_info.prg);
    f.numlayers = info->m_default_tile_info.numlayers;
    f.mct = info->m_default_tile_info.mct;
    for (OPJ_UINT32 c = 0; info->m_default_tile_info.tccp_info && c < info->nbcomps; ++c) {
        const opj_tccp_info_t& t = info->m_default_tile_info.tccp_info[c];
        nj2k::test::HeaderFacts::Tccp tc;
        tc.csty = t.csty;
        tc.numresolutions = t.numresolutions;
        tc.cblkw = t.cblkw;
        tc.cblkh = t.cblkh;
        tc.cblksty = t.cblksty;
        tc.qmfbid = t.qmfbid;
        tc.qntsty = t.qntsty;
        tc.numgbits = t.numgbits;
        tc.roishift = t.roishift;
        // what OpenJPEG reports: the step sizes of 1 or 3 x resolutions - 2 bands when that is below
        // 97, and the first numresolutions bytes of the precinct arrays (it copies bytes, not elements)
        const OPJ_UINT32 bands = t.qntsty == 1 ? 1 : 3 * t.numresolutions - 2;
        if (bands < OPJ_J2K_MAXBANDS) {
            tc.mant.assign(t.stepsizes_mant, t.stepsizes_mant + bands);
            tc.expn.assign(t.stepsizes_expn, t.stepsizes_expn + bands);
        }
        if (t.numresolutions < OPJ_J2K_MAXRLVLS) {
            const auto* w = reinterpret_cast<const std::uint8_t*>(t.prcw);
            const auto* h = reinterpret_cast<const std::uint8_t*>(t.prch);
            tc.prc.assign(w, w + t.numresolutions);
            tc.prc.insert(tc.prc.end(), h, h + t.numresolutions);
        }
        f.tccp.push_back(tc);
    }
    return f;
}

// ---- the viewer's upload encode (JPEG2KEncode::encode, release line) --------------------------

struct Grow {
    std::vector<std::uint8_t>* out;
    std::size_t pos = 0;
};

inline OPJ_SIZE_T grow_write(void* buf, OPJ_SIZE_T n, void* user)
{
    auto* g = static_cast<Grow*>(user);
    if (g->pos + n > g->out->size()) {
        g->out->resize(g->pos + n);
    }
    std::memcpy(g->out->data() + g->pos, buf, n);
    g->pos += n;
    return n;
}

inline OPJ_OFF_T grow_skip(OPJ_OFF_T n, void* user)
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

inline OPJ_BOOL grow_seek(OPJ_OFF_T to, void* user)
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

// The viewer's upload encode (UploadSpec, decoder.h). Lossy: RLCP 9/7 with rate layers by surface
// (estimate_num_layers, rates 8 * 4^i); lossless: 5/3, one layer. MCT from 3 components; fewer
// resolutions for images below 128 px a side; the "no comment" comment; the spec's fields override.
inline bool replay_upload_ex(const Api& A, const nj2k::test::UploadSpec& us, std::vector<std::uint8_t>& out)
{
    const std::uint8_t* raw = us.raw;
    const int width = us.width, height = us.height, comps = us.comps;
    const bool reversible = us.reversible;
    out.clear();
    opj_cparameters_t p;
    std::memset(&p, 0, sizeof(p));
    A.opj_set_default_encoder_parameters(&p);
    p.cod_format = OPJ_CODEC_J2K;
    p.prog_order = OPJ_RLCP;
    p.cp_disto_alloc = 1;
    p.max_cs_size = us.max_cs_size;
    if (reversible) {
        p.irreversible = 0;
        p.tcp_numlayers = 1;
        p.tcp_rates[0] = 0.0f;
    } else {
        p.irreversible = 1;
    }
    std::string comment_text = us.comment ? us.comment : "";
    p.cp_comment = us.comment ? &comment_text[0] : nullptr;

    std::vector<opj_image_cmptparm_t> cp(static_cast<std::size_t>(comps));
    for (auto& c : cp) {
        std::memset(&c, 0, sizeof(c));
        c.prec = 8;
        c.dx = static_cast<OPJ_UINT32>(p.subsampling_dx);
        c.dy = static_cast<OPJ_UINT32>(p.subsampling_dy);
        c.w = static_cast<OPJ_UINT32>(width);
        c.h = static_cast<OPJ_UINT32>(height);
    }
    opj_image_t* image = A.opj_image_create(static_cast<OPJ_UINT32>(comps), cp.data(), OPJ_CLRSPC_SRGB);
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
    opj_codec_t* enc = A.opj_create_compress(OPJ_CODEC_J2K);
    if (!enc) {
        A.opj_image_destroy(image);
        return false;
    }
    A.opj_set_error_handler(enc, on_info, nullptr);
    A.opj_set_warning_handler(enc, on_info, nullptr);
    A.opj_set_info_handler(enc, on_info, nullptr);
    p.tcp_mct = static_cast<char>(us.mct >= 0 ? us.mct : (image->numcomps >= 3 ? 1 : 0));
    if (p.irreversible) {
        const std::uint32_t surface = static_cast<std::uint32_t>(width) * static_cast<std::uint32_t>(height);
        const int layers = surface <= 1024 ? 2 : surface <= 16384 ? 3 : surface <= 262144 ? 4 : surface <= 1048576 ? 5 : 6;
        p.tcp_numlayers = layers;
        for (int k = layers - 1; k >= 0; --k) {
            p.tcp_rates[layers - 1 - k] = 8.0f * static_cast<float>(1 << (k << 1));
        }
    }
    if (us.layers > 0) {
        p.tcp_numlayers = us.layers;
    }
    for (std::size_t k = 0; k < us.rates.size() && k < 100; ++k) {
        p.tcp_rates[k] = us.rates[k];
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
            ++levels;  // floor(log2(min_dim))
        }
        p.numresolution = 1 + levels;
    }
    if (us.numres > 0) {
        p.numresolution = us.numres;
    }
    bool ok = A.opj_setup_encoder(enc, &p, image) != 0;
    opj_stream_t* st = nullptr;
    Grow sink{&out};
    if (ok) {
        // the viewer's zeroed size guess; below 4 px a side it is 0, OpenJPEG makes no stream of
        // 0 bytes, and the viewer's upload fails, so this one fails too
        const std::size_t guess = static_cast<std::size_t>(wt) * ht * 64 * 64 * 3;
        out.assign(guess, 0);
        st = A.opj_stream_create(guess, OPJ_FALSE);
        ok = st != nullptr;
    }
    if (ok) {
        A.opj_stream_set_user_data(st, &sink, nullptr);
        A.opj_stream_set_user_data_length(st, out.size());
        A.opj_stream_set_write_function(st, grow_write);
        A.opj_stream_set_skip_function(st, grow_skip);
        A.opj_stream_set_seek_function(st, grow_seek);
        ok = A.opj_start_compress(enc, image, st) && A.opj_encode(enc, st) && A.opj_end_compress(enc, st);
    }
    out.resize(ok ? sink.pos : 0);
    if (st) {
        A.opj_stream_destroy(st);
    }
    A.opj_destroy_codec(enc);
    A.opj_image_destroy(image);
    return ok;
}

inline bool replay_upload(const Api& A, const std::uint8_t* raw, int width, int height, int comps, bool reversible,
                          std::vector<std::uint8_t>& out)
{
    nj2k::test::UploadSpec us;
    us.raw = raw;
    us.width = width;
    us.height = height;
    us.comps = comps;
    us.reversible = reversible;
    return replay_upload_ex(A, us, out);
}

// EncodeSpec's codestream (decoder.h)
inline bool replay_encode(const Api& A, const nj2k::test::EncodeSpec& s, std::vector<std::uint8_t>& out)
{
    out.clear();
    const std::size_t comps = s.planes.size();
    if (comps == 0 || s.dx.size() != comps || s.dy.size() != comps) {
        return false;
    }
    opj_cparameters_t p;
    std::memset(&p, 0, sizeof(p));
    A.opj_set_default_encoder_parameters(&p);
    p.cod_format = OPJ_CODEC_J2K;
    p.irreversible = s.reversible ? 0 : 1;
    p.tcp_numlayers = 1;
    p.tcp_rates[0] = s.reversible ? 0.0f : 8.0f;
    p.cp_disto_alloc = 1;
    p.numresolution = s.numres;
    p.tcp_mct = static_cast<char>(s.mct ? 1 : 0);
    if (s.tile) {
        p.tile_size_on = OPJ_TRUE;
        p.cp_tdx = static_cast<int>(s.tile);
        p.cp_tdy = static_cast<int>(s.tile);
    }
    std::vector<opj_image_cmptparm_t> cp(comps);
    for (std::size_t c = 0; c < comps; ++c) {
        std::memset(&cp[c], 0, sizeof(cp[c]));
        cp[c].prec = 8;
        cp[c].dx = s.dx[c];
        cp[c].dy = s.dy[c];
        cp[c].w = (s.width + s.dx[c] - 1) / s.dx[c];
        cp[c].h = (s.height + s.dy[c] - 1) / s.dy[c];
        if (s.planes[c].size() != std::size_t(cp[c].w) * cp[c].h) {
            return false;
        }
    }
    opj_image_t* image = A.opj_image_create(static_cast<OPJ_UINT32>(comps), cp.data(), OPJ_CLRSPC_SRGB);
    if (!image) {
        return false;
    }
    image->x1 = s.width;
    image->y1 = s.height;
    for (std::size_t c = 0; c < comps; ++c) {
        for (std::size_t i = 0; i < s.planes[c].size(); ++i) {
            image->comps[c].data[i] = s.planes[c][i];
        }
    }
    opj_codec_t* enc = A.opj_create_compress(OPJ_CODEC_J2K);
    bool ok = enc != nullptr;
    if (ok) {
        A.opj_set_error_handler(enc, on_info, nullptr);
        A.opj_set_warning_handler(enc, on_info, nullptr);
        A.opj_set_info_handler(enc, on_info, nullptr);
        ok = A.opj_setup_encoder(enc, &p, image) != 0;
    }
    opj_stream_t* st = nullptr;
    Grow sink{&out};
    if (ok) {
        st = A.opj_stream_create(1 << 16, OPJ_FALSE);
        ok = st != nullptr;
    }
    if (ok) {
        A.opj_stream_set_user_data(st, &sink, nullptr);
        A.opj_stream_set_write_function(st, grow_write);
        A.opj_stream_set_skip_function(st, grow_skip);
        A.opj_stream_set_seek_function(st, grow_seek);
        ok = A.opj_start_compress(enc, image, st) && A.opj_encode(enc, st) && A.opj_end_compress(enc, st);
    }
    out.resize(ok ? sink.pos : 0);
    if (st) {
        A.opj_stream_destroy(st);
    }
    if (enc) {
        A.opj_destroy_codec(enc);
    }
    A.opj_image_destroy(image);
    return ok;
}

}  // namespace REPLAY_NS

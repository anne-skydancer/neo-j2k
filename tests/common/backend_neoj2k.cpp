// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// neo-j2k as a backend, driven by the same request source as OpenJPEG through the compatibility
// header: this file compiling is the check that OpenJPEG code builds against neo-j2k unchanged.
// Since stage 1c it decodes.
#include <neoj2k/neoj2k_openjpeg.h>

#include <algorithm>
#include <cstdlib>

#include <string>
#include <type_traits>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <malloc.h>
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "common.h"
#include "decoder.h"

#define REPLAY_NS nj2k_replay_neoj2k
#include "replay.inl"

namespace nj2k::test {

namespace {

class NeoBackend final : public Backend {
public:
    NeoBackend() : api_(REPLAY_NS::linked_api()) {}
    const std::string& name() const noexcept override { return name_; }
    Outcome decode(const Request& rq) override { return REPLAY_NS::replay(api_, rq); }
    HeaderFacts header(const Request& rq) override { return REPLAY_NS::replay_header(api_, rq); }
    bool encode_upload(const std::uint8_t* raw, int width, int height, int comps, bool reversible,
                       std::vector<std::uint8_t>& out) override
    {
        return REPLAY_NS::replay_upload(api_, raw, width, height, comps, reversible, out);
    }

private:
    std::string name_ = "neoj2k";
    REPLAY_NS::Api api_;
};

// The functions of neo-j2k's own path: the linked library's, or those of another build of it
// loaded at run time (neolib:<path>, the bench's A/B of two builds in one process).
struct NeoFns {
    decltype(&nj2k_create_decompress_with_allocator) create;
    decltype(&nj2k_set_default_decoder_parameters) defaults;
    decltype(&nj2k_setup_decoder) setup;
    decltype(&nj2k_decoder_set_strict_mode) strict;
    decltype(&nj2k_decoder_limits_init) limits_init;
    decltype(&nj2k_decoder_set_limits) set_limits;
    decltype(&nj2k_destroy_codec) destroy;
    decltype(&nj2k_set_decoded_resolution_factor) factor;
    decltype(&nj2k_read_header_memory) header;
    decltype(&nj2k_get_codestream_layout) layout;
    decltype(&nj2k_decode_memory) decode;
};

NeoFns linked_fns()
{
    return NeoFns{nj2k_create_decompress_with_allocator, nj2k_set_default_decoder_parameters, nj2k_setup_decoder,
                  nj2k_decoder_set_strict_mode, nj2k_decoder_limits_init, nj2k_decoder_set_limits, nj2k_destroy_codec,
                  nj2k_set_decoded_resolution_factor, nj2k_read_header_memory, nj2k_get_codestream_layout,
                  nj2k_decode_memory};
}

// neo-j2k's own path, as the viewer will call it (stage 8): one codec for every request, kept
// between them, under the Second Life limits, its memory from a counting allocator hook; the header
// read from memory (parsed again only when the main header changes) and the image decoded into the
// viewer's 8-bit, bottom-up, interleaved buffer. The viewer's request otherwise as replay_viewer has
// it: the header pass at reduce 0, its reduce factor, size guard and geometry checks. Viewer mode
// only; other modes, and header reads, go through the compatibility API.
class NeoMemoryBackend final : public Backend {
public:
    explicit NeoMemoryBackend(const NeoFns& fns, std::string name = "neoj2k-memory")
        : api_(REPLAY_NS::linked_api()), f_(fns), name_(std::move(name))
    {
        const nj2k_allocator_t hook{sizeof(nj2k_allocator_t), alloc, release, this};
        codec_ = f_.create(NJ2K_CODEC_J2K, &hook);
        if (codec_) {
            nj2k_dparameters_t p;
            f_.defaults(&p);
            f_.setup(codec_, &p);
            f_.strict(codec_, NJ2K_FALSE);
            nj2k_decoder_limits_t limits;
            f_.limits_init(&limits, NJ2K_DECODER_LIMITS_SL_TEXTURE);
            f_.set_limits(codec_, &limits);
        }
    }
    ~NeoMemoryBackend() override
    {
        if (codec_) {
            f_.destroy(codec_);
        }
    }
    const std::string& name() const noexcept override { return name_; }
    HeaderFacts header(const Request& rq) override { return REPLAY_NS::replay_header(api_, rq); }
    bool encode_upload(const std::uint8_t* raw, int width, int height, int comps, bool reversible,
                       std::vector<std::uint8_t>& out) override
    {
        return REPLAY_NS::replay_upload(api_, raw, width, height, comps, reversible, out);
    }
    bool alloc_counts(std::uint64_t out[4]) override
    {
        out[0] = allocs_;
        out[1] = 0;
        out[2] = frees_;
        out[3] = bytes_;
        return true;
    }
    void alloc_reset() override { allocs_ = frees_ = bytes_ = 0; }

    Outcome decode(const Request& rq) override
    {
        if (rq.mode != Mode::Viewer || rq.jp2) {
            return REPLAY_NS::replay(api_, rq);
        }
        Outcome out;
        auto refuse = [&](const char* stage) {
            out.ok = false;
            out.stage = stage;
            return out;
        };
        if (!codec_) {
            return refuse("codec");
        }
        // getMetadata: the header at reduce 0, the tile grid's size, the fewest resolutions
        nj2k_image_t image;
        nj2k_codestream_layout_t layout{};
        layout.struct_size = sizeof layout;
        f_.factor(codec_, 0);
        if (!f_.header(codec_, rq.data, rq.size, &image) || !f_.layout(codec_, &layout)) {
            return refuse("header");
        }
        if (!viewer::components_supported(image.numcomps)) {
            return refuse("header");
        }
        const int width = viewer::grid_side(layout.tw, layout.tdx);
        const int height = viewer::grid_side(layout.th, layout.tdy);
        const std::uint64_t gw = std::uint64_t(layout.tw) * layout.tdx, gh = std::uint64_t(layout.th) * layout.tdy;
        const int resolutions = gw <= 0xffff && gh <= 0xffff ? static_cast<int>(layout.min_resolutions) : 0;
        const int reduce = viewer::reduce_factor(rq.reduce, resolutions, width, height);
        // decodeImpl: the factor, the header with it (held: not parsed again), the guard
        if (!f_.factor(codec_, static_cast<NJ2K_UINT32>(reduce)) || !f_.header(codec_, rq.data, rq.size, &image)) {
            return refuse("header");
        }
        const std::uint64_t ref_w = image.x1 > image.x0 ? image.x1 - image.x0 : 0;
        const std::uint64_t ref_h = image.y1 > image.y0 ? image.y1 - image.y0 : 0;
        if (!viewer::decode_allowed(ref_w, ref_h, image.numcomps, static_cast<unsigned>(reduce))) {
            return refuse("guard");
        }
        const int channels = std::min(static_cast<int>(image.numcomps), 4);
        const NJ2K_UINT32 w = image.comps[0].w, h = image.comps[0].h, f = image.comps[0].factor;
        bool geometry = channels >= 1 && w > 0 && h > 0 && w <= 65535u && h <= 65535u;
        for (int c = 0; geometry && c < channels; ++c) {
            geometry = image.comps[c].w == w && image.comps[c].h == h && image.comps[c].factor == f;
        }
        if (!geometry) {
            return refuse("geometry");
        }
        // the viewer's raw image: the low byte of each sample, interleaved, bottom row first
        raw_.resize(static_cast<std::size_t>(w) * h * channels);
        nj2k_plane_t planes[4];
        const std::int64_t row = std::int64_t(w) * channels;
        for (int c = 0; c < channels; ++c) {
            planes[c] = nj2k_plane_t{raw_.data() + std::size_t(h - 1) * std::size_t(row) + c, -row,
                                     static_cast<NJ2K_UINT32>(channels), static_cast<NJ2K_UINT32>(c)};
        }
        const nj2k_output_t output{sizeof(nj2k_output_t), NJ2K_SAMPLE_U8, static_cast<NJ2K_UINT32>(channels), planes};
        if (!f_.decode(codec_, rq.data, rq.size, &output)) {
            return refuse("decode");
        }
        out.ok = true;
        out.x0 = image.x0;
        out.y0 = image.y0;
        out.x1 = image.x1;
        out.y1 = image.y1;
        out.numcomps = image.numcomps;
        if (rq.inspect) {
            Fnv64 h8;
            h8.bytes(raw_.data(), raw_.size());
            out.u8_hash = h8.value();
        } else {
            keep_alive(raw_.data(), raw_.size());
        }
        return out;
    }

private:
    static void* alloc(void* user, NJ2K_SIZE_T size, NJ2K_SIZE_T alignment)
    {
        auto* b = static_cast<NeoMemoryBackend*>(user);
        ++b->allocs_;
        b->bytes_ += size;
#if defined(_WIN32)
        return _aligned_malloc(size, alignment < 16 ? 16 : alignment);
#else
        void* p = nullptr;
        return posix_memalign(&p, alignment < sizeof(void*) ? sizeof(void*) : alignment, size) == 0 ? p : nullptr;
#endif
    }
    static void release(void* user, void* ptr, NJ2K_SIZE_T)
    {
        ++static_cast<NeoMemoryBackend*>(user)->frees_;
#if defined(_WIN32)
        _aligned_free(ptr);
#else
        std::free(ptr);
#endif
    }

    REPLAY_NS::Api api_;
    NeoFns f_;
    std::string name_;
    nj2k_codec_t* codec_ = nullptr;
    std::vector<std::uint8_t> raw_;
    std::uint64_t allocs_ = 0, frees_ = 0, bytes_ = 0;
};

}  // namespace

std::unique_ptr<Backend> make_neoj2k_backend()
{
    return std::make_unique<NeoBackend>();
}

std::unique_ptr<Backend> make_neoj2k_memory_backend()
{
    return std::make_unique<NeoMemoryBackend>(linked_fns());
}

// Another build of neo-j2k's shared library, on its own path as neoj2k-memory, loaded at run time
// and kept loaded until the process ends: the bench compares two builds request by request.
std::unique_ptr<Backend> make_neoj2k_library_backend(const std::string& path, const std::string& name,
                                                     std::string* error)
{
#if defined(_WIN32)
    // the path is UTF-8, as every path of the tests and the bench
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(n > 0 ? n : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), n);
    HMODULE m = LoadLibraryExW(wide.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    auto sym = [m](const char* s) { return m ? reinterpret_cast<void*>(GetProcAddress(m, s)) : nullptr; };
#else
    void* m = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    auto sym = [m](const char* s) { return m ? dlsym(m, s) : nullptr; };
#endif
    if (!m) {
        if (error) {
            *error = "cannot load " + path;
        }
        return nullptr;
    }
    NeoFns f{};
    bool ok = true;
    auto get = [&](auto& fn, const char* s) {
        void* p = sym(s);
        ok = ok && p != nullptr;
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(p);
    };
    get(f.create, "nj2k_create_decompress_with_allocator");
    get(f.defaults, "nj2k_set_default_decoder_parameters");
    get(f.setup, "nj2k_setup_decoder");
    get(f.strict, "nj2k_decoder_set_strict_mode");
    get(f.limits_init, "nj2k_decoder_limits_init");
    get(f.set_limits, "nj2k_decoder_set_limits");
    get(f.destroy, "nj2k_destroy_codec");
    get(f.factor, "nj2k_set_decoded_resolution_factor");
    get(f.header, "nj2k_read_header_memory");
    get(f.layout, "nj2k_get_codestream_layout");
    get(f.decode, "nj2k_decode_memory");
    if (!ok) {
        if (error) {
            *error = path + " lacks neo-j2k's memory API";
        }
        return nullptr;
    }
    return std::make_unique<NeoMemoryBackend>(f, name);
}

}  // namespace nj2k::test

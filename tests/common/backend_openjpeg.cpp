// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// OpenJPEG-API backends: the oracle linked into the test program, and any library with OpenJPEG's
// ABI loaded at run time (the viewer's shipped openjp2.dll, the fork audit's fix build, the
// counting oracle module).
#include <openjpeg.h>

#include "common.h"
#include "decoder.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define REPLAY_NS nj2k_replay_openjpeg
#include "replay.inl"

namespace nj2k::test {

namespace {

class OracleBackend final : public Backend {
public:
    OracleBackend() : api_(REPLAY_NS::linked_api()) {}
    const std::string& name() const noexcept override { return name_; }
    Outcome decode(const Request& rq) override { return REPLAY_NS::replay(api_, rq); }
    HeaderFacts header(const Request& rq) override { return REPLAY_NS::replay_header(api_, rq); }
    bool encode_upload(const std::uint8_t* raw, int width, int height, int comps, bool reversible,
                       std::vector<std::uint8_t>& out) override
    {
        return REPLAY_NS::replay_upload(api_, raw, width, height, comps, reversible, out);
    }

private:
    std::string name_ = "oracle";
    REPLAY_NS::Api api_;
};

using CountsFn = void (*)(unsigned long long*);
using ResetFn = void (*)();

class LibraryBackend final : public Backend {
public:
    ~LibraryBackend() override
    {
#if defined(_WIN32)
        if (module_) {
            FreeLibrary(static_cast<HMODULE>(module_));
        }
#else
        if (module_) {
            dlclose(module_);
        }
#endif
    }

    bool load(const std::string& path, const std::string& name, std::string* error)
    {
        name_ = name;
#if defined(_WIN32)
        const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(n > 0 ? n : 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), n);
        module_ = LoadLibraryExW(wide.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        auto sym = [this](const char* s) { return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(module_), s)); };
#else
        module_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        auto sym = [this](const char* s) { return dlsym(module_, s); };
#endif
        if (!module_) {
            if (error) {
                *error = "cannot load " + path;
            }
            return false;
        }
#define REPLAY_GET(f)                                                                              \
    api_.f = reinterpret_cast<decltype(api_.f)>(sym(#f));                                          \
    if (!api_.f) {                                                                                 \
        if (error) {                                                                               \
            *error = path + " lacks " #f;                                                          \
        }                                                                                          \
        return false;                                                                              \
    }
        REPLAY_FUNCS(REPLAY_GET)
#undef REPLAY_GET
        // optional: a library without the limits API takes every request but those with limits
#define REPLAY_GET_OPTIONAL(f) api_.f = reinterpret_cast<decltype(api_.f)>(sym(#f));
        REPLAY_LIMIT_FUNCS(REPLAY_GET_OPTIONAL)
#undef REPLAY_GET_OPTIONAL
        counts_ = reinterpret_cast<CountsFn>(sym("nj2k_oracle_alloc_counts"));
        reset_ = reinterpret_cast<ResetFn>(sym("nj2k_oracle_alloc_reset"));
        return true;
    }

    const std::string& name() const noexcept override { return name_; }
    Outcome decode(const Request& rq) override { return REPLAY_NS::replay(api_, rq); }
    HeaderFacts header(const Request& rq) override { return REPLAY_NS::replay_header(api_, rq); }
    bool encode_upload(const std::uint8_t* raw, int width, int height, int comps, bool reversible,
                       std::vector<std::uint8_t>& out) override
    {
        return REPLAY_NS::replay_upload(api_, raw, width, height, comps, reversible, out);
    }
    bool alloc_counts(std::uint64_t out[4]) override
    {
        if (!counts_) {
            return false;
        }
        unsigned long long c[4];
        counts_(c);
        for (int i = 0; i < 4; ++i) {
            out[i] = c[i];
        }
        return true;
    }
    void alloc_reset() override
    {
        if (reset_) {
            reset_();
        }
    }

private:
    std::string name_;
    void* module_ = nullptr;
    REPLAY_NS::Api api_;
    CountsFn counts_ = nullptr;
    ResetFn reset_ = nullptr;
};

}  // namespace

bool oracle_encode(const EncodeSpec& spec, std::vector<std::uint8_t>& out)
{
    return REPLAY_NS::replay_encode(REPLAY_NS::linked_api(), spec, out);
}

std::unique_ptr<Backend> make_oracle_backend()
{
    (void)single_thread_openjpeg();
    return std::make_unique<OracleBackend>();
}

std::unique_ptr<Backend> make_library_backend(const std::string& path, const std::string& name, std::string* error)
{
    (void)single_thread_openjpeg();  // before the library loads (a static C runtime copies the environment then)
    auto b = std::make_unique<LibraryBackend>();
    if (!b->load(path, name, error)) {
        return nullptr;
    }
    return b;
}

std::unique_ptr<Backend> make_backend(const std::string& spec, std::string* error)
{
    if (spec == "oracle") {
        return make_oracle_backend();
    }
    if (spec == "neoj2k") {
        return make_neoj2k_backend();
    }
    if (spec == "neoj2k-memory") {
        return make_neoj2k_memory_backend();
    }
    if (spec.rfind("lib:", 0) == 0 || spec.rfind("neolib:", 0) == 0) {
        const bool neo = spec[0] == 'n';
        std::string path = spec.substr(neo ? 7 : 4), name;
        const auto eq = path.rfind('=');
        if (eq != std::string::npos && eq > 2) {
            name = path.substr(eq + 1);
            path.resize(eq);
        } else {
            name = base_name(path);
        }
        return neo ? make_neoj2k_library_backend(path, name, error) : make_library_backend(path, name, error);
    }
    if (error) {
        *error = "unknown backend '" + spec + "' (oracle, neoj2k, neoj2k-memory, lib:<path>[=<name>], neolib:<path>[=<name>])";
    }
    return nullptr;
}

const char* mode_name(Mode m) noexcept
{
    switch (m) {
    case Mode::Plain: return "plain";
    case Mode::Strict: return "strict";
    case Mode::Viewer: return "viewer";
    }
    return "?";
}

bool parse_mode(const std::string& s, Mode& m) noexcept
{
    if (s == "plain") {
        m = Mode::Plain;
    } else if (s == "strict") {
        m = Mode::Strict;
    } else if (s == "viewer") {
        m = Mode::Viewer;
    } else {
        return false;
    }
    return true;
}

}  // namespace nj2k::test

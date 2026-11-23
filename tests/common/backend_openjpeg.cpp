// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// OpenJPEG-API backends: the oracle linked into the test program, and any library with OpenJPEG's
// ABI loaded at run time (the viewer's shipped openjp2.dll, the fork audit's fix build, the
// counting oracle module).
#include <openjpeg.h>

#include <filesystem>

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

// A shared library loaded by path (LoadLibraryExW, dlopen) and its symbols.
class Module {
public:
    Module() = default;
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
    ~Module()
    {
#if defined(_WIN32)
        if (handle_) {
            FreeLibrary(static_cast<HMODULE>(handle_));
        }
#else
        if (handle_) {
            dlclose(handle_);
        }
#endif
    }

    // The path is made absolute first: a bare name would make dlopen search the system's folders, and
    // LOAD_WITH_ALTERED_SEARCH_PATH wants a full path (the program's argument is in the ANSI code page).
    bool open(const std::string& path, std::string* error)
    {
        const std::filesystem::path full = std::filesystem::absolute(std::filesystem::path(path));
#if defined(_WIN32)
        handle_ = LoadLibraryExW(full.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!handle_ && error) {
            char text[256] = {};
            const DWORD code = GetLastError();
            FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, text,
                           sizeof(text) - 1, nullptr);
            std::string why(text);  // FormatMessage ends the text with "\r\n": trimmed, so the error ends with ")"
            while (!why.empty() && (why.back() == '\r' || why.back() == '\n' || why.back() == ' ')) {
                why.pop_back();
            }
            *error = "cannot load " + path + " (error " + std::to_string(code) + ": " + why + ")";
        }
#else
        handle_ = dlopen(full.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle_ && error) {
            const char* why = dlerror();
            *error = "cannot load " + path + " (" + (why ? why : "no reason given") + ")";
        }
#endif
        return handle_ != nullptr;
    }

    void* sym(const char* name) const
    {
#if defined(_WIN32)
        return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_), name));
#else
        return dlsym(handle_, name);
#endif
    }

private:
    void* handle_ = nullptr;
};

class LibraryBackend final : public Backend {
public:
    bool load(const std::string& path, const std::string& name, std::string* error)
    {
        name_ = name;
        if (!module_.open(path, error)) {
            return false;
        }
        auto sym = [this](const char* s) { return module_.sym(s); };
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
    Module module_;
    REPLAY_NS::Api api_;
    CountsFn counts_ = nullptr;
    ResetFn reset_ = nullptr;
};

// The upload encode through a library loaded for it alone: the functions of REPLAY_UPLOAD_FUNCS only.
class UploadLibraryImpl final : public UploadLibrary {
public:
    bool load(const std::string& path, const std::string& name, std::string* error)
    {
        name_ = name;
        if (!module_.open(path, error)) {
            return false;
        }
#define REPLAY_GET(f)                                                                              \
    api_.f = reinterpret_cast<decltype(api_.f)>(module_.sym(#f));                                  \
    if (!api_.f) {                                                                                 \
        if (error) {                                                                               \
            *error = path + " lacks " #f;                                                          \
        }                                                                                          \
        return false;                                                                              \
    }
        REPLAY_UPLOAD_FUNCS(REPLAY_GET)
        REPLAY_GET(opj_version)
#undef REPLAY_GET
        // only a request for threads needs it
        api_.opj_codec_set_threads =
            reinterpret_cast<decltype(api_.opj_codec_set_threads)>(module_.sym("opj_codec_set_threads"));
        const char* v = api_.opj_version();
        version_ = v ? v : "";
        return true;
    }

    const std::string& name() const noexcept override { return name_; }
    const char* version() const noexcept override { return version_.c_str(); }
    bool supports_threads() const noexcept override { return api_.opj_codec_set_threads != nullptr; }
    bool upload(const UploadSpec& spec, int threads, std::vector<std::uint8_t>& out) override
    {
        UploadSpec us = spec;
        Threads t{api_.opj_codec_set_threads, threads};
        if (threads > 1) {
            if (!t.set) {
                return false;  // a library without opj_codec_set_threads cannot take the request
            }
            us.configure_encoder = set_threads;
            us.configure_user = &t;
        }
        return REPLAY_NS::replay_upload_ex(api_, us, out);
    }

private:
    struct Threads {
        decltype(&::opj_codec_set_threads) set;
        int count;
    };
    static bool set_threads(void* codec, void* user)
    {
        const auto* t = static_cast<const Threads*>(user);
        return t->set(static_cast<opj_codec_t*>(codec), t->count) != 0;
    }

    std::string name_, version_;
    Module module_;
    REPLAY_NS::Api api_;
};

}  // namespace

bool oracle_upload(const UploadSpec& spec, std::vector<std::uint8_t>& out)
{
    return REPLAY_NS::replay_upload_ex(REPLAY_NS::linked_api(), spec, out);
}

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

void split_library_spec(const std::string& spec, std::string& path, std::string& name)
{
    path = spec;
    const auto eq = path.rfind('=');
    if (eq != std::string::npos && eq > 2) {
        name = path.substr(eq + 1);
        path.resize(eq);
    } else {
        name = base_name(path);
    }
}

std::unique_ptr<UploadLibrary> make_upload_library(const std::string& path, const std::string& name, std::string* error)
{
    (void)single_thread_openjpeg();  // before the library loads, as for the decoders
    auto b = std::make_unique<UploadLibraryImpl>();
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
        std::string path, name;
        split_library_spec(spec.substr(neo ? 7 : 4), path, name);
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

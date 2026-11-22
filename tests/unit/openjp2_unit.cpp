// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The openjp2 module (modules/openjp2): OpenJPEG's API over neo-j2k, loaded at run time as a viewer loads
// its openjp2 library, and called through OpenJPEG's own header. This checks what the adapter adds to the
// forwarded calls (the differential tests, `openjp2_*` in ctest, check the calls themselves):
//   - opj_version names OpenJPEG's API level and neo-j2k's release; the thread support is there;
//   - opj_codec_set_threads starts the threads it was asked for, replaces them when asked again, caps them
//     at 64, refuses a negative count and no codec, answers true for an encoder (which uses none and starts
//     none), and opj_destroy_codec joins them (the threads named "openjp2-worker" say so);
//   - a decode on any number of threads gives the oracle's samples and viewer image, at every reduce
//     level, in the plain and the viewer's mode, with several decoders running at once (the
//     ThreadSanitizer job runs it);
//   - the viewer's upload encode through the module is the oracle's, byte for byte.
// Usage: nj2k_openjp2_unit <openjp2 library> <corpus folder>
#include <openjpeg.h>

#include <neoj2k/neoj2k.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

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
#include <tlhelp32.h>
#else
#include <dlfcn.h>

#include <filesystem>
#include <fstream>
#endif

using namespace nj2k::test;

namespace {

int g_failures = 0;
#define CHECK_MSG(cond, ...)                                          \
    do {                                                              \
        if (!(cond)) {                                                \
            ++g_failures;                                             \
            std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            std::fprintf(stderr, __VA_ARGS__);                        \
            std::fprintf(stderr, "\n");                               \
        }                                                             \
    } while (0)

// the pool's threads: those named "openjp2-worker" (the module names its workers; counting every thread of
// the process would count the system's own too, such as a Windows loader's workers)
unsigned pool_threads()
{
    constexpr const char* kName = "openjp2-worker";
    unsigned n = 0;
#if defined(_WIN32)
    using GetDescription = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    auto get = kernel ? reinterpret_cast<GetDescription>(reinterpret_cast<void*>(GetProcAddress(kernel, "GetThreadDescription")))
                      : nullptr;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (!get || snap == INVALID_HANDLE_VALUE) {
        return 0;
    }
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) {
            continue;
        }
        if (HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID)) {
            PWSTR name = nullptr;
            if (SUCCEEDED(get(h, &name)) && name) {
                n += std::wstring(name) == L"openjp2-worker";
                LocalFree(name);
            }
            CloseHandle(h);
        }
    }
    CloseHandle(snap);
#else
    for (const auto& e : std::filesystem::directory_iterator("/proc/self/task")) {
        std::ifstream comm(e.path() / "comm");
        std::string name;
        std::getline(comm, name);
        n += name == kName;
    }
#endif
    (void)kName;
    return n;
}

// waits for the count to settle at expected (a joined thread may take a moment to leave the system's list)
bool threads_become(unsigned expected)
{
    for (int i = 0; i < 400; ++i) {
        if (pool_threads() == expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// the library's functions, taken from it by name as a viewer's loader would
struct Library {
    void* module = nullptr;
    decltype(&::opj_version) version = nullptr;
    decltype(&::opj_has_thread_support) has_thread_support = nullptr;
    decltype(&::opj_get_num_cpus) get_num_cpus = nullptr;
    decltype(&::opj_create_decompress) create_decompress = nullptr;
    decltype(&::opj_create_compress) create_compress = nullptr;
    decltype(&::opj_destroy_codec) destroy_codec = nullptr;
    decltype(&::opj_codec_set_threads) codec_set_threads = nullptr;

    bool load(const std::string& path)
    {
#if defined(_WIN32)
        module = LoadLibraryA(path.c_str());
        auto sym = [this](const char* s) { return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(module), s)); };
#else
        module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        auto sym = [this](const char* s) { return dlsym(module, s); };
#endif
        if (!module) {
            return false;
        }
#define GET(f) f = reinterpret_cast<decltype(f)>(sym("opj_" #f))
        GET(version);
        GET(has_thread_support);
        GET(get_num_cpus);
        GET(create_decompress);
        GET(create_compress);
        GET(destroy_codec);
        GET(codec_set_threads);
#undef GET
        return version && has_thread_support && get_num_cpus && create_decompress && create_compress &&
               destroy_codec && codec_set_threads;
    }
};

void test_identity(const Library& lib)
{
    // what a viewer prints after "Runtime:" and keys its texture cache on: five characters at most. Firestorm
    // stores "OpenJPEG: %i.%i.%i, Runtime: %s" (LLImageJ2COJ::getEngineInfo) in a 32-byte field of its
    // texture cache header and stops with a fatal error when it does not fit (v0.1.1 did not).
    const std::string got = lib.version() ? lib.version() : "(null)";
    CHECK_MSG(got == "nj2k", "opj_version is '%s', not 'nj2k'", got.c_str());
    char engine[128];
    std::snprintf(engine, sizeof engine, "OpenJPEG: %i.%i.%i, Runtime: %s", OPJ_VERSION_MAJOR, OPJ_VERSION_MINOR,
                  OPJ_VERSION_BUILD, got.c_str());
    constexpr std::size_t kFirestormHeaderField = 32;  // sHeaderEncoderStringSize in lltexturecache.h
    CHECK_MSG(std::strlen(engine) + 1 <= kFirestormHeaderField, "Firestorm's engine string '%s' is %zu characters; its cache header holds %zu",
              engine, std::strlen(engine), kFirestormHeaderField - 1);
    CHECK_MSG(lib.has_thread_support() == OPJ_TRUE, "opj_has_thread_support is false");
    CHECK_MSG(lib.get_num_cpus() >= 1, "opj_get_num_cpus is %d", lib.get_num_cpus());
    std::printf("version: %s\n", got.c_str());
}

void test_threads(const Library& lib)
{
    const unsigned base = pool_threads();
    opj_codec_t* c = lib.create_decompress(OPJ_CODEC_J2K);
    CHECK_MSG(c != nullptr, "no decoder");
    if (!c) {
        return;
    }
    CHECK_MSG(pool_threads() == base, "a decoder started threads");
    struct Case {
        int asked;
        unsigned extra;  // threads beyond the caller's: asked - 1, none below 2, at most 63
    };
    // the calling thread works too, so n threads in all are n - 1 workers; 0 and 1 run on the caller
    for (const Case k : {Case{4, 3}, Case{2, 1}, Case{0, 0}, Case{1, 0}, Case{64, 63}, Case{1000, 63}, Case{3, 2},
                         Case{0, 0}}) {
        CHECK_MSG(lib.codec_set_threads(c, k.asked) == OPJ_TRUE, "opj_codec_set_threads(%d) refused", k.asked);
        CHECK_MSG(threads_become(base + k.extra), "opj_codec_set_threads(%d): %u threads, %u expected", k.asked,
                  pool_threads(), base + k.extra);
    }
    CHECK_MSG(lib.codec_set_threads(c, 5) == OPJ_TRUE, "set_threads(5) refused");
    CHECK_MSG(lib.codec_set_threads(c, -1) == OPJ_FALSE, "a negative count was accepted");
    CHECK_MSG(threads_become(base + 4), "a refused call changed the codec's threads");
    CHECK_MSG(lib.codec_set_threads(nullptr, 4) == OPJ_FALSE, "no codec was accepted");
    lib.destroy_codec(c);
    CHECK_MSG(threads_become(base), "opj_destroy_codec left %u threads, %u expected", pool_threads(), base);

    // an encoder gets the same kind of pool; large encodes borrow it, small ones remain serial
    opj_codec_t* e = lib.create_compress(OPJ_CODEC_J2K);
    CHECK_MSG(e != nullptr, "no encoder");
    if (e) {
        CHECK_MSG(lib.codec_set_threads(e, 4) == OPJ_TRUE, "an encoder refused threads");
        CHECK_MSG(threads_become(base + 3), "an encoder did not start three workers");
        lib.destroy_codec(e);
        CHECK_MSG(threads_become(base), "destroying an encoder left workers behind");
    }
    // many codecs, each with its own pool, destroyed in another order than they were made
    opj_codec_t* many[5] = {};
    for (opj_codec_t*& m : many) {
        m = lib.create_decompress(OPJ_CODEC_J2K);
        CHECK_MSG(m && lib.codec_set_threads(m, 3) == OPJ_TRUE, "a codec of five refused its threads");
    }
    CHECK_MSG(threads_become(base + 10), "five codecs of 3 threads: %u threads", pool_threads());
    for (int i : {2, 0, 4, 1, 3}) {
        lib.destroy_codec(many[i]);
    }
    CHECK_MSG(threads_become(base), "five codecs destroyed, %u threads, %u expected", pool_threads(), base);
    lib.destroy_codec(nullptr);  // as OpenJPEG's: nothing
    std::printf("threads: started, replaced, capped and joined (pool threads at the start: %u)\n", base);
}

const char* const kLarge[] = {"large_1024_rgb_97.j2k", "large_4096_g_97.j2k", "large_2048x64_rgba_53.j2k",
                              "large_4096x8_rgb_97.j2k"};

void test_decode(const std::string& path, const std::string& folder)
{
    std::string err;
    auto lib = make_library_backend(path, "openjp2", &err);
    CHECK_MSG(lib != nullptr, "cannot load the module as a backend: %s", err.c_str());
    auto oracle = make_oracle_backend();
    if (!lib) {
        return;
    }
    std::size_t decodes = 0;
    for (const char* name : kLarge) {
        const auto file = read_file(folder + "/" + name);
        CHECK_MSG(file.has_value(), "no %s in the corpus", name);
        if (!file) {
            continue;
        }
        for (Mode mode : {Mode::Plain, Mode::Viewer}) {
            for (int reduce : {0, 1, 2}) {
                Request rq;
                rq.data = file->data();
                rq.size = file->size();
                rq.reduce = reduce;
                rq.mode = mode;
                const Outcome ref = oracle->decode(rq);
                for (int threads : {0, 1, 2, 3, 8, 64}) {
                    rq.threads = threads;
                    const Outcome got = lib->decode(rq);
                    ++decodes;
                    const bool same = got.ok == ref.ok && got.hash == ref.hash && got.u8_hash == ref.u8_hash;
                    CHECK_MSG(same, "%s, %s, reduce %d, %d threads: the module's decode differs from the oracle's",
                              name, mode == Mode::Plain ? "plain" : "viewer", reduce, threads);
                }
            }
        }
    }
    std::printf("decode: %zu decodes at 0-64 threads equal to the oracle's\n", decodes);
}

void test_concurrent(const std::string& path, const std::string& folder)
{
    const auto file = read_file(folder + "/large_1024_rgb_97.j2k");
    CHECK_MSG(file.has_value(), "no large_1024_rgb_97.j2k in the corpus");
    if (!file) {
        return;
    }
    Request rq;
    rq.data = file->data();
    rq.size = file->size();
    const Outcome ref = make_oracle_backend()->decode(rq);
    // four decoders at once, each with a pool of its own, each decoding repeatedly
    std::vector<std::thread> workers;
    std::vector<int> bad(4, 0);
    for (int w = 0; w < 4; ++w) {
        workers.emplace_back([&, w] {
            std::string err;
            auto lib = make_library_backend(path, "openjp2", &err);
            if (!lib) {
                bad[w] = -1;
                return;
            }
            Request mine = rq;
            mine.threads = 2 + w;
            for (int i = 0; i < 6; ++i) {
                const Outcome got = lib->decode(mine);
                bad[w] += !(got.ok == ref.ok && got.hash == ref.hash);
            }
        });
    }
    for (std::thread& t : workers) {
        t.join();
    }
    for (int w = 0; w < 4; ++w) {
        CHECK_MSG(bad[w] == 0, "decoder %d of 4 at once: %d wrong decodes", w, bad[w]);
    }
    std::printf("concurrent: 4 decoders with pools of 2 to 5 threads, 6 decodes each\n");
}

void test_encode(const std::string& path)
{
    std::string err;
    auto lib = make_library_backend(path, "openjp2", &err);
    auto oracle = make_oracle_backend();
    if (!lib) {
        CHECK_MSG(false, "cannot load the module as a backend: %s", err.c_str());
        return;
    }
    struct Image {
        int w, h, comps;
    };
    std::size_t encodes = 0;
    for (const Image im : {Image{64, 64, 3}, Image{131, 97, 3}, Image{50, 70, 1}, Image{33, 40, 4}}) {
        std::vector<std::uint8_t> raw(static_cast<std::size_t>(im.w) * im.h * im.comps);
        std::uint32_t s = 12345u + static_cast<std::uint32_t>(im.w);
        for (std::size_t i = 0; i < raw.size(); ++i) {
            s = s * 1664525u + 1013904223u;
            raw[i] = static_cast<std::uint8_t>((i / im.comps) % 251 + (s >> 29));
        }
        for (bool reversible : {true, false}) {
            std::vector<std::uint8_t> a, b;
            const bool ra = oracle->encode_upload(raw.data(), im.w, im.h, im.comps, reversible, a);
            const bool rb = lib->encode_upload(raw.data(), im.w, im.h, im.comps, reversible, b);
            ++encodes;
            CHECK_MSG(ra && rb && a == b, "%d x %d x %d %s: the module's encode (%zu bytes) is not the oracle's (%zu)",
                      im.w, im.h, im.comps, reversible ? "lossless" : "lossy", b.size(), a.size());
        }
    }
    std::printf("encode: %zu upload encodes equal to the oracle's, byte for byte\n", encodes);
}

}  // namespace

int main(int argc, char** argv)
{
    console_reports();
    if (argc != 3) {
        std::fprintf(stderr, "usage: nj2k_openjp2_unit <openjp2 library> <corpus folder>\n");
        return 2;
    }
    Library lib;
    if (!lib.load(argv[1])) {
        std::fprintf(stderr, "cannot load %s, or it lacks a function a viewer calls\n", argv[1]);
        return 2;
    }
    test_identity(lib);
    test_threads(lib);
    test_decode(argv[1], argv[2]);
    test_concurrent(argv[1], argv[2]);
    test_encode(argv[1]);
    return g_failures ? 1 : 0;
}

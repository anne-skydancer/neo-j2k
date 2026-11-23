// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The large-image bench (stages 1c and 5): one decode per process, so that the process's peak memory
// is that decoder's alone, against the oracle (OpenJPEG 2.5.4, built in) at the same thread count;
// and the saturated-pool run (stage 5). bench/run_large.py drives it (doc/bench.md).
//
//   nj2k_large gen <out.j2k> <w> <h> <tile|0> <lossy 0|1>
//       a three-component 8-bit image with texture-like content, by the oracle's encoder (one rate
//       layer, six resolutions, the colour transform; 9/7 at rate 20 when lossy, else 5/3)
//   nj2k_large dec <oracle|oracle-mem|neo|neo-mem|lib:PATH[=NAME]> <file> [--threads N] [--pin CORE] [--repeat R]
//       one decode: oracle and neo through a file stream, oracle-mem through a memory stream, neo-mem
//       through nj2k_decode_memory into int32 planes (the file read before the timing, as a host holds
//       it). N threads: OpenJPEG's opj_codec_set_threads(N) (its pool starts inside the timed call, as
//       a host that uses it pays), neo-j2k a pool of N - 1 workers and the caller lent as its executor
//       (started before). Prints "ms <t> peak_mb <p> hash <h> ok": the peak is the process's peak
//       commit (Windows) or peak resident set (Linux), the hash an FNV-1a over every plane.
//       lib:PATH is a library with OpenJPEG's ABI loaded by path (the shipped openjp2.dll, a shared
//       OpenJPEG build): the same memory decode through its opj_* calls only (tests/common's backend, plain
//       mode), N threads by its opj_codec_set_threads when N > 1, ms the median of R decodes (R is 1 by
//       default; every one must succeed), the hash over the geometry and every sample from two more, untimed
//       decodes that must agree; peak_mb is the peak above what the process held before its first decode, one
//       decimal. Two libraries
//       decoding one file give one hash when their samples are identical.
//   nj2k_large sat <file> <hosts> <decodes> <threads>
//       the saturated host: <hosts> threads each decode the file <decodes> times through
//       nj2k_decode_memory, each with a codec of its own; with <threads> 0 every decode is on its
//       host thread, else every codec is lent one shared pool (<threads> - 1 workers, concurrency
//       <threads>). Prints "wall_ms <t> cpu_ms <c> decodes_per_s <r> hash <h> ok".
//   nj2k_large split <file> <N> <rounds>
//       where threads start: one codec decoding the file from memory into int32 planes again and
//       again on one thread, and another through a pool of N - 1 workers and the caller with every
//       stage split however small (the library's thresholds at 0), taking turns; medians.
//   nj2k_large clear
//       the plane clear's threshold: a fresh plane of 64 KiB to 64 MiB cleared on one thread and in 8
//       bands through a pool of 7 workers and the caller, taking turns; medians.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#else
#include <sched.h>
#include <sys/resource.h>
#endif

#include <openjpeg.h>

#include <neoj2k/neoj2k.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "../src/parallel.h"  // set_parallel_thresholds (the static library's, for split)
#include "common.h"
#include "decoder.h"
#include "thread_pool.h"

namespace {

std::uint64_t fnv(std::uint64_t h, const void* p, std::size_t n)
{
    const auto* b = static_cast<const unsigned char*>(p);
    for (std::size_t i = 0; i < n; ++i) {
        h = (h ^ b[i]) * 1099511628211ull;
    }
    return h;
}
constexpr std::uint64_t kFnvStart = 1469598103934665603ull;

void quiet(const char*, void*) {}

double peak_mb()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
    return double(pmc.PeakPagefileUsage) / (1024.0 * 1024.0);
#else
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return double(ru.ru_maxrss) / 1024.0;  // KiB on Linux
#endif
}

// What the process holds now (private commit on Windows, resident set elsewhere), the baseline of a peak above it
double current_mb()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof pmc;
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc);
    return double(pmc.PrivateUsage) / (1024.0 * 1024.0);
#else
    unsigned long pages = 0, resident = 0;
    if (std::FILE* f = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(f, "%lu %lu", &pages, &resident) != 2) {
            resident = 0;
        }
        std::fclose(f);
    }
    return double(resident) * 4096.0 / (1024.0 * 1024.0);
#endif
}

double cpu_ms()
{
#if defined(_WIN32)
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    auto ms = [](const FILETIME& f) {
        return double((std::uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 10000.0;
    };
    return ms(k) + ms(u);
#else
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000.0 + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1000.0;
#endif
}

void pin(int core)
{
    if (core < 0) {
        return;
    }
#if defined(_WIN32)
    SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << core);
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    sched_setaffinity(0, sizeof set, &set);
#endif
}

void clear_opj_threads()
{
#if defined(_WIN32)
    _putenv("OPJ_NUM_THREADS=");
#else
    unsetenv("OPJ_NUM_THREADS");
#endif
}

int gen(const char* out, int w, int h, int tile, bool lossy)
{
    opj_image_cmptparm_t cp[3];
    std::memset(cp, 0, sizeof cp);
    for (auto& c : cp) {
        c.dx = c.dy = 1;
        c.w = static_cast<OPJ_UINT32>(w);
        c.h = static_cast<OPJ_UINT32>(h);
        c.prec = 8;
    }
    opj_image_t* img = opj_image_create(3, cp, OPJ_CLRSPC_SRGB);
    img->x1 = static_cast<OPJ_UINT32>(w);
    img->y1 = static_cast<OPJ_UINT32>(h);
    // texture-like content: gradients, a few frequencies, and noise
    std::uint32_t rng = 12345;
    for (int c = 0; c < 3; ++c) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                rng = rng * 1664525u + 1013904223u;
                const double v = 128 + 50 * std::sin(x * (0.013 + 0.004 * c)) * std::cos(y * 0.007) +
                                 30 * std::sin((x + y) * 0.11) + double((rng >> 24) % 24) - 12;
                img->comps[c].data[std::size_t(y) * w + x] = std::max(0, std::min(255, int(v)));
            }
        }
    }
    opj_cparameters_t p;
    opj_set_default_encoder_parameters(&p);
    p.tcp_numlayers = 1;
    p.cp_disto_alloc = 1;
    p.tcp_rates[0] = lossy ? 20.0f : 0.0f;
    p.irreversible = lossy ? 1 : 0;
    p.numresolution = 6;
    p.tcp_mct = 1;
    if (tile) {
        p.tile_size_on = OPJ_TRUE;
        p.cp_tdx = tile;
        p.cp_tdy = tile;
    }
    opj_codec_t* codec = opj_create_compress(OPJ_CODEC_J2K);
    opj_set_info_handler(codec, quiet, nullptr);
    opj_set_warning_handler(codec, quiet, nullptr);
    opj_setup_encoder(codec, &p, img);
    opj_stream_t* s = opj_stream_create_default_file_stream(out, OPJ_FALSE);
    const bool ok = s && opj_start_compress(codec, img, s) && opj_encode(codec, s) && opj_end_compress(codec, s);
    opj_stream_destroy(s);
    opj_destroy_codec(codec);
    opj_image_destroy(img);
    std::printf("%s %s\n", out, ok ? "written" : "FAILED");
    return ok ? 0 : 1;
}

std::vector<unsigned char> read_all(const char* p)
{
    std::ifstream f(p, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

struct MemoryBytes {
    const unsigned char* p;
    std::size_t n, at;
};

// The oracle's image: decode time from t0, the hash of its planes
int oracle_decode(opj_stream_t* s, int threads, std::chrono::steady_clock::time_point t0)
{
    opj_codec_t* codec = opj_create_decompress(OPJ_CODEC_J2K);
    opj_dparameters_t p;
    opj_set_default_decoder_parameters(&p);
    opj_setup_decoder(codec, &p);
    if (threads > 1 && !opj_codec_set_threads(codec, threads)) {
        std::printf("opj_codec_set_threads failed\n");
        return 1;
    }
    opj_set_warning_handler(codec, quiet, nullptr);
    opj_set_info_handler(codec, quiet, nullptr);
    opj_image_t* img = nullptr;
    const bool ok = s && opj_read_header(s, codec, &img) && opj_decode(codec, s, img) && opj_end_decompress(codec, s);
    const auto t1 = std::chrono::steady_clock::now();
    std::uint64_t hash = kFnvStart;
    for (unsigned c = 0; ok && c < img->numcomps; ++c) {
        hash = fnv(hash, img->comps[c].data, std::size_t(img->comps[c].w) * img->comps[c].h * 4);
    }
    std::printf("ms %.1f peak_mb %.0f hash %016llx %s\n", std::chrono::duration<double, std::milli>(t1 - t0).count(),
                peak_mb(), static_cast<unsigned long long>(hash), ok ? "ok" : "FAILED");
    opj_image_destroy(img);
    opj_destroy_codec(codec);
    return ok ? 0 : 1;
}

// neo-j2k from memory into int32 planes (allocated, not cleared, as a caller's own buffer): the
// time of the header read and the decode
struct NeoMemory {
    bool ok = false;
    double ms = 0;
    std::uint64_t hash = kFnvStart;
};
NeoMemory neo_memory(const std::vector<unsigned char>& bytes, const nj2k_executor_t* ex)
{
    NeoMemory r;
    const auto th0 = std::chrono::steady_clock::now();
    nj2k_codec_t* codec = nj2k_create_decompress(NJ2K_CODEC_J2K);
    if (ex) {
        nj2k_decoder_set_executor(codec, ex);
    }
    nj2k_image_t image;
    const bool header = nj2k_read_header_memory(codec, bytes.data(), bytes.size(), &image) != 0;
    const auto th1 = std::chrono::steady_clock::now();
    std::vector<std::int32_t*> planes;
    std::vector<nj2k_plane_t> pl;
    for (unsigned c = 0; header && c < image.numcomps; ++c) {
        planes.push_back(static_cast<std::int32_t*>(std::malloc(std::size_t(image.comps[c].w) * image.comps[c].h * 4)));
        pl.push_back(nj2k_plane_t{planes.back(), static_cast<NJ2K_INT64>(image.comps[c].w) * 4, 4, c});
    }
    const auto td0 = std::chrono::steady_clock::now();
    if (header) {
        const nj2k_output_t out{sizeof(nj2k_output_t), NJ2K_SAMPLE_I32, image.numcomps, pl.data()};
        r.ok = nj2k_decode_memory(codec, bytes.data(), bytes.size(), &out) != 0;
    }
    const auto td1 = std::chrono::steady_clock::now();
    for (unsigned c = 0; c < planes.size(); ++c) {
        r.hash = fnv(r.hash, planes[c], std::size_t(image.comps[c].w) * image.comps[c].h * 4);
        std::free(planes[c]);
    }
    r.ms = std::chrono::duration<double, std::milli>((th1 - th0) + (td1 - td0)).count();
    nj2k_destroy_codec(codec);
    return r;
}

// A library with OpenJPEG's ABI, loaded by path and driven through its opj_* calls from memory
int dec_library(const std::string& spec, const char* file, int threads, int repeat)
{
    const auto bytes = read_all(file);
    std::string error;
    const std::unique_ptr<nj2k::test::Backend> backend = nj2k::test::make_backend(spec, &error);
    if (!backend) {
        std::fprintf(stderr, "nj2k_large: %s\n", error.c_str());
        return 1;
    }
    nj2k::test::Request rq;
    rq.data = bytes.data();
    rq.size = bytes.size();
    rq.mode = nj2k::test::Mode::Plain;
    rq.inspect = false;
    rq.threads = threads > 1 ? threads : 0;
    std::vector<double> ms;
    bool ok = true;
    const double baseline = current_mb();
    for (int i = 0; i < repeat; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        const nj2k::test::Outcome o = backend->decode(rq);
        const auto t1 = std::chrono::steady_clock::now();
        ok = ok && o.ok;
        ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    const double peak = peak_mb() - baseline;  // above what the process held before its first decode; before the hashing decodes
    // two more decodes, untimed, hashed: they must agree with each other (the timed ones only had to succeed)
    rq.inspect = true;
    const nj2k::test::Outcome h = backend->decode(rq);
    const nj2k::test::Outcome h2 = backend->decode(rq);
    ok = ok && h.ok && h2.ok && h.hash == h2.hash;
    std::sort(ms.begin(), ms.end());
    std::printf("ms %.3f peak_mb %.1f hash %016llx %s\n", ms[ms.size() / 2], peak,
                static_cast<unsigned long long>(h.hash), ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}

int dec(const std::string& which, const char* file, int threads, int repeat)
{
    if (which.rfind("lib:", 0) == 0) {
        return dec_library(which, file, threads, repeat);
    }
    if (which == "oracle") {
        const auto t0 = std::chrono::steady_clock::now();
        opj_stream_t* s = opj_stream_create_default_file_stream(file, OPJ_TRUE);
        const int rc = oracle_decode(s, threads, t0);
        opj_stream_destroy(s);
        return rc;
    }
    if (which == "neo") {
        nj2k::test::ThreadPool pool(unsigned(threads - 1));
        const nj2k_executor_t ex = pool.executor();
        const auto t0 = std::chrono::steady_clock::now();
        nj2k_codec_t* codec = nj2k_create_decompress(NJ2K_CODEC_J2K);
        nj2k_dparameters_t p;
        nj2k_set_default_decoder_parameters(&p);
        nj2k_setup_decoder(codec, &p);
        if (threads > 1) {
            nj2k_decoder_set_executor(codec, &ex);
        }
        nj2k_stream_t* s = nj2k_stream_create_default_file_stream(file, NJ2K_TRUE);
        nj2k_image_t* img = nullptr;
        const bool ok =
            s && nj2k_read_header(s, codec, &img) && nj2k_decode(codec, s, img) && nj2k_end_decompress(codec, s);
        const auto t1 = std::chrono::steady_clock::now();
        std::uint64_t hash = kFnvStart;
        for (unsigned c = 0; ok && c < img->numcomps; ++c) {
            hash = fnv(hash, img->comps[c].data, std::size_t(img->comps[c].w) * img->comps[c].h * 4);
        }
        std::printf("ms %.1f peak_mb %.0f hash %016llx %s\n",
                    std::chrono::duration<double, std::milli>(t1 - t0).count(), peak_mb(),
                    static_cast<unsigned long long>(hash), ok ? "ok" : "FAILED");
        nj2k_image_destroy(img);
        nj2k_stream_destroy(s);
        nj2k_destroy_codec(codec);
        return ok ? 0 : 1;
    }
    // the file's bytes in memory first (outside the timing), as a host holds them
    const auto bytes = read_all(file);
    if (which == "oracle-mem") {
        MemoryBytes m{bytes.data(), bytes.size(), 0};
        const auto t0 = std::chrono::steady_clock::now();
        opj_stream_t* s = opj_stream_create(1 << 20, OPJ_TRUE);
        opj_stream_set_user_data(s, &m, nullptr);
        opj_stream_set_user_data_length(s, m.n);
        opj_stream_set_read_function(s, [](void* out, OPJ_SIZE_T n, void* u) -> OPJ_SIZE_T {
            auto* mm = static_cast<MemoryBytes*>(u);
            if (mm->at >= mm->n) {
                return static_cast<OPJ_SIZE_T>(-1);
            }
            const std::size_t k = std::min<std::size_t>(n, mm->n - mm->at);
            std::memcpy(out, mm->p + mm->at, k);
            mm->at += k;
            return k;
        });
        opj_stream_set_skip_function(s, [](OPJ_OFF_T n, void* u) -> OPJ_OFF_T {
            auto* mm = static_cast<MemoryBytes*>(u);
            const OPJ_OFF_T to = OPJ_OFF_T(mm->at) + n;
            if (to < 0 || to > OPJ_OFF_T(mm->n)) {
                mm->at = to < 0 ? 0 : mm->n;
                return -1;
            }
            mm->at = std::size_t(to);
            return n;
        });
        opj_stream_set_seek_function(s, [](OPJ_OFF_T to, void* u) -> OPJ_BOOL {
            auto* mm = static_cast<MemoryBytes*>(u);
            if (to < 0 || to > OPJ_OFF_T(mm->n)) {
                return OPJ_FALSE;
            }
            mm->at = std::size_t(to);
            return OPJ_TRUE;
        });
        const int rc = oracle_decode(s, threads, t0);
        opj_stream_destroy(s);
        return rc;
    }
    if (which == "neo-mem") {
        nj2k::test::ThreadPool pool(unsigned(threads - 1));
        const nj2k_executor_t ex = pool.executor();
        const NeoMemory r = neo_memory(bytes, threads > 1 ? &ex : nullptr);
        std::printf("ms %.1f peak_mb %.0f hash %016llx %s\n", r.ms, peak_mb(), static_cast<unsigned long long>(r.hash),
                    r.ok ? "ok" : "FAILED");
        return r.ok ? 0 : 1;
    }
    std::printf("unknown decoder %s\n", which.c_str());
    return 2;
}

int sat(const char* file, int hosts, int decodes, int threads)
{
    const auto bytes = read_all(file);
    nj2k::test::ThreadPool pool(threads > 1 ? unsigned(threads - 1) : 0u);
    const nj2k_executor_t ex = pool.executor();
    const NeoMemory ref = neo_memory(bytes, nullptr);  // the hash every decode must give (untimed)
    std::atomic<int> differ{0};
    const double c0 = cpu_ms();
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> host;
    for (int h = 0; h < hosts; ++h) {
        host.emplace_back([&] {
            for (int i = 0; i < decodes; ++i) {
                const NeoMemory r = neo_memory(bytes, threads > 1 ? &ex : nullptr);
                differ += !r.ok || r.hash != ref.hash;
            }
        });
    }
    for (std::thread& t : host) {
        t.join();
    }
    const double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const double cpu = cpu_ms() - c0;
    std::printf("wall_ms %.1f cpu_ms %.1f decodes_per_s %.2f hash %016llx %s\n", wall, cpu,
                hosts * decodes * 1000.0 / wall, static_cast<unsigned long long>(ref.hash),
                ref.ok && differ == 0 ? "ok" : "FAILED");
    return ref.ok && differ == 0 ? 0 : 1;
}

int split(const char* file, unsigned n, int rounds)
{
    const auto bytes = read_all(file);
    nj2k::test::ThreadPool pool(n > 1 ? n - 1 : 0);
    const nj2k_executor_t ex = pool.executor(n);
    nj2k_codec_t* one = nj2k_create_decompress(NJ2K_CODEC_J2K);
    nj2k_codec_t* many = nj2k_create_decompress(NJ2K_CODEC_J2K);
    nj2k_decoder_set_executor(many, &ex);
    nj2k::detail::set_parallel_thresholds(many, 0, 0, 0, 0);
    nj2k_image_t image;
    if (!nj2k_read_header_memory(one, bytes.data(), bytes.size(), &image)) {
        return 1;
    }
    std::vector<std::vector<std::int32_t>> planes(image.numcomps);
    std::vector<nj2k_plane_t> pl(image.numcomps);
    for (unsigned c = 0; c < image.numcomps; ++c) {
        planes[c].resize(std::size_t(image.comps[c].w) * image.comps[c].h);
        pl[c] = nj2k_plane_t{planes[c].data(), static_cast<NJ2K_INT64>(image.comps[c].w) * 4, 4, c};
    }
    const nj2k_output_t out{sizeof(nj2k_output_t), NJ2K_SAMPLE_I32, image.numcomps, pl.data()};
    std::vector<double> t1, tn;
    for (int r = 0; r < rounds + 3; ++r) {
        for (int k = 0; k < 2; ++k) {
            nj2k_codec_t* c = ((r + k) & 1) ? many : one;
            const auto a = std::chrono::steady_clock::now();
            if (!nj2k_decode_memory(c, bytes.data(), bytes.size(), &out)) {
                return 1;
            }
            const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - a).count();
            if (r >= 3) {  // three rounds of warm-up
                (c == one ? t1 : tn).push_back(us);
            }
        }
    }
    std::sort(t1.begin(), t1.end());
    std::sort(tn.begin(), tn.end());
    std::printf("%ux%ux%u one %.1f us, %u threads %.1f us, ratio %.2f\n", image.comps[0].w, image.comps[0].h,
                image.numcomps, t1[t1.size() / 2], n, tn[tn.size() / 2], tn[tn.size() / 2] / t1[t1.size() / 2]);
    nj2k_destroy_codec(one);
    nj2k_destroy_codec(many);
    return 0;
}

struct ClearBands {
    unsigned char* p;
    std::size_t bytes;
    unsigned parts;
};
void clear_band(void* ctx, NJ2K_UINT32 i)
{
    const auto* c = static_cast<const ClearBands*>(ctx);
    const std::size_t a = c->bytes * i / c->parts, b = c->bytes * (i + 1) / c->parts;
    std::memset(c->p + a, 0, b - a);
}

int clear()
{
    nj2k::test::ThreadPool pool(7);
    const nj2k_executor_t ex = pool.executor(8);
    std::printf("bytes, one thread us, 8 bands us, ratio\n");
    for (int lg = 16; lg <= 26; lg += 2) {
        const std::size_t bytes = std::size_t(1) << lg;
        std::vector<double> t1, tn;
        const int reps = lg >= 24 ? 60 : 300;
        for (int r = 0; r < reps; ++r) {
            for (int k = 0; k < 2; ++k) {
                const bool bands = ((r + k) & 1) != 0;
                // a fresh plane each time (first touch, as a decode's new plane)
                auto* p = static_cast<unsigned char*>(std::malloc(bytes));
                if (!p) {
                    return 1;
                }
                ClearBands c{p, bytes, 8};
                const auto a = std::chrono::steady_clock::now();
                if (bands) {
                    ex.parallel_for(ex.user, 8, clear_band, &c);
                } else {
                    std::memset(p, 0, bytes);
                }
                const auto b = std::chrono::steady_clock::now();
                const volatile unsigned char seen = p[bytes / 2];
                (void)seen;
                std::free(p);
                (bands ? tn : t1).push_back(std::chrono::duration<double, std::micro>(b - a).count());
            }
        }
        std::sort(t1.begin(), t1.end());
        std::sort(tn.begin(), tn.end());
        std::printf("%zu, %.1f, %.1f, %.2f\n", bytes, t1[t1.size() / 2], tn[tn.size() / 2],
                    tn[tn.size() / 2] / t1[t1.size() / 2]);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    clear_opj_threads();
    int threads = 1, core = -1, repeat = 1;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--threads" && i + 1 < argc) {
            threads = std::max(1, std::atoi(argv[++i]));
        } else if (a == "--repeat" && i + 1 < argc) {
            repeat = std::max(1, std::atoi(argv[++i]));
        } else if (a == "--pin" && i + 1 < argc) {
            core = std::atoi(argv[++i]);
        } else {
            args.push_back(a);
        }
    }
    if (args.size() == 6 && args[0] == "gen") {
        return gen(args[1].c_str(), std::atoi(args[2].c_str()), std::atoi(args[3].c_str()), std::atoi(args[4].c_str()),
                   std::atoi(args[5].c_str()) != 0);
    }
    if (args.size() == 3 && args[0] == "dec") {
        if (threads == 1) {
            pin(core);
        }
        return dec(args[1], args[2].c_str(), threads, repeat);
    }
    if (args.size() == 5 && args[0] == "sat") {
        return sat(args[1].c_str(), std::atoi(args[2].c_str()), std::atoi(args[3].c_str()), std::atoi(args[4].c_str()));
    }
    if (args.size() == 4 && args[0] == "split") {
        return split(args[1].c_str(), unsigned(std::max(1, std::atoi(args[2].c_str()))), std::atoi(args[3].c_str()));
    }
    if (args.size() == 1 && args[0] == "clear") {
        return clear();
    }
    std::fprintf(stderr,
                 "usage: nj2k_large gen <out> <w> <h> <tile|0> <lossy 0|1>\n"
                 "       nj2k_large dec <oracle|oracle-mem|neo|neo-mem|lib:PATH[=NAME]> <file> [--threads N] [--pin CORE] [--repeat R]\n"
                 "       nj2k_large sat <file> <hosts> <decodes> <threads>\n"
                 "       nj2k_large split <file> <N> <rounds>\n"
                 "       nj2k_large clear\n");
    return 2;
}

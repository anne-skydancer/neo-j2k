// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// nj2k_encode_bench: the viewer's upload encode (JPEG2KEncode::encode: RLCP, 64 x 64 code-blocks, 5/3
// lossless or 9/7 with rate layers by surface), timed and with its peak memory, for one encoder in one
// process, so that the peak is its own (run it once for each encoder and size: bench/encode_bench.py
// does, and compares the two). Stage 6a's report: correctness first, speed is stage 6b's.
//
//   nj2k_encode_bench --backend oracle|neoj2k|lib:<path>[=<name>] --size N [--height H] [--comps C] [--mode lossy|lossless]
//                     [--threads N] [--budget-ms 6000] [--min-iters 3] [--max-iters 200]
//
// lib:<path> runs the same sequence through an OpenJPEG-ABI shared library loaded by path (the shipped
// openjp2.dll, the openjp2 module, a shared OpenJPEG build), calling only opj_* functions through
// pointers resolved from it, plus opj_codec_set_threads when --threads > 1 (OpenJPEG's own thread pool,
// as an application that loads the library asks for it); its opj_version goes to stderr. --threads is
// refused for the linked oracle only. The checksum is an FNV-1a of the codestream: equal across
// backends when their output is byte-identical.
//
// One CSV line: backend,width,height,comps,mode,bytes,checksum,iters,median_ms,min_ms,peak_extra_mb.
// peak_extra_mb is the process's peak commit (Windows; peak resident set elsewhere) above what it held
// before its first encode, which includes the viewer's own buffers (the raw image, the OpenJPEG image
// of int32 planes, the output) that both encoders share.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

#include "common.h"
#include "decoder.h"
#include "thread_pool.h"

using namespace nj2k::test;

namespace {

struct Memory {
    double current_mb = 0, peak_mb = 0;
};

Memory memory_now()
{
    Memory m;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc;
    std::memset(&pmc, 0, sizeof(pmc));
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        m.current_mb = double(pmc.PrivateUsage) / 1048576.0;
        m.peak_mb = double(pmc.PeakPagefileUsage) / 1048576.0;
    }
#else
    if (std::FILE* f = std::fopen("/proc/self/status", "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), f)) {
            unsigned long kb = 0;
            if (std::sscanf(line, "VmRSS: %lu kB", &kb) == 1) {
                m.current_mb = double(kb) / 1024.0;
            } else if (std::sscanf(line, "VmHWM: %lu kB", &kb) == 1) {
                m.peak_mb = double(kb) / 1024.0;
            }
        }
        std::fclose(f);
    }
#endif
    return m;
}

[[noreturn]] void usage(const char* why)
{
    std::fprintf(stderr, "nj2k_encode_bench: %s\n", why);
    std::fprintf(stderr,
                 "usage: nj2k_encode_bench --backend oracle|neoj2k|lib:<path>[=<name>] --size N [--height H] [--comps C]\n"
                 "       [--mode lossy|lossless] [--threads N] [--budget-ms 6000] [--min-iters 3] [--max-iters 200]\n");
    std::exit(2);
}

bool lend_encoder(void* codec, void* user)
{
    return nj2k_encoder_set_executor(static_cast<nj2k_codec_t*>(codec),
                                     static_cast<const nj2k_executor_t*>(user)) != 0;
}

}  // namespace

int main(int argc, char** argv)
{
    console_reports();
    (void)single_thread_openjpeg();
    std::string backend;
    int width = 0, height = 0, comps = 3;
    bool reversible = false;
    int budget_ms = 6000, min_iters = 3, max_iters = 200, threads = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) {
                usage("missing value");
            }
            return argv[++i];
        };
        if (a == "--backend") {
            backend = next();
        } else if (a == "--size") {
            width = std::atoi(next());
        } else if (a == "--height") {
            height = std::atoi(next());
        } else if (a == "--comps") {
            comps = std::atoi(next());
        } else if (a == "--mode") {
            const std::string m = next();
            if (m != "lossy" && m != "lossless") {
                usage("--mode takes lossy or lossless");
            }
            reversible = m == "lossless";
        } else if (a == "--budget-ms") {
            budget_ms = std::atoi(next());
        } else if (a == "--threads") {
            threads = std::atoi(next());
        } else if (a == "--min-iters") {
            min_iters = std::atoi(next());
        } else if (a == "--max-iters") {
            max_iters = std::atoi(next());
        } else {
            usage("unknown option");
        }
    }
    const bool library = backend.rfind("lib:", 0) == 0;
    if ((backend != "oracle" && backend != "neoj2k" && !library) || width < 4 || comps < 1 || comps > 8 ||
        threads < 1 || threads > 64 || (backend == "oracle" && threads != 1)) {
        usage("--backend and --size are needed");
    }
    std::unique_ptr<UploadLibrary> lib;
    std::string label = backend;
    if (library) {
        std::string path, error;
        split_library_spec(backend.substr(4), path, label);
        lib = make_upload_library(path, label, &error);
        if (!lib) {
            std::fprintf(stderr, "nj2k_encode_bench: %s\n", error.c_str());
            return 1;
        }
        std::fprintf(stderr, "nj2k_encode_bench: %s is %s, opj_version %s\n", label.c_str(), path.c_str(),
                     lib->version());
    }
    if (height < 4) {
        height = width;
    }

    // a texture: gradients, a few edges and noise, 8-bit interleaved, bottom-up as the viewer's
    std::vector<std::uint8_t> raw(std::size_t(width) * height * comps);
    std::uint32_t x_state = 0x2545F491u;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < comps; ++c) {
                x_state ^= x_state << 13;
                x_state ^= x_state >> 17;
                x_state ^= x_state << 5;
                const int edge = ((x / 97 + y / 61 + c) & 1) * 60;
                const int v = (x * (c + 1) / 9 + y / (c + 2) + edge + static_cast<int>(x_state % 23)) & 0xFF;
                raw[(std::size_t(y) * width + x) * comps + c] = static_cast<std::uint8_t>(v);
            }
        }
    }
    UploadSpec us;
    us.raw = raw.data();
    us.width = width;
    us.height = height;
    us.comps = comps;
    us.reversible = reversible;
    std::unique_ptr<ThreadPool> pool;
    nj2k_executor_t executor = {};
    if (threads > 1 && !lib) {
        pool =std::make_unique<ThreadPool>(static_cast<unsigned>(threads - 1));
        executor = pool->executor(static_cast<std::uint32_t>(threads));
        us.configure_encoder = lend_encoder;
        us.configure_user = &executor;
    }
    auto encode = [&](std::vector<std::uint8_t>& out) {
        if (lib) {
            return lib->upload(us, threads, out);
        }
        return backend == "oracle" ? oracle_upload(us, out) : neoj2k_upload(us, out);
    };

    const Memory before = memory_now();
    std::vector<std::uint8_t> out;
    if (!encode(out)) {  // the first encode also warms the caches
        std::fprintf(stderr, "nj2k_encode_bench: the encoder refused %dx%d\n", width, height);
        return 1;
    }
    const std::size_t bytes = out.size();
    Fnv64 hash;
    hash.bytes(out.data(), out.size());
    std::vector<double> times;
    const auto start = std::chrono::steady_clock::now();
    while (int(times.size()) < max_iters) {
        const auto t0 = std::chrono::steady_clock::now();
        (void)encode(out);
        const auto t1 = std::chrono::steady_clock::now();
        times.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        if (int(times.size()) >= min_iters &&
            std::chrono::duration<double, std::milli>(t1 - start).count() >= double(budget_ms)) {
            break;
        }
    }
    std::sort(times.begin(), times.end());
    const double median = times[times.size() / 2];
    const Memory after = memory_now();
    std::printf("%s,%d,%d,%d,%s,%zu,%s,%zu,%.3f,%.3f,%.1f\n", label.c_str(), width, height, comps,
                reversible ? "lossless" : "lossy", bytes, hex64(hash.value()).c_str(), times.size(), median,
                times.front(), after.peak_mb - before.current_mb);
    return 0;
}

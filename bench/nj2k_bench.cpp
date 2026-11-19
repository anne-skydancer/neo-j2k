// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// nj2k_bench: the viewer's request, timed, with every decoder under test interleaved.
//
//   nj2k_bench --a SPEC [--a SPEC ...] --files LIST [--mode viewer|plain] [--reduce 0-5]
//              [--kinds full,budget,first] [--encode lossy,lossless] [--budget-min 15]
//              [--target-ms 20] [--min-iters 5] [--max-iters 2000] [--pin CORE] [--csv FILE]
//              [--json FILE]
//
// Rules (plan audit C3; the bench rules of the literature report section 5):
// - Every configuration (file, kind, reduce level) runs all decoders in turn, request by request,
//   the order rotating every round, so they share the machine's load; the result to gate on is the
//   ratio to the first decoder, per file, then its median per class, with allocation counts.
// - The run is time-boxed (--budget-min, 15 by default) and never waits for a quiet machine: it
//   records the load it ran under (total and other processes' CPU, and the compilers, linkers and
//   other benches it saw) and says when the box cut it short.
// - Clock: the TSC, calibrated against steady_clock at start (nominal cycles, not core cycles).
// - Decoders: oracle (linked), lib:<path>[=<name>] (OpenJPEG ABI: the shipped DLL, the counting
//   oracle module, which adds heap-call counts), neoj2k (from stage 1c). Each needs its own name.
// - Timed rounds do the viewer's work only: the request, its checks and its 8-bit copy. The
//   identity checksums and the heap counts come from untimed rounds before them.
// - OpenJPEG runs on the calling thread: OPJ_NUM_THREADS is cleared at start (and its old value
//   recorded), since OpenJPEG would start a thread pool for it.
// Exit status: 0, or 2 on a usage error. Timing never fails a run; the gates read the CSV.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#else
#include <pthread.h>
#include <sched.h>
#include <x86intrin.h>
#endif

#include "common.h"
#include "decoder.h"
#include "j2k_scan.h"
#include "load.h"

using namespace nj2k::test;

namespace {

struct Args {
    std::vector<std::string> backends;
    std::string files, csv, json;
    Mode mode = Mode::Viewer;
    std::vector<int> reduce{0, 1, 2, 3, 4, 5};
    std::vector<std::string> kinds{"full"};
    std::vector<std::string> encode;
    double budget_min = 15, target_ms = 20;
    int min_iters = 5, max_iters = 2000, pin = -1;
};

[[noreturn]] void usage(const std::string& msg)
{
    std::fprintf(stderr, "nj2k_bench: %s\nsee the comment at the top of bench/nj2k_bench.cpp\n", msg.c_str());
    std::exit(2);
}

double g_tsc_per_us = 0;

std::uint64_t tsc() noexcept
{
    return __rdtsc();
}

void calibrate()
{
    const auto t0 = std::chrono::steady_clock::now();
    const std::uint64_t c0 = tsc();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(200)) {
    }
    const std::uint64_t c1 = tsc();
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    g_tsc_per_us = double(c1 - c0) / us;
}

void pin_thread(int core)
{
    if (core < 0) {
        return;
    }
#if defined(_WIN32)
    SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << core);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#endif
}

double median(std::vector<double> v)
{
    if (v.empty()) {
        return 0;
    }
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

double percentile(std::vector<double> v, double p)
{
    if (v.empty()) {
        return 0;
    }
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, std::size_t(p * double(v.size() - 1) + 0.5))];
}

struct Input {
    std::string name;
    Bytes data;
    int w = 0, h = 0, comps = 0, resolutions = 0;
    int grid_w = 0, grid_h = 0;  // the size the viewer stores and budgets with: the tile grid's
};

std::string size_class(int w, int h)
{
    const int m = std::max(w, h);
    for (int c : {32, 64, 128, 256, 512, 1024}) {
        if (m <= c) {
            return std::to_string(c);
        }
    }
    return ">1024";
}

struct Row {
    std::string backend, file, kind, cls;
    int w = 0, h = 0, comps = 0, reduce = 0;
    std::size_t bytes = 0;
    int iters = 0;
    bool ok = false;
    double med_us = 0, p10_us = 0, p90_us = 0;
    double allocs = -1, reallocs = -1, frees = -1, alloc_bytes = -1;
    std::uint64_t checksum = 0;
};

}  // namespace

int main(int argc, char** argv)
{
    console_reports();
    const std::string opj_threads = single_thread_openjpeg();  // before any codec exists
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage("missing value after " + k);
            }
            return argv[++i];
        };
        if (k == "--a") a.backends.push_back(next());
        else if (k == "--files") a.files = next();
        else if (k == "--csv") a.csv = next();
        else if (k == "--json") a.json = next();
        else if (k == "--reduce") a.reduce = parse_int_list(next());
        else if (k == "--kinds") a.kinds = split(next(), ',');
        else if (k == "--encode") a.encode = split(next(), ',');
        else if (k == "--budget-min") a.budget_min = std::stod(next());
        else if (k == "--target-ms") a.target_ms = std::stod(next());
        else if (k == "--min-iters") a.min_iters = std::stoi(next());
        else if (k == "--max-iters") a.max_iters = std::stoi(next());
        else if (k == "--pin") a.pin = std::stoi(next());
        else if (k == "--mode") {
            if (!parse_mode(next(), a.mode)) {
                usage("unknown mode");
            }
        } else {
            usage("unknown option " + k);
        }
    }
    if (a.backends.empty() || a.files.empty()) {
        usage("needs --a (one or more) and --files");
    }
    std::vector<std::unique_ptr<Backend>> decs;
    for (const auto& spec : a.backends) {
        std::string err;
        auto b = make_backend(spec, &err);
        if (!b) {
            usage(err);
        }
        for (const auto& d : decs) {
            if (d->name() == b->name()) {
                // rows are kept by decoder name: two of one name would merge
                usage("two decoders named '" + b->name() + "': name them (lib:<path>=<name>)");
            }
        }
        decs.push_back(std::move(b));
    }
    std::vector<Input> inputs;
    for (const auto& path : expand_inputs(a.files)) {
        auto data = read_file(path);
        const auto mh = data ? parse_main_header(*data) : std::nullopt;
        if (!mh) {
            std::fprintf(stderr, "nj2k_bench: skip %s (not a codestream)\n", path.c_str());
            continue;
        }
        Input in;
        in.name = base_name(path);
        in.data = std::move(*data);
        in.w = int(mh->width());
        in.h = int(mh->height());
        in.grid_w = viewer::grid_side(mh->tiles_x(), mh->xtsiz);
        in.grid_h = viewer::grid_side(mh->tiles_y(), mh->ytsiz);
        in.comps = mh->csiz;
        in.resolutions = mh->min_resolutions;
        inputs.push_back(std::move(in));
    }
    if (inputs.empty()) {
        usage("no codestreams");
    }
    pin_thread(a.pin);
    calibrate();

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                      std::chrono::duration<double>(a.budget_min * 60.0));
    LoadSampler sampler;
    std::vector<LoadSample> loads;
    std::vector<std::string> seen;
    auto note_load = [&] {
        loads.push_back(sampler.sample());
        for (const auto& p : busy_processes()) {
            if (std::find(seen.begin(), seen.end(), p) == seen.end()) {
                seen.push_back(p);
            }
        }
    };
    auto last_note = std::chrono::steady_clock::now();

    std::vector<Row> rows;
    std::size_t configs = 0, configs_total = 0;
    bool boxed = false;
    const std::uint64_t target = std::uint64_t(a.target_ms * 1000.0 * g_tsc_per_us);
    const std::size_t D = decs.size();

    // decode configurations
    struct Config {
        const Input* in;
        std::string kind;
        int reduce;
        std::size_t bytes;
    };
    std::vector<Config> cfgs;
    for (const auto& in : inputs) {
        for (int r : a.reduce) {
            if (r >= std::max(in.resolutions, 1)) {
                continue;
            }
            for (const auto& kind : a.kinds) {
                std::size_t n = 0;
                if (kind == "full") {
                    n = in.data.size();
                } else if (kind == "budget") {
                    const auto b = std::size_t(viewer::data_size_j2c(in.grid_w, in.grid_h, r));
                    if (r == 0 || b >= in.data.size()) {
                        continue;
                    }
                    n = b;
                } else if (kind == "first") {
                    if (r != 5 || in.data.size() <= viewer::kFirstRequestBytes) {
                        continue;
                    }
                    n = viewer::kFirstRequestBytes;
                } else {
                    usage("unknown kind " + kind);
                }
                cfgs.push_back({&in, kind, r, n});
            }
        }
    }
    configs_total = cfgs.size();
    for (const auto& cfg : cfgs) {
        if (std::chrono::steady_clock::now() >= deadline) {
            boxed = true;
            break;
        }
        Request rq;
        rq.data = cfg.in->data.data();
        rq.size = cfg.bytes;
        rq.reduce = cfg.reduce;
        rq.mode = a.mode;
        std::vector<std::vector<double>> times(D);
        std::vector<std::uint64_t> spent(D, 0);
        std::vector<Row> r(D);
        // untimed rounds: the identity checksum (the first call may also set lasting state up),
        // then the heap calls of one request as the timed rounds make it
        Request timed = rq;
        timed.inspect = false;
        for (std::size_t k = 0; k < D; ++k) {
            const Outcome o = decs[k]->decode(rq);
            r[k].ok = o.ok;
            r[k].checksum = o.ok ? (a.mode == Mode::Viewer ? o.u8_hash : o.hash) : 0;
            std::uint64_t counts[4];
            decs[k]->alloc_reset();
            (void)decs[k]->decode(timed);
            if (decs[k]->alloc_counts(counts)) {
                r[k].allocs = double(counts[0]);
                r[k].reallocs = double(counts[1]);
                r[k].frees = double(counts[2]);
                r[k].alloc_bytes = double(counts[3]);
            }
        }
        int it = 0;
        for (; it < a.max_iters; ++it) {
            bool done = it >= a.min_iters;
            for (std::size_t k = 0; k < D && done; ++k) {
                done = spent[k] >= target;
            }
            if (done) {
                break;
            }
            for (std::size_t j = 0; j < D; ++j) {
                const std::size_t k = (j + std::size_t(it)) % D;  // rotate who goes first
                const std::uint64_t t0 = tsc();
                (void)decs[k]->decode(timed);
                const std::uint64_t t1 = tsc();
                spent[k] += t1 - t0;
                times[k].push_back(double(t1 - t0) / g_tsc_per_us);
            }
        }
        for (std::size_t k = 0; k < D; ++k) {
            r[k].backend = decs[k]->name();
            r[k].file = cfg.in->name;
            r[k].kind = cfg.kind;
            r[k].cls = size_class(cfg.in->w, cfg.in->h);
            r[k].w = cfg.in->w;
            r[k].h = cfg.in->h;
            r[k].comps = cfg.in->comps;
            r[k].reduce = cfg.reduce;
            r[k].bytes = cfg.bytes;
            r[k].iters = it;
            r[k].med_us = median(times[k]);
            r[k].p10_us = percentile(times[k], 0.1);
            r[k].p90_us = percentile(times[k], 0.9);
            rows.push_back(r[k]);
        }
        ++configs;
        if (std::chrono::steady_clock::now() - last_note > std::chrono::seconds(2)) {
            note_load();
            last_note = std::chrono::steady_clock::now();
        }
    }

    // upload encodes: the decoded whole file at reduce 0 is the upload source
    for (const auto& kind : a.encode) {
        const bool reversible = kind == "lossless";
        if (!reversible && kind != "lossy") {
            usage("--encode takes lossy and/or lossless");
        }
        for (const auto& in : inputs) {
            if (std::chrono::steady_clock::now() >= deadline) {
                boxed = true;
                break;
            }
            ++configs_total;
            Request rq;
            rq.data = in.data.data();
            rq.size = in.data.size();
            rq.capture = true;
            const Outcome src = decs[0]->decode(rq);
            if (!src.ok || src.numcomps < 1 || src.numcomps > 4) {
                continue;
            }
            const int w = int(src.comps[0].w), h = int(src.comps[0].h), c = int(src.numcomps);
            bool same_size = true;
            for (const auto& comp : src.comps) {
                same_size = same_size && int(comp.w) == w && int(comp.h) == h;
            }
            if (!same_size) {
                continue;
            }
            std::vector<std::uint8_t> raw(std::size_t(w) * h * c);
            for (int k = 0; k < c; ++k) {  // the viewer's raw image: interleaved, bottom row first
                for (int y = 0; y < h; ++y) {
                    for (int x = 0; x < w; ++x) {
                        raw[(std::size_t(h - 1 - y) * w + x) * c + k] =
                            std::uint8_t(src.planes[std::size_t(k)][std::size_t(y) * w + x]);
                    }
                }
            }
            std::vector<std::vector<double>> times(D);
            std::vector<std::uint64_t> spent(D, 0);
            std::vector<Row> r(D);
            std::vector<std::uint8_t> out;
            for (std::size_t k = 0; k < D; ++k) {
                r[k].ok = decs[k]->encode_upload(raw.data(), w, h, c, reversible, out);
                Fnv64 hsh;
                hsh.bytes(out.data(), out.size());
                r[k].checksum = r[k].ok ? hsh.value() : 0;
                r[k].bytes = out.size();
            }
            const double enc_target = double(target) * 5;  // encodes are slower: fewer, longer rounds
            int it = 0;
            for (; it < std::max(1, a.max_iters / 20); ++it) {
                bool done = it >= std::max(1, a.min_iters / 2);
                for (std::size_t k = 0; k < D && done; ++k) {
                    done = double(spent[k]) >= enc_target;
                }
                if (done) {
                    break;
                }
                for (std::size_t j = 0; j < D; ++j) {
                    const std::size_t k = (j + std::size_t(it)) % D;
                    const std::uint64_t t0 = tsc();
                    (void)decs[k]->encode_upload(raw.data(), w, h, c, reversible, out);
                    const std::uint64_t t1 = tsc();
                    spent[k] += t1 - t0;
                    times[k].push_back(double(t1 - t0) / g_tsc_per_us);
                }
            }
            for (std::size_t k = 0; k < D; ++k) {
                r[k].backend = decs[k]->name();
                r[k].file = in.name;
                r[k].kind = "encode-" + kind;
                r[k].cls = size_class(w, h);
                r[k].w = w;
                r[k].h = h;
                r[k].comps = c;
                r[k].iters = it;
                r[k].med_us = median(times[k]);
                r[k].p10_us = percentile(times[k], 0.1);
                r[k].p90_us = percentile(times[k], 0.9);
                rows.push_back(r[k]);
            }
            ++configs;
            if (std::chrono::steady_clock::now() - last_note > std::chrono::seconds(2)) {
                note_load();
                last_note = std::chrono::steady_clock::now();
            }
        }
    }
    note_load();
    const double wall_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    if (!a.csv.empty()) {
        if (std::FILE* f = std::fopen(a.csv.c_str(), "w")) {
            std::fprintf(f, "backend,file,class,w,h,comps,kind,bytes,reduce,iters,ok,checksum,median_us,p10_us,p90_us,"
                            "allocs,reallocs,frees,alloc_bytes\n");
            for (const auto& r : rows) {
                std::fprintf(f, "%s,%s,%s,%d,%d,%d,%s,%zu,%d,%d,%d,%s,%.3f,%.3f,%.3f,%.0f,%.0f,%.0f,%.0f\n",
                             r.backend.c_str(), r.file.c_str(), r.cls.c_str(), r.w, r.h, r.comps, r.kind.c_str(), r.bytes,
                             r.reduce, r.iters, int(r.ok), hex64(r.checksum).c_str(), r.med_us, r.p10_us, r.p90_us,
                             r.allocs, r.reallocs, r.frees, r.alloc_bytes);
            }
            std::fclose(f);
        }
    }

    // summary per class and kind: median request time per backend, the median per-file ratio to the
    // first backend (taken in the same rounds), outputs identical to the first backend, heap calls
    std::vector<double> other, total;
    for (const auto& l : loads) {
        other.push_back(l.other());
        total.push_back(l.total);
    }
    const double other_mean = other.empty() ? 0 : std::accumulate(other.begin(), other.end(), 0.0) / double(other.size());
    std::printf("nj2k_bench: %zu of %zu configurations in %.1f s%s; %s mode; TSC %.1f MHz; pinned %s; "
                "OPJ_NUM_THREADS %s\n",
                configs, configs_total, wall_s, boxed ? " (time-boxed: stopped at the budget)" : "", mode_name(a.mode),
                g_tsc_per_us, a.pin >= 0 ? std::to_string(a.pin).c_str() : "no",
                opj_threads.empty() ? "unset" : ("cleared (was " + opj_threads + ")").c_str());
    std::printf("load while running: other processes %.0f %% mean, %.0f %% p90, %.0f %% max of %d CPUs; seen: %s\n",
                other_mean, percentile(other, 0.9), other.empty() ? 0.0 : *std::max_element(other.begin(), other.end()),
                logical_cpus(), seen.empty() ? "none" : [&] {
                    std::string s;
                    for (const auto& p : seen) {
                        s += (s.empty() ? "" : ", ") + p;
                    }
                    return s;
                }().c_str());
    std::map<std::string, std::map<std::string, std::vector<const Row*>>> by;  // class|kind -> backend -> rows
    for (const auto& r : rows) {
        // one class per kind, size and reduce level: a 1024 px texture at reduce 5 is a small request
        by[r.kind + " " + r.cls + " r" + std::to_string(r.reduce)][r.backend].push_back(&r);
    }
    std::printf("%-28s %-14s %6s %10s %10s %8s %9s %9s\n", "kind class reduce", "backend", "files", "median us",
                "ratio", "same", "allocs", "frees");
    std::string json = "{\n  \"configurations\": " + std::to_string(configs) + ",\n  \"configurations_planned\": " +
                       std::to_string(configs_total) + ",\n  \"time_boxed\": " + (boxed ? "true" : "false") +
                       ",\n  \"wall_seconds\": " + std::to_string(wall_s) + ",\n  \"clock\": \"TSC calibrated against steady_clock\"" +
                       ",\n  \"tsc_mhz\": " + std::to_string(g_tsc_per_us) + ",\n  \"load_other_mean\": " +
                       std::to_string(other_mean) + ",\n  \"load_other_p90\": " + std::to_string(percentile(other, 0.9)) +
                       ",\n  \"logical_cpus\": " + std::to_string(logical_cpus()) +
                       ",\n  \"opj_num_threads_cleared\": \"" + opj_threads + "\"" + ",\n  \"classes\": [\n";
    bool first_json = true;
    for (const auto& [key, per] : by) {
        static const std::vector<const Row*> none;
        const auto base_it = per.find(decs[0]->name());
        const auto& base = base_it != per.end() ? base_it->second : none;
        for (const auto& dec : decs) {
            const auto it = per.find(dec->name());
            if (it == per.end()) {
                continue;
            }
            std::vector<double> med, ratio, allocs, frees;
            std::size_t same = 0;
            for (const Row* r : it->second) {
                med.push_back(r->med_us);
                const Row* br = nullptr;  // the first backend's row of the same configuration
                for (const Row* x : base) {
                    if (x->file == r->file && x->reduce == r->reduce && x->kind == r->kind) {
                        br = x;
                    }
                }
                if (br && br->med_us > 0) {
                    ratio.push_back(r->med_us / br->med_us);
                    same += r->checksum == br->checksum && r->ok == br->ok;
                }
                if (r->allocs >= 0) {
                    allocs.push_back(r->allocs);
                    frees.push_back(r->frees);
                }
            }
            std::printf("%-28s %-14s %6zu %10.2f %10.3f %8zu %9s %9s\n", key.c_str(), dec->name().c_str(), it->second.size(),
                        median(med), median(ratio), same, allocs.empty() ? "-" : std::to_string(long(median(allocs))).c_str(),
                        frees.empty() ? "-" : std::to_string(long(median(frees))).c_str());
            json += std::string(first_json ? "" : ",\n") + "    {\"class\": \"" + key + "\", \"backend\": \"" + dec->name() +
                    "\", \"files\": " + std::to_string(it->second.size()) + ", \"median_us\": " + std::to_string(median(med)) +
                    ", \"ratio_to_first\": " + std::to_string(median(ratio)) + ", \"identical_to_first\": " +
                    std::to_string(same) + ", \"allocs_median\": " + (allocs.empty() ? "null" : std::to_string(median(allocs))) + "}";
            first_json = false;
        }
    }
    json += "\n  ]\n}\n";
    if (!a.json.empty()) {
        write_file(a.json, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(json.data()), json.size()));
    }
    return 0;
}

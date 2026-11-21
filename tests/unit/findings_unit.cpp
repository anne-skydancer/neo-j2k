// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// Proof tests for two findings of the Rust twin's plan (2026-11-20, section 10.1), each held as
// the behaviour the library has today, so that a change to it has to change the expectation here:
//
// --c2 <dir>[,<dir>...] --expect history|none
//   C2: whether a decode at the memory limit depends on the codec's history. The work arena's
//   high-water mark counts whole earlier blocks (their unused tails and 64-byte headers included),
//   and a codec that decoded before holds those blocks merged into one, so a fresh codec's mark is
//   higher than a warmed codec's for the same decode, and the tile check (samples plus that mark
//   against max_memory) could refuse on a fresh codec what a warmed one decodes. For every input
//   (the folders' codestreams, and the decode fuzz target's inputs without their option bytes) at
//   reduce 0-5, the smallest max_memory at which a fresh codec decodes is found by bisection; at
//   one byte less, the fresh codec must refuse with NJ2K_E_LIMIT, and a warmed codec (one that
//   decoded the same bytes under a 2 GiB limit before) is tried. "history": some input decodes on
//   the warmed codec where the fresh one refuses; "none": none does. Today: none. The decode's
//   working-memory estimate (or a tile's estimate with its own coding) refuses first at every
//   boundary of the corpus, the fuzz seeds, regressions and bombs (311 cases), so the mark's
//   history never reaches a status; CTest runs the corpus and the regressions (fuzz/bombs, which
//   ask for up to 1.3 GB, by hand).
//
// --c3 <dir> --expect differ|same
//   C3: whether the host's workers' floating-point environment reaches the samples. Every 9/7 file
//   of the folder up to 2^20 samples decodes on one thread, then with every stage forced through
//   an executor whose jobs all run on one worker thread that set MXCSR first: to round toward
//   zero, to flush-to-zero with denormals-are-zero, and to both. "differ": some samples differ
//   (the executor contract must state the environment, or the jobs must set it); "same": no
//   sample differs. Today: differ. Round toward zero changes about half the samples of every 9/7
//   file (all 28 of the corpus, the two larger ones too when run without the cap); flush-to-zero
//   with denormals-are-zero alone changes none of the corpus.
#include <neoj2k/neoj2k.h>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <xmmintrin.h>

#include "common.h"

// the library's internals (by their path: src/ is no include directory)
#include "../../src/parallel.h"

namespace {

using nj2k::test::Bytes;

int g_failures = 0;
#define CHECK_MSG(cond, ...)                                       \
    do {                                                           \
        if (!(cond)) {                                             \
            ++g_failures;                                          \
            std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            std::fprintf(stderr, __VA_ARGS__);                     \
            std::fprintf(stderr, "\n");                            \
        }                                                          \
    } while (0)

struct Decoded {
    bool ok = false;
    nj2k_status_t status = NJ2K_OK;
    std::vector<std::int32_t> samples;
    std::string errors;  // the error messages, oldest first
};

void collect_error(const char* m, void* user)
{
    *static_cast<std::string*>(user) += m ? m : "";
}

// A memory codec, non-strict, at the reduce factor; error messages into *errors when given.
nj2k_codec_t* make_codec(int reduce = 0, std::string* errors = nullptr)
{
    nj2k_codec_t* codec = nj2k_create_decompress(NJ2K_CODEC_J2K);
    if (!codec) {
        return nullptr;
    }
    if (errors) {
        nj2k_set_error_handler(codec, collect_error, errors);
    }
    nj2k_dparameters_t p;
    nj2k_set_default_decoder_parameters(&p);
    p.cp_reduce = static_cast<NJ2K_UINT32>(reduce);
    if (!nj2k_setup_decoder(codec, &p) || !nj2k_decoder_set_strict_mode(codec, NJ2K_FALSE)) {
        nj2k_destroy_codec(codec);
        return nullptr;
    }
    return codec;
}

// max_memory alone (0: no limit), every other limit off
bool set_max_memory(nj2k_codec_t* codec, std::uint64_t max_memory)
{
    nj2k_decoder_limits_t limits;
    nj2k_decoder_limits_init(&limits, NJ2K_DECODER_LIMITS_NONE);
    limits.max_memory = max_memory;
    return nj2k_decoder_set_limits(codec, &limits) != NJ2K_FALSE;
}

// The header from memory, planar I32 planes, the decode.
Decoded decode(nj2k_codec_t* codec, std::span<const std::uint8_t> data)
{
    Decoded d;
    nj2k_image_t image;
    if (!nj2k_read_header_memory(codec, data.data(), data.size(), &image)) {
        d.status = nj2k_get_status(codec);
        return d;
    }
    std::vector<std::size_t> offset(image.numcomps);
    std::size_t total = 0;
    for (std::uint32_t c = 0; c < image.numcomps; ++c) {
        offset[c] = total;
        total += std::size_t(image.comps[c].w) * image.comps[c].h;
    }
    d.samples.assign(total ? total : 1, 0);
    std::vector<nj2k_plane_t> planes(image.numcomps);
    for (std::uint32_t c = 0; c < image.numcomps; ++c) {
        planes[c] = nj2k_plane_t{d.samples.data() + offset[c], std::int64_t(image.comps[c].w) * 4, 4, c};
    }
    const nj2k_output_t out{sizeof(nj2k_output_t), NJ2K_SAMPLE_I32, image.numcomps, planes.data()};
    d.ok = nj2k_decode_memory(codec, data.data(), data.size(), &out) != NJ2K_FALSE;
    d.status = nj2k_get_status(codec);
    return d;
}

// ---- C2 ----------------------------------------------------------------------------------------

struct Limited {
    nj2k_status_t status = NJ2K_E_NO_MEMORY;
    std::string errors;
};

// A fresh codec under the limit.
Limited fresh(std::span<const std::uint8_t> data, int reduce, std::uint64_t max_memory)
{
    Limited l;
    nj2k_codec_t* codec = make_codec(reduce, &l.errors);
    if (codec && set_max_memory(codec, max_memory)) {
        l.status = decode(codec, data).status;
    }
    if (codec) {
        nj2k_destroy_codec(codec);
    }
    return l;
}

// A codec that decoded the same bytes under the warm-up limit before, then the limit, then the
// decode (a codec reading from memory takes new limits between decodes).
Limited warmed(std::span<const std::uint8_t> data, int reduce, std::uint64_t warm_limit, std::uint64_t max_memory)
{
    Limited l;
    std::string warm_errors;
    nj2k_codec_t* codec = make_codec(reduce, &warm_errors);
    if (codec && set_max_memory(codec, warm_limit)) {
        decode(codec, data);
        nj2k_set_error_handler(codec, collect_error, &l.errors);
        if (set_max_memory(codec, max_memory)) {
            l.status = decode(codec, data).status;
        }
    }
    if (codec) {
        nj2k_destroy_codec(codec);
    }
    return l;
}

bool decodes(nj2k_status_t st)
{
    return st == NJ2K_OK || st == NJ2K_TRUNCATED;
}

// Which of the decoder's memory checks refused, from its message.
const char* refusal(const std::string& errors)
{
    if (errors.find("The tile's samples need") != std::string::npos) {
        return "tile samples and held structures (decode.cpp, the high-water check)";
    }
    if (errors.find("with its own coding") != std::string::npos) {
        return "a tile's own coding estimate";
    }
    if (errors.find("bytes of working memory") != std::string::npos) {
        return "the decode's working-memory estimate";
    }
    return errors.empty() ? "no message" : "another check";
}

// The corpus files and the fuzz inputs, codestreams only (a fuzz input's four option bytes dropped).
std::vector<std::pair<std::string, Bytes>> c2_inputs(const std::vector<std::string>& dirs)
{
    std::vector<std::pair<std::string, Bytes>> out;
    for (const std::string& dir : dirs) {
        for (const std::string& path : nj2k::test::expand_inputs(dir)) {
            if (auto data = nj2k::test::read_file(path)) {
                out.emplace_back(nj2k::test::base_name(path), std::move(*data));
            }
        }
        // the decode target's inputs with option bytes (fuzz/fuzz_common.h)
        std::error_code ec;
        std::vector<std::string> bins;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            const std::string n = e.path().filename().string();
            if (e.is_regular_file() && n.rfind("decode-", 0) == 0 && e.path().extension() == ".bin") {
                bins.push_back(e.path().string());
            }
        }
        std::sort(bins.begin(), bins.end());
        for (const std::string& path : bins) {
            auto data = nj2k::test::read_file(path);
            if (data && data->size() > 4 && !((*data)[0] == 0xFF && (*data)[1] == 0x4F)) {
                out.emplace_back(nj2k::test::base_name(path), Bytes(data->begin() + 4, data->end()));
            }
        }
    }
    return out;
}

int run_c2(const std::vector<std::string>& dirs, bool expect_history)
{
    // A ceiling for the bisection and the warm-up: the bombs ask for more than any host would lend.
    constexpr std::uint64_t kCeiling = std::uint64_t(1) << 31;
    std::size_t cases = 0, history = 0;
    std::vector<std::string> deciders;
    for (const auto& [name, data] : c2_inputs(dirs)) {
        if (data.size() > (1u << 20)) {
            continue;
        }
        for (int reduce = 0; reduce <= 5; ++reduce) {
            const std::span<const std::uint8_t> bytes(data.data(), data.size());
            if (!decodes(fresh(bytes, reduce, kCeiling).status) || decodes(fresh(bytes, reduce, 1).status)) {
                continue;
            }
            // the smallest limit a fresh codec decodes at: lo refuses, hi decodes
            std::uint64_t lo = 1, hi = kCeiling;
            while (hi - lo > 1) {
                const std::uint64_t mid = lo + (hi - lo) / 2;
                (decodes(fresh(bytes, reduce, mid).status) ? hi : lo) = mid;
            }
            ++cases;
            const Limited f = fresh(bytes, reduce, lo);
            const Limited w = warmed(bytes, reduce, kCeiling, lo);
            const std::string decider = refusal(f.errors);
            if (std::find(deciders.begin(), deciders.end(), decider) == deciders.end()) {
                deciders.push_back(decider);
                std::printf("C2 %s r%d: the first refusal by %s, at %llu bytes: %s", name.c_str(), reduce,
                            decider.c_str(), static_cast<unsigned long long>(lo), f.errors.c_str());
            }
            CHECK_MSG(f.status == NJ2K_E_LIMIT, "%s r%d: a fresh codec at %llu bytes gives %d, not NJ2K_E_LIMIT",
                      name.c_str(), reduce, static_cast<unsigned long long>(lo), int(f.status));
            if (decodes(w.status)) {
                // the warmed codec's own smallest limit, for the evidence
                std::uint64_t wlo = 1, whi = lo;
                while (whi - wlo > 1) {
                    const std::uint64_t mid = wlo + (whi - wlo) / 2;
                    (decodes(warmed(bytes, reduce, kCeiling, mid).status) ? whi : wlo) = mid;
                }
                ++history;
                std::printf("C2 %s r%d: a fresh codec decodes from %llu bytes, a warmed one from %llu (%llu fewer); "
                            "the fresh codec's refusal at %llu: %s\n",
                            name.c_str(), reduce, static_cast<unsigned long long>(hi),
                            static_cast<unsigned long long>(whi), static_cast<unsigned long long>(hi - whi),
                            static_cast<unsigned long long>(lo), decider.c_str());
            }
        }
    }
    std::printf("C2: %zu inputs and reduce levels bisected, %zu decode on a warmed codec at a limit a fresh codec "
                "refuses; the refusals at the boundary came from:",
                cases, history);
    for (const std::string& d : deciders) {
        std::printf(" [%s]", d.c_str());
    }
    std::printf("\n");
    CHECK_MSG(cases > 0, "no input to bisect");
    if (expect_history) {
        CHECK_MSG(history > 0, "C2 no longer holds: change the expectation to none");
    } else {
        CHECK_MSG(history == 0, "a decode at the memory limit depends on the codec's history (C2)");
    }
    return g_failures ? 1 : 0;
}

// ---- C3 ----------------------------------------------------------------------------------------

// MXCSR: round toward zero (RC = 11), flush-to-zero (FZ), denormals-are-zero (DAZ)
constexpr unsigned kMxcsrRz = 3u << 13, kMxcsrFz = 1u << 15, kMxcsrDaz = 1u << 6;

// An executor whose every job runs on one worker thread that set MXCSR when it started: a host
// whose worker threads keep another floating-point environment (game, audio and physics threads
// do). The caller waits for each batch; the worker lives as long as the executor.
class EnvExecutor {
public:
    explicit EnvExecutor(unsigned mxcsr) : worker_([this, mxcsr] { work(mxcsr); }) {}
    EnvExecutor(const EnvExecutor&) = delete;
    EnvExecutor& operator=(const EnvExecutor&) = delete;
    ~EnvExecutor()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        worker_.join();
    }

    std::uint64_t batches() const noexcept { return batches_; }

    static void run(void* user, NJ2K_UINT32 count, nj2k_job_fn job, void* ctx)
    {
        auto* self = static_cast<EnvExecutor*>(user);
        std::unique_lock<std::mutex> lock(self->mutex_);
        ++self->batches_;
        self->job_ = job;
        self->ctx_ = ctx;
        self->count_ = count;
        self->pending_ = true;
        self->wake_.notify_all();
        self->done_.wait(lock, [self] { return !self->pending_; });
    }

private:
    void work(unsigned mxcsr)
    {
        _mm_setcsr((_mm_getcsr() & ~(kMxcsrRz | kMxcsrFz | kMxcsrDaz)) | mxcsr);
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock, [this] { return pending_ || stop_; });
            if (!pending_) {
                return;
            }
            for (NJ2K_UINT32 i = 0; i < count_; ++i) {
                job_(ctx_, i);  // under the lock: jobs never call the executor (neoj2k.h)
            }
            pending_ = false;
            done_.notify_all();
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_, done_;
    nj2k_job_fn job_ = nullptr;
    void* ctx_ = nullptr;
    NJ2K_UINT32 count_ = 0;
    bool pending_ = false, stop_ = false;
    std::uint64_t batches_ = 0;
    std::thread worker_;  // last: started once the members above exist
};

int run_c3(const std::string& corpus, bool expect_differ)
{
    struct Env {
        const char* name;
        unsigned mxcsr;
    };
    static const Env envs[] = {{"round toward zero", kMxcsrRz},
                               {"FTZ and DAZ", kMxcsrFz | kMxcsrDaz},
                               {"round toward zero, FTZ and DAZ", kMxcsrRz | kMxcsrFz | kMxcsrDaz}};
    std::size_t files = 0, differ = 0, forced_batches = 0;
    for (const std::string& path : nj2k::test::expand_inputs(corpus)) {
        const std::string name = nj2k::test::base_name(path);
        if (name.find("_97") == std::string::npos) {
            continue;
        }
        const auto data = nj2k::test::read_file(path);
        if (!data) {
            continue;
        }
        nj2k_codec_t* one = make_codec();
        const Decoded ref = one ? decode(one, *data) : Decoded{};
        if (one) {
            nj2k_destroy_codec(one);
        }
        // images up to 2^20 samples: the corpus's two larger ones would add a minute to every build
        if (!ref.ok || ref.samples.size() > (std::size_t(1) << 20)) {
            continue;
        }
        ++files;
        bool file_differs = false;
        for (const Env& e : envs) {
            EnvExecutor env(e.mxcsr);
            nj2k_codec_t* many = make_codec();
            const nj2k_executor_t ex{sizeof(nj2k_executor_t), 8, &EnvExecutor::run, &env};
            const bool ready = many && nj2k_decoder_set_executor(many, &ex) &&
                               nj2k::detail::set_parallel_thresholds(many, 0, 0, 0, 0);
            CHECK_MSG(ready, "%s: no threaded codec", name.c_str());
            if (!ready) {
                if (many) {
                    nj2k_destroy_codec(many);
                }
                continue;
            }
            const Decoded got = decode(many, *data);
            nj2k_destroy_codec(many);
            forced_batches += env.batches();
            CHECK_MSG(got.status == ref.status, "%s: status %d threaded, %d on one thread", name.c_str(),
                      int(got.status), int(ref.status));
            std::size_t n = 0;
            for (std::size_t i = 0; i < ref.samples.size() && i < got.samples.size(); ++i) {
                n += ref.samples[i] != got.samples[i];
            }
            if (n) {
                file_differs = true;
                std::printf("C3 %s, workers at %s: %zu of %zu samples differ from one thread (%llu executor "
                            "batches)\n",
                            name.c_str(), e.name, n, ref.samples.size(), static_cast<unsigned long long>(env.batches()));
            }
        }
        differ += file_differs;
    }
    std::printf("C3: %zu 9/7 files, %zu with other samples when the host's workers keep another floating-point "
                "environment (%zu executor batches in all)\n",
                files, differ, forced_batches);
    CHECK_MSG(files > 0 && forced_batches > 0, "no 9/7 file went through the executor");
    if (expect_differ) {
        CHECK_MSG(differ > 0, "C3 no longer holds: change the expectation to same (the hotfix's test)");
    } else {
        CHECK_MSG(differ == 0, "the host's workers' floating-point environment reaches the samples (C3)");
    }
    return g_failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv)
{
    nj2k::test::console_reports();
    if (argc != 5 || std::strcmp(argv[3], "--expect") != 0) {
        std::fprintf(stderr, "usage: nj2k_findings_unit --c2 <dir>[,<dir>...] --expect history|none\n"
                             "       nj2k_findings_unit --c3 <corpus> --expect differ|same\n");
        return 2;
    }
    const std::string what = argv[1], corpus = argv[2], expect = argv[4];
    if (what == "--c2" && (expect == "history" || expect == "none")) {
        return run_c2(nj2k::test::split(corpus, ','), expect == "history");
    }
    if (what == "--c3" && (expect == "differ" || expect == "same")) {
        return run_c3(corpus, expect == "differ");
    }
    std::fprintf(stderr, "unknown arguments\n");
    return 2;
}

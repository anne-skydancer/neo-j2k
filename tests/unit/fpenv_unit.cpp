// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The output does not depend on the host's floating-point environment (finding C3 of the Rust twin's
// plan, found by its proof test; doc/api.md, "Threads in one image"). A 9/7 decode or a lossy encode
// rounds its results, so a host thread whose environment is not the default (round to nearest-even, no
// flush-to-zero, no denormals-are-zero) used to change the samples. For every environment below (round
// toward zero, round up, round down, FTZ with DAZ, and the rounding modes with both) this checks that
//   - a decode of every 9/7 file of the folder (up to 2^20 samples) is the same when its executor's
//     worker thread runs the jobs under that environment, with every stage forced through the executor,
//   - the same when the calling thread has the environment (no executor, and an executor whose worker is
//     at the default),
//   - a lossy (9/7) encode on a calling thread under the environment writes the same bytes,
//   - and that the caller's MXCSR is the one it set after each call (the library puts it back).
// Usage: nj2k_fpenv_unit <corpus folder>
#include <neoj2k/neoj2k.h>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <xmmintrin.h>

#include "common.h"

// the library's internals (by their path: src/ is no include directory)
#include "../../src/parallel.h"

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

// MXCSR control bits: rounding control (bits 13-14), flush-to-zero (15), denormals-are-zero (6)
constexpr unsigned kRc = 3u << 13, kFz = 1u << 15, kDaz = 1u << 6;
constexpr unsigned kControl = kRc | kFz | kDaz;
constexpr unsigned kDown = 1u << 13, kUp = 2u << 13, kZero = 3u << 13;

struct Env {
    const char* name;
    unsigned bits;
};
const Env kEnvs[] = {{"round toward zero", kZero},
                     {"round up", kUp},
                     {"round down", kDown},
                     {"FTZ and DAZ", kFz | kDaz},
                     {"round toward zero, FTZ and DAZ", kZero | kFz | kDaz},
                     {"round up, FTZ and DAZ", kUp | kFz | kDaz}};

unsigned control() { return _mm_getcsr() & kControl; }
void set_control(unsigned bits) { _mm_setcsr((_mm_getcsr() & ~kControl) | bits); }

// The calling thread under an environment, put back when the scope ends.
class CallerEnv {
public:
    explicit CallerEnv(unsigned bits) : saved_(control()) { set_control(bits); }
    ~CallerEnv() { set_control(saved_); }
    CallerEnv(const CallerEnv&) = delete;
    CallerEnv& operator=(const CallerEnv&) = delete;

private:
    unsigned saved_;
};

// An executor whose every job runs on one worker thread that set its environment when it started: a
// host whose worker threads keep another floating-point environment. The caller waits for each batch.
class EnvExecutor {
public:
    explicit EnvExecutor(unsigned bits) : worker_([this, bits] { work(bits); }) {}
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
    void work(unsigned bits)
    {
        set_control(bits);
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

// ---- decode ------------------------------------------------------------------------------------

struct Decoded {
    bool ok = false;
    nj2k_status_t status = NJ2K_OK;
    std::vector<std::int32_t> samples;
};

// A memory decode (planar I32 planes) by a fresh codec; with an executor, every stage goes through it.
Decoded decode(const std::vector<std::uint8_t>& data, EnvExecutor* env)
{
    Decoded d;
    nj2k_codec_t* codec = nj2k_create_decompress(NJ2K_CODEC_J2K);
    if (!codec) {
        return d;
    }
    nj2k_dparameters_t p;
    nj2k_set_default_decoder_parameters(&p);
    bool ready = nj2k_setup_decoder(codec, &p) && nj2k_decoder_set_strict_mode(codec, NJ2K_FALSE);
    if (ready && env) {
        const nj2k_executor_t ex{sizeof(nj2k_executor_t), 8, &EnvExecutor::run, env};
        ready = nj2k_decoder_set_executor(codec, &ex) && nj2k::detail::set_parallel_thresholds(codec, 0, 0, 0, 0);
    }
    nj2k_image_t image;
    if (ready && nj2k_read_header_memory(codec, data.data(), data.size(), &image)) {
        std::size_t total = 0;
        for (std::uint32_t c = 0; c < image.numcomps; ++c) {
            total += std::size_t(image.comps[c].w) * image.comps[c].h;
        }
        d.samples.assign(total ? total : 1, 0);
        std::vector<nj2k_plane_t> planes(image.numcomps);
        std::size_t off = 0;
        for (std::uint32_t c = 0; c < image.numcomps; ++c) {
            planes[c] = nj2k_plane_t{d.samples.data() + off, std::int64_t(image.comps[c].w) * 4, 4, c};
            off += std::size_t(image.comps[c].w) * image.comps[c].h;
        }
        const nj2k_output_t out{sizeof(nj2k_output_t), NJ2K_SAMPLE_I32, image.numcomps, planes.data()};
        d.ok = nj2k_decode_memory(codec, data.data(), data.size(), &out) != NJ2K_FALSE;
    }
    d.status = nj2k_get_status(codec);
    nj2k_destroy_codec(codec);
    return d;
}

std::size_t differing(const Decoded& a, const Decoded& b)
{
    if (a.samples.size() != b.samples.size()) {
        return std::max(a.samples.size(), b.samples.size());
    }
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.samples.size(); ++i) {
        n += a.samples[i] != b.samples[i];
    }
    return n;
}

void test_decode(const std::string& corpus)
{
    std::size_t files = 0, batches = 0;
    for (const std::string& path : nj2k::test::expand_inputs(corpus)) {
        const std::string name = nj2k::test::base_name(path);
        if (name.find("_97") == std::string::npos) {
            continue;
        }
        const auto data = nj2k::test::read_file(path);
        if (!data) {
            continue;
        }
        const Decoded ref = decode(*data, nullptr);
        // images up to 2^20 samples: the corpus's larger ones would add minutes
        if (!ref.ok || ref.samples.size() > (std::size_t(1) << 20)) {
            continue;
        }
        ++files;
        for (const Env& e : kEnvs) {
            {  // the host's worker threads
                EnvExecutor env(e.bits);
                const Decoded got = decode(*data, &env);
                batches += env.batches();
                CHECK_MSG(got.ok && got.status == ref.status, "%s: status %d (workers at %s)", name.c_str(),
                          int(got.status), e.name);
                const std::size_t n = differing(ref, got);
                CHECK_MSG(n == 0, "%s: %zu of %zu samples differ, workers at %s", name.c_str(), n,
                          ref.samples.size(), e.name);
            }
            {  // the calling thread: one thread, and with workers at the default
                CallerEnv caller(e.bits);
                const Decoded one = decode(*data, nullptr);
                CHECK_MSG(control() == e.bits, "%s: the caller's MXCSR not put back (%s)", name.c_str(), e.name);
                const std::size_t n = differing(ref, one);
                CHECK_MSG(n == 0, "%s: %zu samples differ, calling thread at %s", name.c_str(), n, e.name);
                EnvExecutor env(0);
                const Decoded many = decode(*data, &env);
                batches += env.batches();
                const std::size_t m = differing(ref, many);
                CHECK_MSG(m == 0, "%s: %zu samples differ, calling thread at %s, threaded", name.c_str(), m, e.name);
                CHECK_MSG(control() == e.bits, "%s: the caller's MXCSR not put back (%s, threaded)", name.c_str(),
                          e.name);
            }
        }
    }
    std::printf("decode: %zu 9/7 files, %zu executor batches\n", files, batches);
    CHECK_MSG(files > 0 && batches > 0, "no 9/7 file went through the executor");
}

// ---- encode ------------------------------------------------------------------------------------

NJ2K_SIZE_T sink_write(void* buf, NJ2K_SIZE_T n, void* user)
{
    auto* v = static_cast<std::vector<std::uint8_t>*>(user);
    v->insert(v->end(), static_cast<std::uint8_t*>(buf), static_cast<std::uint8_t*>(buf) + n);
    return n;
}

// A lossy (9/7, colour transform) encode of a 3-component image of noise and ramps; false when it failed.
bool encode(int w, int h, std::vector<std::uint8_t>& out)
{
    nj2k_cparameters_t p;
    nj2k_set_default_encoder_parameters(&p);
    p.prog_order = NJ2K_RLCP;
    p.irreversible = 1;
    p.tcp_mct = 1;
    p.cp_disto_alloc = 1;
    p.tcp_numlayers = 1;
    p.tcp_rates[0] = 8.0f;
    std::vector<nj2k_image_cmptparm_t> parms(3);
    for (auto& c : parms) {
        std::memset(&c, 0, sizeof(c));
        c.prec = 8;
        c.dx = c.dy = 1;
        c.w = static_cast<NJ2K_UINT32>(w);
        c.h = static_cast<NJ2K_UINT32>(h);
    }
    nj2k_image_t* image = nj2k_image_create(3, parms.data(), NJ2K_CLRSPC_SRGB);
    nj2k_codec_t* codec = nj2k_create_compress(NJ2K_CODEC_J2K);
    nj2k_stream_t* stream = nj2k_stream_create(1, NJ2K_FALSE);
    bool ok = image && codec && stream;
    if (ok) {
        image->x1 = static_cast<NJ2K_UINT32>(w);
        image->y1 = static_cast<NJ2K_UINT32>(h);
        std::uint32_t r = 12345;
        for (int c = 0; c < 3; ++c) {
            for (int i = 0; i < w * h; ++i) {
                r = r * 1664525u + 1013904223u;
                image->comps[c].data[i] = ((i % w) * (c + 2) / 3 + (i / w) + static_cast<int>(r >> 29)) & 0xFF;
            }
        }
        nj2k_stream_set_user_data(stream, &out, nullptr);
        nj2k_stream_set_write_function(stream, sink_write);
        ok = nj2k_setup_encoder(codec, &p, image) && nj2k_start_compress(codec, image, stream) &&
             nj2k_encode(codec, stream) && nj2k_end_compress(codec, stream);
    }
    if (stream) {
        nj2k_stream_destroy(stream);
    }
    if (codec) {
        nj2k_destroy_codec(codec);
    }
    if (image) {
        nj2k_image_destroy(image);
    }
    return ok;
}

void test_encode()
{
    std::vector<std::uint8_t> ref;
    CHECK_MSG(encode(131, 97, ref) && ref.size() > 100, "the reference encode failed");
    for (const Env& e : kEnvs) {
        CallerEnv caller(e.bits);
        std::vector<std::uint8_t> got;
        const bool ok = encode(131, 97, got);
        CHECK_MSG(ok && got == ref, "the lossy encode differs (%zu against %zu bytes), calling thread at %s",
                  got.size(), ref.size(), e.name);
        CHECK_MSG(control() == e.bits, "the caller's MXCSR not put back by the encode (%s)", e.name);
    }
    std::printf("encode: %zu bytes, %zu environments\n", ref.size(), sizeof(kEnvs) / sizeof(kEnvs[0]));
}

}  // namespace

int main(int argc, char** argv)
{
    nj2k::test::console_reports();
    if (argc != 2) {
        std::fprintf(stderr, "usage: nj2k_fpenv_unit <corpus folder>\n");
        return 2;
    }
    CHECK_MSG(control() == 0, "the test starts in a non-default environment");
    test_decode(argv[1]);
    test_encode();
    return g_failures ? 1 : 0;
}

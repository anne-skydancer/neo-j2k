// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The openjp2 module: OpenJPEG's API as a library a viewer already loads (openjp2.dll, libopenjp2.so.7),
// with neo-j2k inside. A viewer that talks to OpenJPEG keeps talking to it, unchanged; the module is the
// adapter between its calls and neo-j2k's.
//
// What an OpenJPEG caller meets here:
//  - every function of OpenJPEG 2.5.4's openjpeg.h (and the fork's decoder limits API) under its name.
//    Most call their neo-j2k twin with the same arguments (opj_forward.inc, generated: neo-j2k's structures
//    have OpenJPEG's layout, and the generator checks that every signature matches openjpeg.h's);
//  - opj_codec_set_threads starts the threads OpenJPEG would, which neo-j2k's core never does (it starts
//    none and takes an executor from its host): the module is that host. A codec gets a pool of its own,
//    lent to it as the executor, and opj_destroy_codec joins it, as OpenJPEG does;
//  - opj_version says "nj2k" (a viewer prints it after "Runtime:", in a field that holds five characters);
//  - a codec is refused on a processor the library does not run on (opj_create_* give NULL), where the
//    core's own callers check nj2k_cpu_supported() first.
//
// This file is built for baseline x86-64 whatever the core's instruction set is, so that the CPU check can
// run before any instruction of the core's does.
#include <neoj2k/neoj2k.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <new>
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
#define OPJ_EXPORT extern "C" __declspec(dllexport)
#else
#if defined(__linux__)
#include <pthread.h>
#endif
#define OPJ_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#define NJ2K_XSTR(x) #x
#define NJ2K_STR(x) NJ2K_XSTR(x)

// the forwarders, and the declarations of the functions below
#include "opj_forward.inc"

namespace {

// The name the pool's threads carry (a debugger lists them, and the tests count them): the thread
// description on Windows 10 and later, looked up at run time so that older Windows still load the library,
// and the thread name on Linux (at most 15 characters).
void name_this_thread() noexcept
{
#if defined(_WIN32)
    using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll")) {
        if (auto set = reinterpret_cast<SetDescription>(reinterpret_cast<void*>(GetProcAddress(kernel, "SetThreadDescription")))) {
            set(GetCurrentThread(), L"openjp2-worker");
        }
    }
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), "openjp2-worker");
#endif
}

// A decode is cut into at most 64 parts (doc/api.md, threads in one image), so more threads would idle.
constexpr int kMaxThreads = 64;

// The pool behind one decoder's opj_codec_set_threads: workers started once and joined when the codec is
// destroyed (or its threads set again), whose parallel_for queues a batch of jobs, wakes the workers, runs
// jobs itself while any is unclaimed and returns when every job has returned. A caller never waits for a
// job it could run itself, so nothing deadlocks with every worker busy. The same design as the tests'
// pool (tests/common/thread_pool.h), which the ThreadSanitizer job exercises.
class Pool {
public:
    // workers threads besides the caller; nullptr when memory or a thread is refused
    static Pool* create(unsigned workers) noexcept
    {
        Pool* p = new (std::nothrow) Pool;
        if (!p) {
            return nullptr;
        }
        try {
            p->threads_.reserve(workers);
            for (unsigned i = 0; i < workers; ++i) {
                p->threads_.emplace_back([p] { p->work(); });
            }
        } catch (...) {
            delete p;  // stops and joins the workers that did start
            return nullptr;
        }
        return p;
    }

    ~Pool()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        for (std::thread& t : threads_) {
            t.join();
        }
    }

    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    // threads in all, the calling thread included, as the codec cuts its stages
    nj2k_executor_t executor() noexcept
    {
        return nj2k_executor_t{sizeof(nj2k_executor_t), static_cast<NJ2K_UINT32>(threads_.size() + 1), &Pool::run,
                               this};
    }

private:
    Pool() = default;

    struct Batch {
        Batch(nj2k_job_fn j, void* c, std::uint32_t n) noexcept : job(j), ctx(c), count(n) {}
        nj2k_job_fn job;
        void* ctx;
        std::uint32_t count;
        std::atomic<std::uint32_t> next{0};
        std::uint32_t users = 0;  // workers between picking the batch and leaving it (under mutex_)
    };

    static void run(void* user, NJ2K_UINT32 count, nj2k_job_fn job, void* ctx)
    {
        static_cast<Pool*>(user)->parallel_for(count, job, ctx);
    }

    // claims and runs the batch's jobs until none is left
    static void drain(Batch& b) noexcept
    {
        for (std::uint32_t i = b.next.fetch_add(1, std::memory_order_relaxed); i < b.count;
             i = b.next.fetch_add(1, std::memory_order_relaxed)) {
            b.job(b.ctx, i);
        }
    }

    void parallel_for(std::uint32_t count, nj2k_job_fn job, void* ctx)
    {
        Batch b{job, ctx, count};
        if (count > 1 && !threads_.empty()) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                open_.push_back(&b);
            }
            wake_.notify_all();
        }
        drain(b);
        std::unique_lock<std::mutex> lock(mutex_);
        // no worker can pick it up any more; those that did finish their jobs and leave it
        open_.erase(std::remove(open_.begin(), open_.end(), &b), open_.end());
        left_.wait(lock, [&] { return b.users == 0; });
    }

    void work()
    {
        name_this_thread();
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            Batch* b = nullptr;
            wake_.wait(lock, [&] {
                for (Batch* o : open_) {
                    if (o->next.load(std::memory_order_relaxed) < o->count) {
                        b = o;
                        return true;
                    }
                }
                return stop_;
            });
            if (!b) {
                return;  // stopping, nothing left to do
            }
            ++b->users;
            lock.unlock();
            drain(*b);
            lock.lock();
            if (--b->users == 0) {
                left_.notify_all();
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_, left_;
    std::vector<Batch*> open_;
    bool stop_ = false;
    std::vector<std::thread> threads_;
};

// The codecs that have a pool: opj_destroy_codec has only the codec to find it by. A list under a lock;
// the library's objects hold no state of their own, and this module's one writable global is this.
struct Slot {
    nj2k_codec_t* codec;
    Pool* pool;
    Slot* next;
};

std::mutex g_slots_lock;
Slot* g_slots = nullptr;

// forgets the codec's pool and returns it (nullptr when it has none)
Pool* take(nj2k_codec_t* codec) noexcept
{
    std::lock_guard<std::mutex> lock(g_slots_lock);
    for (Slot** s = &g_slots; *s; s = &(*s)->next) {
        if ((*s)->codec == codec) {
            Slot* gone = *s;
            Pool* pool = gone->pool;
            *s = gone->next;
            delete gone;
            return pool;
        }
    }
    return nullptr;
}

bool remember(nj2k_codec_t* codec, Pool* pool) noexcept
{
    Slot* s = new (std::nothrow) Slot{codec, pool, nullptr};
    if (!s) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_slots_lock);
    s->next = g_slots;
    g_slots = s;
    return true;
}

// What opj_version() returns, which a viewer prints after "Runtime:" and keys its texture cache on. FIVE
// CHARACTERS AT MOST: Firestorm stores "OpenJPEG: 2.5.4, Runtime: <this>" in a 32-byte field of its texture
// cache header (sHeaderEncoderStringSize in lltexturecache.h) and stops with a fatal error when it does not fit
// ("Version string doesn't fit in header", lltexturecache.cpp). OpenJPEG's own "2.5.4" is exactly five, and the
// text before it takes 26 of the 31 characters. A longer string made the viewer crash at start (v0.1.1).
// "nj2k" names neo-j2k where the viewer shows it; the full name and the version are in the file's version
// resource (modules/openjp2/openjp2.rc.in) and in the zip's instructions.
constexpr char kVersion[] = "nj2k";
static_assert(sizeof(kVersion) - 1 <= 5, "opj_version() must fit what Firestorm's texture cache header holds");

}  // namespace

OPJ_EXPORT const char* opj_version(void)
{
    return kVersion;
}

OPJ_EXPORT NJ2K_BOOL opj_has_thread_support(void)
{
    return NJ2K_TRUE;
}

OPJ_EXPORT nj2k_codec_t* opj_create_decompress(NJ2K_CODEC_FORMAT format)
{
    if (!nj2k_cpu_supported()) {
        return nullptr;
    }
    return nj2k_create_decompress(format);
}

OPJ_EXPORT nj2k_codec_t* opj_create_compress(NJ2K_CODEC_FORMAT format)
{
    if (!nj2k_cpu_supported()) {
        return nullptr;
    }
    return nj2k_create_compress(format);
}

// OpenJPEG gives the codec a pool of num_threads workers, replacing the one it had, and answers true. The
// same here, with these differences, none of which changes a sample or a byte:
//  - the calling thread works too while it waits, so the codec runs on num_threads threads in all, not
//    num_threads plus a waiting caller; 0 and 1 run on the calling thread;
//  - at most 64 threads (the most a decode or encode stage is cut into).
OPJ_EXPORT NJ2K_BOOL opj_codec_set_threads(nj2k_codec_t* p_codec, int num_threads)
{
    if (!p_codec || num_threads < 0) {
        return NJ2K_FALSE;
    }
    // The codec stops calling its earlier pool, and then the pool is joined. Trying the two setters also
    // identifies which kind of codec this is without adding a public type query.
    Pool* old = take(p_codec);
    const bool decoder = nj2k_decoder_set_executor(p_codec, nullptr) != 0;
    const bool encoder = !decoder && nj2k_encoder_set_executor(p_codec, nullptr) != 0;
    if (!decoder && !encoder) {
        delete old;
        return NJ2K_FALSE;
    }
    delete old;
    const int total = std::min(num_threads, kMaxThreads);
    if (total < 2) {
        return NJ2K_TRUE;
    }
    Pool* pool = Pool::create(static_cast<unsigned>(total - 1));
    if (!pool) {
        return NJ2K_FALSE;
    }
    const nj2k_executor_t executor = pool->executor();
    const bool lent = decoder ? nj2k_decoder_set_executor(p_codec, &executor) != 0
                              : nj2k_encoder_set_executor(p_codec, &executor) != 0;
    if (!lent || !remember(p_codec, pool)) {
        if (decoder) {
            nj2k_decoder_set_executor(p_codec, nullptr);
        } else {
            nj2k_encoder_set_executor(p_codec, nullptr);
        }
        delete pool;
        return NJ2K_FALSE;
    }
    return NJ2K_TRUE;
}

OPJ_EXPORT void opj_destroy_codec(nj2k_codec_t* p_codec)
{
    Pool* pool = take(p_codec);
    nj2k_destroy_codec(p_codec);
    delete pool;  // joins its threads: the codec is gone, and nothing calls them any more
}

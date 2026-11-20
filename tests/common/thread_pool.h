// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// A host's executor for the tests, the fuzz replay and the benches (stage 5): a pool of worker
// threads started once and joined when the pool is destroyed, whose parallel_for queues a batch of
// jobs, wakes the workers, runs jobs itself while any is unclaimed, and returns when every job has
// returned. Any number of threads may call it at once (many decoders sharing one pool, as a host's
// decode threads would), and a job may call it again: a caller never waits for a job it could run
// itself, so neither deadlocks with every worker busy. It is the kind of executor
// nj2k_decoder_set_executor() expects, not part of the library (which starts no thread).
#pragma once

#include <neoj2k/neoj2k.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace nj2k::test {

class ThreadPool {
public:
    // workers threads besides the callers (0: every job runs on its caller)
    explicit ThreadPool(unsigned workers)
    {
        threads_.reserve(workers);
        for (unsigned i = 0; i < workers; ++i) {
            threads_.emplace_back([this] { work(); });
        }
    }
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ~ThreadPool()
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

    unsigned workers() const noexcept { return static_cast<unsigned>(threads_.size()); }

    // The executor for a codec: concurrency 0 says the workers and the caller
    nj2k_executor_t executor(std::uint32_t concurrency = 0) noexcept
    {
        return nj2k_executor_t{sizeof(nj2k_executor_t), concurrency ? concurrency : workers() + 1, &ThreadPool::run,
                               this};
    }

    // parallel_for of nj2k_executor_t
    static void run(void* user, NJ2K_UINT32 count, nj2k_job_fn job, void* ctx)
    {
        static_cast<ThreadPool*>(user)->parallel_for(count, job, ctx);
    }

    // calls of parallel_for and jobs run so far (the tests check that a decode used the pool)
    std::uint64_t batches() const noexcept { return batches_.load(std::memory_order_relaxed); }

private:
    struct Batch {
        nj2k_job_fn job;
        void* ctx;
        std::uint32_t count;
        std::atomic<std::uint32_t> next{0};
        std::uint32_t users = 0;  // workers between picking the batch and leaving it (under mutex_)
    };

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
        batches_.fetch_add(1, std::memory_order_relaxed);
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
    std::atomic<std::uint64_t> batches_{0};
    std::vector<std::thread> threads_;
};

}  // namespace nj2k::test

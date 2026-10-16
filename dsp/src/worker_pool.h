#pragma once
// Persistent worker threads for per-block work on the playback path (the double-double
// resampler's phases): created once, woken for every block, so no thread is created while
// playing. run(n, f) calls f(i) for every i in [0, n) on the workers and the calling thread and
// returns when all are done. Each index is one independent job, so results do not depend on the
// thread count. Workers run at above-normal priority, and in the realtime helper class while they
// work for a realtime thread (platform::in_realtime_scope): a pool built for a chain warmed up in the
// background serves the processing thread once the chain is swapped in. The class changes only when
// the caller's does, not for every block.

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "daphne/platform/platform.h"

namespace daphne::dsp {

class WorkerPool {
public:
    explicit WorkerPool(int workers) {
        for (int t = 0; t < workers; ++t) th_.emplace_back([this] { loop(); });
    }
    ~WorkerPool() {
        {
            std::lock_guard<std::mutex> l(m_);
            stop_ = true;
        }
        start_.notify_all();
        for (auto& t : th_) t.join();
    }
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    int workers() const { return static_cast<int>(th_.size()); }

    template <class F>
    void run(int n, const F& f) {
        if (n <= 0) return;
        if (th_.empty() || n == 1) {
            for (int i = 0; i < n; ++i) f(i);
            return;
        }
        {
            std::lock_guard<std::mutex> l(m_);
            ctx_ = &f;
            call_ = [](const void* c, int i) { (*static_cast<const F*>(c))(i); };
            n_ = n;
            rt_ = platform::in_realtime_scope();
            next_.store(0);
            done_.store(0);
            ++gen_;
        }
        start_.notify_all();
        work();   // the caller takes jobs too
        std::unique_lock<std::mutex> l(m_);
        finished_.wait(l, [&] { return done_.load() == n_ && busy_ == 0; });
    }

private:
    void work() {
        for (int i; (i = next_.fetch_add(1)) < n_;) {
            call_(ctx_, i);
            if (done_.fetch_add(1) + 1 == n_) {
                std::lock_guard<std::mutex> l(m_);
                finished_.notify_all();
            }
        }
    }
    void loop() {
        platform::set_thread_priority(platform::Priority::AboveNormal);
        long long seen = 0;
        std::optional<platform::RealtimeHelperScope> rt;
        for (;;) {
            bool want_rt = false;
            {
                std::unique_lock<std::mutex> l(m_);
                start_.wait(l, [&] { return stop_ || gen_ != seen; });
                if (stop_) return;
                seen = gen_;
                want_rt = rt_;
                ++busy_;
            }
            if (want_rt && !rt) rt.emplace();
            else if (!want_rt && rt) rt.reset();
            work();
            {
                std::lock_guard<std::mutex> l(m_);
                --busy_;
            }
            finished_.notify_all();
        }
    }

    std::vector<std::thread> th_;
    std::mutex m_;
    std::condition_variable start_, finished_;
    bool stop_ = false;
    long long gen_ = 0;
    int busy_ = 0;              // workers inside a job (the caller waits for them to leave it)
    int n_ = 0;
    bool rt_ = false;           // the caller of this run is a realtime thread
    std::atomic<int> next_{0}, done_{0};
    const void* ctx_ = nullptr;
    void (*call_)(const void*, int) = nullptr;
};

}  // namespace daphne::dsp

#pragma once
// parallel_for for the filter designs. Every element is computed exactly as in the serial
// loop (no reductions are split), so results are bit-identical with any thread count.
// Workers take the caller's thread priority, and background mode when the caller runs in it
// (dsp::DesignBackgroundScope), so a background design does not compete with playback.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace daphne::dsp {

namespace detail {
inline thread_local bool t_design_background = false;
}

namespace detail {
inline std::atomic<int> g_design_threads{0};   // 0 = automatic
}

// Threads a design may use: the configured limit, or all cores but two (automatic).
inline int design_threads() {
    const int hw = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    const int n = detail::g_design_threads.load(std::memory_order_relaxed);
    if (n > 0) return std::min(n, hw);
    return std::max(1, hw - 2);
}

// f(begin, end) on contiguous parts of [0, n); parts of at least min_chunk elements. The
// calling thread runs the first part, so dsp::progress() calls inside f report its share
// (workers have no progress sink); if that part throws (cancellation), the workers are joined
// before the exception propagates.
template <class F>
void parallel_for(int64_t n, const F& f, int64_t min_chunk = 1 << 12) {
    if (n <= 0) return;
    const int64_t parts = std::min<int64_t>(design_threads(), std::max<int64_t>(1, n / std::max<int64_t>(1, min_chunk)));
    if (parts <= 1) {
        f(int64_t{0}, n);
        return;
    }
#ifdef _WIN32
    const int prio = GetThreadPriority(GetCurrentThread());
#endif
    const bool background = detail::t_design_background;
    std::vector<std::thread> th;
    th.reserve(static_cast<size_t>(parts - 1));
    for (int64_t t = 1; t < parts; ++t) {
        th.emplace_back([&, t] {
#ifdef _WIN32
            if (background) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
            else SetThreadPriority(GetCurrentThread(), prio);
#endif
            f(n * t / parts, n * (t + 1) / parts);
        });
    }
    try {
        f(int64_t{0}, n / parts);
    } catch (...) {
        for (auto& x : th) x.join();
        throw;
    }
    for (auto& x : th) x.join();
}

}  // namespace daphne::dsp

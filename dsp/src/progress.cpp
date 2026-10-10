#include "daphne/dsp/progress.h"

#include "parallel.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace daphne::dsp {

namespace {
thread_local std::atomic<double>* t_sink = nullptr;
thread_local double t_base = 0.0, t_span = 1.0;
thread_local const std::atomic<bool>* t_cancel = nullptr;
}  // namespace

void set_design_threads(int n) { detail::g_design_threads = std::max(0, n); }
int design_thread_count() { return design_threads(); }

DesignBackgroundScope::DesignBackgroundScope() : prev_(detail::t_design_background) { detail::t_design_background = true; }
DesignBackgroundScope::~DesignBackgroundScope() { detail::t_design_background = prev_; }

CancelScope::CancelScope(const std::atomic<bool>* flag) : prev_(t_cancel) { t_cancel = flag; }
CancelScope::~CancelScope() { t_cancel = prev_; }

ProgressScope::ProgressScope(std::atomic<double>* sink) : prev_(t_sink), prev_base_(t_base), prev_span_(t_span) {
    t_sink = sink;
    t_base = 0.0;
    t_span = 1.0;
    if (t_sink) t_sink->store(0.0);
}

ProgressScope::~ProgressScope() {
    if (t_sink) t_sink->store(1.0);
    t_sink = prev_;
    t_base = prev_base_;
    t_span = prev_span_;
}

namespace {
thread_local int t_depth = 0;
thread_local std::chrono::steady_clock::time_point t_start[32];
bool trace_enabled() {
    char* v = nullptr;
    size_t n = 0;
    const bool on = _dupenv_s(&v, &n, "DAPHNE_PROGRESS_TRACE") == 0 && v != nullptr;
    free(v);
    return on;
}
const bool t_trace = trace_enabled();
}  // namespace

ProgressStage::ProgressStage(double begin, double end) : base_(t_base), span_(t_span) {
    if (t_trace && t_depth < 32) t_start[t_depth] = std::chrono::steady_clock::now();
    ++t_depth;
    t_base = base_ + span_ * begin;
    t_span = span_ * (end - begin);
    progress(0.0);
}

ProgressStage::~ProgressStage() {
    if (!(t_cancel && t_cancel->load(std::memory_order_relaxed))) progress(1.0);   // never throw from a destructor
    --t_depth;
    if (t_trace && t_depth < 32)
        std::fprintf(stderr, "%*s[%.4f, %.4f] %.3f s\n", t_depth * 2, "", t_base, t_base + t_span,
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start[t_depth]).count());
    t_base = base_;
    t_span = span_;
}

void progress(double fraction) {
    if (t_cancel && t_cancel->load(std::memory_order_relaxed)) throw DesignCancelled{};
    if (!t_sink) return;
    const double v = t_base + t_span * std::clamp(fraction, 0.0, 1.0);
    // keep the sink monotonic across stages
    double cur = t_sink->load(std::memory_order_relaxed);
    while (v > cur && !t_sink->compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

}  // namespace daphne::dsp

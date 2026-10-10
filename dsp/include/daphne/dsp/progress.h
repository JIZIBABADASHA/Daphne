#pragma once
// Progress of long computations (filter design). The caller installs a sink for the current
// thread; the design code reports fractions inside nested stage windows.
//
//   std::atomic<double> p{0};
//   { daphne::dsp::ProgressScope scope(&p); design(...); }   // p goes 0 -> 1
//
// Inside the design:
//   { ProgressStage s(0.1, 0.9); ...; progress(0.5); }      // reports 0.5 of [0.1, 0.9] of the
//                                                          // enclosing window

#include <atomic>

namespace daphne::dsp {

class ProgressScope {
public:
    explicit ProgressScope(std::atomic<double>* sink);
    ~ProgressScope();
    ProgressScope(const ProgressScope&) = delete;
    ProgressScope& operator=(const ProgressScope&) = delete;

private:
    std::atomic<double>* prev_;
    double prev_base_, prev_span_;
};

class ProgressStage {
public:
    ProgressStage(double begin, double end);
    ~ProgressStage();
    ProgressStage(const ProgressStage&) = delete;
    ProgressStage& operator=(const ProgressStage&) = delete;

private:
    double base_, span_;
};

// Cooperative cancellation: while a CancelScope is active on this thread, progress() throws
// DesignCancelled once the flag becomes true (designs only hold RAII resources).
struct DesignCancelled {};

class CancelScope {
public:
    explicit CancelScope(const std::atomic<bool>* flag);
    ~CancelScope();
    CancelScope(const CancelScope&) = delete;
    CancelScope& operator=(const CancelScope&) = delete;

private:
    const std::atomic<bool>* prev_;
};

// Threads a filter design may use (parallel_for); 0 = automatic (all cores but two).
void set_design_threads(int n);
int design_thread_count();

// Filter designs use several cores (parallel_for). Inside this scope (e.g. a background
// thread) the worker threads run in Windows background mode as well.
class DesignBackgroundScope {
public:
    DesignBackgroundScope();
    ~DesignBackgroundScope();
    DesignBackgroundScope(const DesignBackgroundScope&) = delete;
    DesignBackgroundScope& operator=(const DesignBackgroundScope&) = delete;

private:
    bool prev_;
};

// Fraction 0..1 of the current stage window (monotonic reports are not required).
// Throws DesignCancelled when the current CancelScope's flag is set.
void progress(double fraction);

}  // namespace daphne::dsp

// platform.h on macOS and Linux.
#include "daphne/platform/platform.h"

#include <pthread.h>
#include <sched.h>

#include <cstdlib>
#include <sys/types.h>
#include <functional>
#include <thread>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread/qos.h>
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace daphne::platform {

namespace {

thread_local Priority t_priority = Priority::Normal;

#if defined(__APPLE__)

uint32_t abs_from_ns(uint64_t ns) {
    mach_timebase_info_data_t tb{};
    mach_timebase_info(&tb);
    return static_cast<uint32_t>(ns * tb.denom / tb.numer);
}

// A time-constraint policy: up to 5 ms of work in every 10 ms (the processing thread fills a
// ring of about 0.5 s; these values only ask to be scheduled promptly, not a strict period).
bool make_realtime() {
    thread_time_constraint_policy_data_t p{};
    p.period = abs_from_ns(10'000'000);
    p.computation = abs_from_ns(5'000'000);
    p.constraint = abs_from_ns(10'000'000);
    p.preemptible = TRUE;
    return thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                             reinterpret_cast<thread_policy_t>(&p), THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
}

void undo_realtime() {
    thread_standard_policy_data_t p{};
    thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_STANDARD_POLICY, reinterpret_cast<thread_policy_t>(&p),
                      THREAD_STANDARD_POLICY_COUNT);
}

#else

struct SavedSched {
    bool set = false;
    int policy = SCHED_OTHER;
    sched_param param{};
};
thread_local SavedSched t_saved;

// SCHED_FIFO in the lower third of its range (the device threads of a sound server sit higher);
// refused without CAP_SYS_NICE / an RLIMIT_RTPRIO, and then the thread stays as it was.
bool make_realtime() {
    SavedSched s;
    if (pthread_getschedparam(pthread_self(), &s.policy, &s.param) != 0) return false;
    sched_param p{};
    const int lo = sched_get_priority_min(SCHED_FIFO), hi = sched_get_priority_max(SCHED_FIFO);
    p.sched_priority = lo + (hi - lo) / 3;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &p) != 0) return false;
    s.set = true;
    t_saved = s;
    return true;
}

void undo_realtime() {
    if (!t_saved.set) return;
    pthread_setschedparam(pthread_self(), t_saved.policy, &t_saved.param);
    t_saved.set = false;
}

#endif

}  // namespace

RealtimeScope::RealtimeScope() {
    realtime_ = make_realtime();
    fp_ = fp_control();
    set_fp_control(with_flush_to_zero(fp_));
}

RealtimeScope::~RealtimeScope() {
    set_fp_control(fp_);
    if (realtime_) undo_realtime();
}

RealtimeHelperScope::RealtimeHelperScope() {
    if (make_realtime()) handle_ = this;
}

RealtimeHelperScope::~RealtimeHelperScope() {
    if (handle_) undo_realtime();
}

bool set_thread_priority(Priority p) {
    bool ok = true;
#if defined(__APPLE__)
    qos_class_t q = QOS_CLASS_DEFAULT;
    switch (p) {
        case Priority::Background: q = QOS_CLASS_BACKGROUND; break;
        case Priority::BelowNormal: q = QOS_CLASS_UTILITY; break;
        case Priority::Normal: q = QOS_CLASS_DEFAULT; break;
        case Priority::AboveNormal: q = QOS_CLASS_USER_INITIATED; break;
    }
    ok = pthread_set_qos_class_self_np(q, 0) == 0;
#else
    // Linux: background is SCHED_IDLE; the others SCHED_OTHER with a nice value for this thread.
    // Threads inherit the creator's policy (PTHREAD_INHERIT_SCHED): one started by the realtime
    // processing thread (a filter design, parallel_for workers, the gapless preparation) is SCHED_FIFO
    // until it leaves it here; nice alone would not apply to it.
    sched_param sp{};
    if (p == Priority::Background) {
        ok = pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp) == 0;
    } else {
        int policy = SCHED_OTHER;
        sched_param cur{};
        if (pthread_getschedparam(pthread_self(), &policy, &cur) == 0 && policy != SCHED_OTHER)
            ok = pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp) == 0;   // lowering is always allowed
        const int nice = p == Priority::BelowNormal ? 5 : p == Priority::AboveNormal ? -5 : 0;
        ok = setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), nice) == 0 && ok;
    }
#endif
    if (ok) t_priority = p;
    return ok;
}

Priority thread_priority() { return t_priority; }

std::optional<std::string> get_env(const char* name) {
    const char* v = std::getenv(name);
    if (!v) return std::nullopt;
    return std::string(v);
}

std::filesystem::path user_cache_dir() {
#if defined(__APPLE__)
    if (auto home = get_env("HOME"); home && !home->empty()) return path_from_utf8(*home) / "Library" / "Caches";
#else
    if (auto x = get_env("XDG_CACHE_HOME"); x && !x->empty()) return path_from_utf8(*x);
    if (auto home = get_env("HOME"); home && !home->empty()) return path_from_utf8(*home) / ".cache";
#endif
    return {};
}

FILE* open_file(const std::filesystem::path& p, const char* mode) { return std::fopen(p.c_str(), mode); }

static_assert(sizeof(off_t) >= 8, "seek_file needs a 64-bit off_t (_FILE_OFFSET_BITS=64, platform/CMakeLists.txt)");
bool seek_file(FILE* f, int64_t offset) { return fseeko(f, static_cast<off_t>(offset), SEEK_SET) == 0; }

uint64_t thread_tag() { return std::hash<std::thread::id>{}(std::this_thread::get_id()); }

// the native path is already bytes (UTF-8 by convention): no conversion, so nothing can fail
std::filesystem::path path_from_utf8(std::string_view s) { return std::filesystem::path(std::string(s)); }

std::string path_to_utf8(const std::filesystem::path& p) { return p.native(); }

}  // namespace daphne::platform

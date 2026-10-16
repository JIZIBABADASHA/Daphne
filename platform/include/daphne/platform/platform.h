#pragma once
// The operating system services the engine needs, in one place (N25). Everything above this layer
// (dsp, core, capi, cli) is plain C++20; the Windows / macOS / Linux specifics live in
// platform/src/platform_<os>.cpp.
//
// Thread scheduling is best effort: a request the OS refuses (no realtime permission on Linux,
// for example) leaves the thread as it was and is reported by the return value, never fatal.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace daphne::platform {

// ---- threads

// The playback processing thread for its lifetime: Windows MMCSS "Audio", 1 ms timer resolution and
// no EcoQoS throttling; macOS a time-constraint (realtime) policy; Linux SCHED_FIFO when allowed.
// Denormals are flushed (FTZ / DAZ) while it lives.
class RealtimeScope {
public:
    RealtimeScope();
    ~RealtimeScope();
    RealtimeScope(const RealtimeScope&) = delete;
    RealtimeScope& operator=(const RealtimeScope&) = delete;
    // The OS accepted the realtime class.
    bool realtime() const { return realtime_; }

private:
    [[maybe_unused]] void* handle_ = nullptr;   // Windows: the MMCSS handle (unused elsewhere)
    uint64_t fp_ = 0;          // the floating-point control state before
    bool realtime_ = false;
    bool outer_ = false;       // in_realtime_scope() before
};

// The calling thread is inside a RealtimeScope (asked for it, whether or not the OS accepted): a worker
// pool gives its workers the helper class below while they work for such a thread.
bool in_realtime_scope();

// A helper thread of the realtime one (the DSD modulator's second channel, the double-double
// resampler's workers): the same class.
class RealtimeHelperScope {
public:
    RealtimeHelperScope();
    ~RealtimeHelperScope();
    RealtimeHelperScope(const RealtimeHelperScope&) = delete;
    RealtimeHelperScope& operator=(const RealtimeHelperScope&) = delete;

private:
    void* handle_ = nullptr;
};

enum class Priority {
    Background,    // Windows background mode (also lower I/O priority); macOS utility QoS; Linux SCHED_IDLE
    BelowNormal,
    Normal,
    AboveNormal,
};
// The calling thread. False when the OS refused.
bool set_thread_priority(Priority p);
// The calling thread's priority as set through this layer (Normal when never set), so worker
// threads can take their creator's (parallel_for).
Priority thread_priority();

// ---- floating-point control (x86 MXCSR, ARM64 FPCR; 0 elsewhere)

uint64_t fp_control();
void set_fp_control(uint64_t v);
// v with denormals flushed: x86 FTZ + DAZ, ARM64 FZ.
uint64_t with_flush_to_zero(uint64_t v);

// ---- environment and files

// An environment variable (UTF-8); nullopt when not set.
std::optional<std::string> get_env(const char* name);
// The per-user cache root: Windows %LOCALAPPDATA%, macOS ~/Library/Caches, Linux $XDG_CACHE_HOME or
// ~/.cache. Empty when none can be found.
std::filesystem::path user_cache_dir();
// fopen with a path in the OS's own encoding (UTF-16 on Windows, UTF-8 elsewhere); mode as fopen's.
FILE* open_file(const std::filesystem::path& p, const char* mode);
// fseek to a 64-bit offset from the start; false on error.
bool seek_file(FILE* f, int64_t offset);
// A number that differs between the threads of this process (temporary file names).
uint64_t thread_tag();

// ---- paths <-> UTF-8 (the C API and JSON use UTF-8; Windows paths are UTF-16)
// Never throw: on Windows an invalid sequence either way becomes U+FFFD (a file name may hold an
// unpaired surrogate); elsewhere the bytes pass through.

std::filesystem::path path_from_utf8(std::string_view s);
std::string path_to_utf8(const std::filesystem::path& p);

}  // namespace daphne::platform

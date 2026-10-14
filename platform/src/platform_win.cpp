// platform.h on Windows.
#include "daphne/platform/platform.h"

#include <windows.h>
#include <avrt.h>
#include <timeapi.h>

#include <vector>

namespace daphne::platform {

namespace {

std::string utf8(const wchar_t* w, int n) {
    if (n <= 0) return {};
    const int m = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(m), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, n, s.data(), m, nullptr, nullptr);
    return s;
}

}  // namespace

RealtimeScope::RealtimeScope() {
    timeBeginPeriod(1);
    PROCESS_POWER_THROTTLING_STATE pt{};
    pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    pt.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);
    DWORD task = 0;
    handle_ = AvSetMmThreadCharacteristicsW(L"Audio", &task);
    realtime_ = handle_ != nullptr;
    fp_ = fp_control();
    set_fp_control(with_flush_to_zero(fp_));
}

RealtimeScope::~RealtimeScope() {
    set_fp_control(fp_);
    if (handle_) AvRevertMmThreadCharacteristics(handle_);
    timeEndPeriod(1);
}

RealtimeHelperScope::RealtimeHelperScope() {
    DWORD task = 0;
    handle_ = AvSetMmThreadCharacteristicsW(L"Audio", &task);
}

RealtimeHelperScope::~RealtimeHelperScope() {
    if (handle_) AvRevertMmThreadCharacteristics(handle_);
}

bool set_thread_priority(Priority p) {
    int v = THREAD_PRIORITY_NORMAL;
    switch (p) {
        case Priority::Background: v = THREAD_MODE_BACKGROUND_BEGIN; break;
        case Priority::BelowNormal: v = THREAD_PRIORITY_BELOW_NORMAL; break;
        case Priority::Normal: v = THREAD_PRIORITY_NORMAL; break;
        case Priority::AboveNormal: v = THREAD_PRIORITY_ABOVE_NORMAL; break;
    }
    return SetThreadPriority(GetCurrentThread(), v) != 0;
}

Priority thread_priority() {
    const int v = GetThreadPriority(GetCurrentThread());
    if (v == THREAD_PRIORITY_ERROR_RETURN || v == THREAD_PRIORITY_NORMAL) return Priority::Normal;
    return v < THREAD_PRIORITY_NORMAL ? Priority::BelowNormal : Priority::AboveNormal;
}

std::optional<std::string> get_env(const char* name) {
    const std::string n = name;
    const std::wstring wn(n.begin(), n.end());   // variable names are ASCII
    const DWORD len = GetEnvironmentVariableW(wn.c_str(), nullptr, 0);
    if (len == 0) return std::nullopt;
    std::vector<wchar_t> buf(len);
    const DWORD got = GetEnvironmentVariableW(wn.c_str(), buf.data(), len);
    if (got == 0 || got >= len) return std::string{};
    return utf8(buf.data(), static_cast<int>(got));
}

std::filesystem::path user_cache_dir() {
    const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (len == 0) return {};
    std::wstring buf(len, L'\0');
    const DWORD got = GetEnvironmentVariableW(L"LOCALAPPDATA", buf.data(), len);
    if (got == 0 || got >= len) return {};
    buf.resize(got);
    return std::filesystem::path(buf);
}

FILE* open_file(const std::filesystem::path& p, const char* mode) {
    std::wstring m;
    for (const char* c = mode; *c; ++c) m += static_cast<wchar_t>(*c);
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), m.c_str()) != 0) return nullptr;
    return f;
}

bool seek_file(FILE* f, int64_t offset) { return _fseeki64(f, offset, SEEK_SET) == 0; }

uint64_t thread_tag() { return GetCurrentThreadId(); }

}  // namespace daphne::platform

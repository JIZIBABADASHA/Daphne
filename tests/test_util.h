#pragma once
// Portable helpers for the tests: temporary files, environment variables, WAV files by path.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <dr_libs/dr_wav.h>

#include "daphne/platform/platform.h"

namespace test {

inline std::filesystem::path temp_path(const std::string& name) { return std::filesystem::temp_directory_path() / name; }

// value nullptr removes the variable
inline void set_env(const char* name, const std::filesystem::path* value) {
#ifdef _WIN32
    _wputenv_s(std::filesystem::path(name).c_str(), value ? value->c_str() : L"");
#else
    if (value) setenv(name, value->c_str(), 1);
    else unsetenv(name);
#endif
}

inline bool wav_write_open(drwav* w, const std::filesystem::path& p, const drwav_data_format* fmt) {
#ifdef _WIN32
    return drwav_init_file_write_w(w, p.c_str(), fmt, nullptr);
#else
    return drwav_init_file_write(w, p.c_str(), fmt, nullptr);
#endif
}

}  // namespace test

#pragma once
// The parsers read files, so a fuzz input is written to a temporary file first: one per process, rewritten for
// every input (libFuzzer runs the inputs one after another in a process).
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>

namespace fuzz {

inline std::filesystem::path input_file(const std::string& ext, const uint8_t* data, size_t size) {
    static const std::string tag = std::to_string(std::random_device{}());
    const auto path = std::filesystem::temp_directory_path() / ("daphne_fuzz_" + tag + ext);
    FILE* f = std::fopen(path.string().c_str(), "wb");
    if (!f) return {};
    if (size) std::fwrite(data, 1, size, f);
    std::fclose(f);
    return path;
}

}  // namespace fuzz

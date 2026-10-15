// The C API's UTF-8 path functions (N25): paths with non-ASCII characters reach the engine intact.
#include <catch_amalgamated.hpp>

#include <cstring>
#include <filesystem>
#include <string>

#include <dr_libs/dr_wav.h>

#include "daphne_core.h"
#include "test_util.h"

namespace {

// a short silent stereo WAV
void write_wav(const std::filesystem::path& p) {
    drwav_data_format fmt{};
    fmt.container = drwav_container_riff;
    fmt.format = DR_WAVE_FORMAT_PCM;
    fmt.channels = 2;
    fmt.sampleRate = 44100;
    fmt.bitsPerSample = 16;
    drwav w;
    REQUIRE(test::wav_write_open(&w, p, &fmt));
    const int16_t z[2 * 441] = {};
    drwav_write_pcm_frames(&w, 441, z);
    drwav_uninit(&w);
}

// No sound in a test: an output device that does not exist (WASAPI: no such endpoint ID; elsewhere there is
// no output yet). A decoded file then fails with DC_ERR_DEVICE, a file that cannot be read with DC_ERR_FILE.
constexpr const char* kNoDevice = R"({"output": {"api": "wasapi_exclusive", "device": "daphne-test-no-such-device"}})";

}  // namespace

TEST_CASE("C API: dc_load_utf8 reads a file whose name is not ASCII", "[capi]") {
    CHECK(dc_abi_version() >= 2);
    const std::string name = "daphne_測試_🎵.wav";   // UTF-8 (the source is UTF-8: /utf-8 on MSVC)
    const auto p = std::filesystem::temp_directory_path() / std::filesystem::path(std::u8string(
                                                                reinterpret_cast<const char8_t*>(name.data()), name.size()));
    write_wav(p);
    const std::string utf8 = [&] {
        const std::u8string u = p.u8string();
        return std::string(reinterpret_cast<const char*>(u.data()), u.size());
    }();
    dc_player* pl = dc_create();
    REQUIRE(pl);
    char err[512] = {};
    // the file decodes: the failure is the device's
    CHECK(dc_load_utf8(pl, utf8.c_str(), kNoDevice, 0.0, 0, err, sizeof err) == DC_ERR_DEVICE);
    // the same name with one character changed is not found: the file's
    const std::string missing = utf8.substr(0, utf8.size() - 4) + "x.wav";
    CHECK(dc_load_utf8(pl, missing.c_str(), kNoDevice, 0.0, 0, err, sizeof err) == DC_ERR_FILE);
    CHECK(dc_load_utf8(pl, nullptr, kNoDevice, 0.0, 0, err, sizeof err) == DC_ERR_ARG);
    dc_set_next_utf8(pl, utf8.c_str());   // no player running: nothing happens
    dc_set_next_gain_utf8(pl, nullptr, 0.0);
    dc_destroy(pl);
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

TEST_CASE("C API: dc_filter_cache_dir_utf8 returns the folder in UTF-8", "[capi]") {
    const std::string name = "daphne_快取_é";
    const auto dir = std::filesystem::temp_directory_path() /
                     std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(name.data()), name.size()));
    test::set_env("DAPHNE_FILTER_CACHE", &dir);
    char out[1024] = {};
    const int n = dc_filter_cache_dir_utf8(out, sizeof out);
    const std::u8string want = dir.u8string();
    CHECK(n == static_cast<int>(want.size()));
    CHECK(std::string(out) == std::string(reinterpret_cast<const char*>(want.data()), want.size()));
    char small[4] = {};
    CHECK(dc_filter_cache_dir_utf8(small, sizeof small) == DC_ERR_BUFFER);
    const std::filesystem::path off("off");
    test::set_env("DAPHNE_FILTER_CACHE", &off);
    CHECK(dc_filter_cache_dir_utf8(out, sizeof out) == 0);   // the cache is disabled
    test::set_env("DAPHNE_FILTER_CACHE", nullptr);   // the tests after this one run as before
}

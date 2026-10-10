#include <catch_amalgamated.hpp>

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <dr_libs/dr_wav.h>

#include "daphne/engine/decoder.h"

namespace {

std::wstring temp_path(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + name;
}

// writes `frames` frames of a stereo integer ramp at `bits`, returns the samples written
std::vector<int32_t> write_int_wav(const std::wstring& path, int bits, int frames) {
    drwav_data_format fmt{};
    fmt.container = drwav_container_riff;
    fmt.format = DR_WAVE_FORMAT_PCM;
    fmt.channels = 2;
    fmt.sampleRate = 44100;
    fmt.bitsPerSample = static_cast<drwav_uint32>(bits);
    drwav w;
    REQUIRE(drwav_init_file_write_w(&w, path.c_str(), &fmt, nullptr));
    const int bytes = bits / 8;
    const int64_t full = (int64_t(1) << (bits - 1)) - 1;
    std::vector<int32_t> s(static_cast<size_t>(frames) * 2);
    std::vector<unsigned char> raw(s.size() * bytes);
    for (size_t i = 0; i < s.size(); ++i) {
        s[i] = static_cast<int32_t>(std::llround(full * std::sin(0.001 * double(i)) * (i & 1 ? -1 : 1)));
        for (int b = 0; b < bytes; ++b) raw[i * bytes + b] = static_cast<unsigned char>(s[i] >> (8 * b));
    }
    drwav_write_pcm_frames(&w, frames, raw.data());
    drwav_uninit(&w);
    return s;
}

void check_exact(const std::wstring& path, int bits, const std::vector<int32_t>& want) {
    std::string err;
    auto d = daphne::open_decoder(path, &err);
    REQUIRE(d);
    CHECK(d->info().bits == bits);
    CHECK(d->info().lossless_int);
    CHECK(d->info().frames == int64_t(want.size() / 2));
    std::vector<double> got(want.size());
    size_t at = 0;
    for (int n; (n = d->read(got.data() + at, 1000)) > 0;) at += size_t(n) * 2;
    REQUIRE(at == want.size());
    const double scale = std::ldexp(1.0, bits - 1);
    size_t bad = 0;
    for (size_t i = 0; i < want.size(); ++i)
        if (got[i] * scale != double(want[i])) ++bad;
    CHECK(bad == 0);
}

}  // namespace

TEST_CASE("WAV integer PCM decodes bit-exactly", "[decoder]") {
    for (int bits : {16, 24, 32}) {
        const auto p = temp_path((L"daphne_test_" + std::to_wstring(bits) + L".wav").c_str());
        const auto s = write_int_wav(p, bits, 10000);
        check_exact(p, bits, s);
        DeleteFileW(p.c_str());
    }
}

TEST_CASE("WAV seek lands on the requested frame", "[decoder]") {
    const auto p = temp_path(L"daphne_test_seek.wav");
    const auto s = write_int_wav(p, 24, 10000);
    auto d = daphne::open_decoder(p, nullptr);
    REQUIRE(d);
    REQUIRE(d->seek(4321));
    double f[2];
    REQUIRE(d->read(f, 1) == 1);
    CHECK(f[0] * 8388608.0 == double(s[4321 * 2]));
    CHECK(f[1] * 8388608.0 == double(s[4321 * 2 + 1]));
    d.reset();
    DeleteFileW(p.c_str());
}

TEST_CASE("unreadable file reports an error", "[decoder]") {
    const auto p = temp_path(L"daphne_test_junk.wav");
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD n;
    WriteFile(h, "not audio at all", 16, &n, nullptr);
    CloseHandle(h);
    std::string err;
    CHECK_FALSE(daphne::open_decoder(p, &err));
    CHECK_FALSE(err.empty());
    DeleteFileW(p.c_str());
}

TEST_CASE("WAV float64 decodes bit-exactly", "[decoder]") {
    const auto p = temp_path(L"daphne_test_f64.wav");
    drwav_data_format fmt{};
    fmt.container = drwav_container_riff;
    fmt.format = DR_WAVE_FORMAT_IEEE_FLOAT;
    fmt.channels = 2;
    fmt.sampleRate = 96000;
    fmt.bitsPerSample = 64;
    drwav w;
    REQUIRE(drwav_init_file_write_w(&w, p.c_str(), &fmt, nullptr));
    std::vector<double> x(20000);
    for (size_t i = 0; i < x.size(); ++i) x[i] = 0.7 * std::sin(0.0123456789 * double(i)) + 1e-12 * double(i % 7);
    drwav_write_pcm_frames(&w, x.size() / 2, x.data());
    drwav_uninit(&w);
    auto d = daphne::open_decoder(p, nullptr);
    REQUIRE(d);
    CHECK_FALSE(d->info().lossless_int);
    std::vector<double> got(x.size());
    size_t at = 0;
    for (int n; (n = d->read(got.data() + at, 777)) > 0;) at += size_t(n) * 2;
    REQUIRE(at == x.size());
    CHECK(got == x);
    d.reset();
    DeleteFileW(p.c_str());
}

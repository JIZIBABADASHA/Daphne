// daphne: command-line player and test tool.

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <dr_libs/dr_flac.h>
#include <dr_libs/dr_wav.h>

#include "daphne/dsp/filter_design.h"
#include "daphne/dsp/modulator.h"
#include "daphne/dsp/progress.h"
#include "daphne/engine/chain.h"
#include "daphne/engine/decoder.h"
#include "daphne/engine/devices.h"
#include "daphne/engine/dsd_output.h"
#include "daphne/engine/player.h"
#include "daphne/engine/dsd_file.h"
#include "daphne/engine/filters.h"
#include "daphne/engine/settings.h"
#include "daphne/engine/text.h"

namespace {

constexpr const char* kVersion = "0.1.0";
std::atomic<bool> g_cancel{false};

BOOL WINAPI on_ctrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        g_cancel = true;
        return TRUE;
    }
    return FALSE;
}

void usage() {
    std::cout << "daphne " << kVersion << "\n\n"
              << "usage:\n"
              << "  daphne version\n"
              << "  daphne devices [--probe]                list WASAPI endpoints (--probe: exclusive formats) and ASIO drivers\n"
              << "  daphne play [--settings file] [--device id|index] [--shared|--exclusive|--asio [driver]]\n"
              << "              [--start S] [--length S] [--next file] [--watch] [--script t:cmd,...] files...\n"
              << "  daphne render-dsp <in> <out.wav> [--settings file] [--channels N] [--float]\n"
              << "                                          the whole chain offline (dsd core: the DoP stream)\n"
              << "  daphne decode-test [--md5] files...     decode files; FLAC is checked against its STREAMINFO MD5;\n"
              << "                                          --md5 prints the MD5 of the decoded stream as s32le\n"
              << "  daphne render <in> <out.wav> [--start F] [--dsd-lowpass 24|30|50]\n"
              << "                                          decode to WAV (source bit depth, or float32 otherwise);\n"
              << "                                          --start: seek to frame F first; --dsd-lowpass: DSD -> PCM low-pass\n"
              << "  daphne settings default [file]          write the default settings (stdout if no file)\n"
              << "  daphne settings check <file>            validate a settings file\n";
}

// ---------------------------------------------------------------- MD5 (BCrypt)

class Md5 {
public:
    Md5() {
        BCryptOpenAlgorithmProvider(&alg_, BCRYPT_MD5_ALGORITHM, nullptr, 0);
        BCryptCreateHash(alg_, &h_, nullptr, 0, nullptr, 0, 0);
    }
    ~Md5() {
        if (h_) BCryptDestroyHash(h_);
        if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
    }
    void update(const void* p, size_t n) { BCryptHashData(h_, (PUCHAR)p, static_cast<ULONG>(n), 0); }
    std::string hex() {
        unsigned char d[16];
        BCryptFinishHash(h_, d, 16, 0);
        char s[33];
        for (int i = 0; i < 16; ++i) snprintf(s + i * 2, 3, "%02x", d[i]);
        return s;
    }

private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE h_ = nullptr;
};

void on_flac_meta(void* user, drflac_metadata* m) {
    if (m->type == DRFLAC_METADATA_BLOCK_TYPE_STREAMINFO) std::memcpy(user, m->data.streaminfo.md5, 16);
}

std::string flac_streaminfo_md5(const std::wstring& path) {
    unsigned char md5[16] = {};
    drflac* f = drflac_open_file_with_metadata_w(path.c_str(), on_flac_meta, md5, nullptr);
    if (!f) return {};
    drflac_close(f);
    char s[33];
    for (int i = 0; i < 16; ++i) snprintf(s + i * 2, 3, "%02x", md5[i]);
    return s;
}

std::wstring suffix_lower(const std::wstring& p) {
    auto d = p.find_last_of(L'.');
    std::wstring s = d == std::wstring::npos ? L"" : p.substr(d + 1);
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

// ---------------------------------------------------------------- commands

int cmd_decode_test(const std::vector<std::wstring>& args) {
    bool md5 = false;
    std::vector<std::wstring> files;
    for (const auto& a : args) {
        if (a == L"--md5") md5 = true;
        else files.push_back(a);
    }
    int bad = 0;
    for (const auto& f : files) {
        std::string err;
        auto d = daphne::open_decoder(f, &err);
        std::cout << daphne::to_utf8(f) << "\n";
        if (!d) {
            std::cout << "  error: " << err << "\n";
            ++bad;
            continue;
        }
        const auto& in = d->info();
        std::cout << "  " << in.codec << " " << in.rate << " Hz, " << in.channels << " ch"
                  << (in.bits ? ", " + std::to_string(in.bits) + "-bit" : "") << ", frames " << in.frames << "\n";
        const bool is_flac = suffix_lower(f) == L"flac" || suffix_lower(f) == L"fla";
        Md5 flac_md5, s32_md5;
        const int ch = in.channels, bps = in.bits;
        std::vector<double> buf(4096 * static_cast<size_t>(ch));
        std::vector<unsigned char> bytes;
        int64_t total = 0;
        double peak = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            const int n = d->read(buf.data(), 4096);
            if (n <= 0) break;
            total += n;
            const size_t ns = static_cast<size_t>(n) * ch;
            for (size_t i = 0; i < ns; ++i) peak = std::max(peak, std::abs(buf[i]));
            if (is_flac && bps > 0) {
                // FLAC MD5: signed samples at the source width, little-endian, ceil(bps/8) bytes each
                const int w = (bps + 7) / 8;
                bytes.resize(ns * w);
                for (size_t i = 0; i < ns; ++i) {
                    const int64_t s = static_cast<int64_t>(std::llround(buf[i] * 2147483648.0)) >> (32 - bps);
                    for (int b = 0; b < w; ++b) bytes[i * w + b] = static_cast<unsigned char>(s >> (8 * b));
                }
                flac_md5.update(bytes.data(), bytes.size());
            }
            if (md5) {
                bytes.resize(ns * 4);
                for (size_t i = 0; i < ns; ++i) {
                    double v = std::nearbyint(buf[i] * 2147483648.0);
                    v = std::min(2147483647.0, std::max(-2147483648.0, v));
                    const int32_t s = static_cast<int32_t>(v);
                    std::memcpy(bytes.data() + i * 4, &s, 4);
                }
                s32_md5.update(bytes.data(), bytes.size());
            }
        }
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "  decoded " << total << " frames, peak " << peak << ", "
                  << (in.rate && secs > 0 ? total / double(in.rate) / secs : 0) << "x realtime\n";
        if (in.frames >= 0 && total != in.frames) std::cout << "  WARNING: frame count " << total << " != header " << in.frames << "\n";
        if (is_flac && bps > 0) {
            const std::string want = flac_streaminfo_md5(f), got = flac_md5.hex();
            const bool unset = want == std::string(32, '0');
            std::cout << "  FLAC MD5 " << got << (unset ? " (STREAMINFO MD5 not set)" : got == want ? " OK" : " MISMATCH (want " + want + ")")
                      << "\n";
            if (!unset && got != want) ++bad;
        }
        if (md5) std::cout << "  s32le MD5 " << s32_md5.hex() << "\n";
    }
    return bad ? 2 : 0;
}

int cmd_render(const std::wstring& in_path, const std::wstring& out_path, int64_t start, int dsd_lowpass) {
    std::string err;
    std::unique_ptr<daphne::Decoder> d;
    if (dsd_lowpass > 0 && daphne::is_dsd_path(in_path)) {
        d = daphne::make_dsd_pcm_decoder(0, dsd_lowpass);
        if (!d->open(in_path, &err)) d.reset();
    } else {
        d = daphne::open_decoder(in_path, &err);
    }
    if (!d) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }
    if (start > 0 && !d->seek(start)) {
        std::cerr << "error: cannot seek to frame " << start << "\n";
        return 1;
    }
    const auto& in = d->info();
    const bool as_int = in.lossless_int && in.bits > 0;
    const int bits = as_int ? (in.bits <= 16 ? 16 : in.bits <= 24 ? 24 : 32) : 32;
    drwav_data_format fmt{};
    fmt.container = drwav_container_riff;
    fmt.format = as_int ? DR_WAVE_FORMAT_PCM : DR_WAVE_FORMAT_IEEE_FLOAT;
    fmt.channels = static_cast<drwav_uint32>(in.channels);
    fmt.sampleRate = static_cast<drwav_uint32>(in.rate);
    fmt.bitsPerSample = static_cast<drwav_uint32>(bits);
    drwav w;
    if (!drwav_init_file_write_w(&w, out_path.c_str(), &fmt, nullptr)) {
        std::cerr << "error: cannot write " << daphne::to_utf8(out_path) << "\n";
        return 1;
    }
    const int ch = in.channels;
    std::vector<double> buf(4096 * static_cast<size_t>(ch));
    std::vector<unsigned char> bytes(4096 * static_cast<size_t>(ch) * 4);
    int64_t total = 0;
    const int w_bytes = bits / 8;
    for (;;) {
        const int n = d->read(buf.data(), 4096);
        if (n <= 0) break;
        const size_t ns = static_cast<size_t>(n) * ch;
        for (size_t i = 0; i < ns; ++i) {
            if (as_int) {
                const int64_t s = static_cast<int64_t>(std::llround(buf[i] * 2147483648.0)) >> (32 - bits);
                for (int b = 0; b < w_bytes; ++b) bytes[i * w_bytes + b] = static_cast<unsigned char>(s >> (8 * b));
            } else {
                const float v = static_cast<float>(buf[i]);
                std::memcpy(bytes.data() + i * 4, &v, 4);
            }
        }
        drwav_write_pcm_frames(&w, n, bytes.data());
        total += n;
    }
    drwav_uninit(&w);
    std::cout << "rendered " << total << " frames (" << in.codec << " -> "
              << (as_int ? std::to_string(bits) + "-bit PCM" : "float32") << ")\n";
    return 0;
}

// Hidden test hook: daphne filter-dump <fs_in> <fs_out> <out.f64> [<lo.f64>] [--taps N] [--phase P] [--apodize]
// Writes the resampling filter as raw float64 (and the double-double low parts with lo.f64).
int cmd_filter_dump(std::vector<std::wstring> args) {
    int taps = 4096;
    double phase = 0.0;
    bool apodize = false;
    for (size_t i = 0; i < args.size();) {
        if (args[i] == L"--apodize") {
            apodize = true;
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(i));
        } else if ((args[i] == L"--taps" || args[i] == L"--phase") && i + 1 < args.size()) {
            if (args[i] == L"--taps") taps = std::stoi(args[i + 1]);
            else phase = std::stod(args[i + 1]);
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(i), args.begin() + static_cast<std::ptrdiff_t>(i) + 2);
        } else {
            ++i;
        }
    }
    if (args.size() < 3) {
        usage();
        return 1;
    }
    const int fs_in = std::stoi(args[0]), fs_out = std::stoi(args[1]);
    const int ratio = std::max(1, fs_out / fs_in);
    std::vector<double> lo;
    const bool want_lo = args.size() >= 4;
    const auto t0 = std::chrono::steady_clock::now();
    const auto h = daphne::dsp::design_lowpass(taps, ratio, fs_in, fs_out, phase, apodize, want_lo ? &lo : nullptr);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::ofstream o(args[2], std::ios::binary);
    o.write(reinterpret_cast<const char*>(h.data()), static_cast<std::streamsize>(h.size() * sizeof(double)));
    if (want_lo) {
        std::ofstream ol(args[3], std::ios::binary);
        ol.write(reinterpret_cast<const char*>(lo.data()), static_cast<std::streamsize>(lo.size() * sizeof(double)));
    }
    std::cout << "taps " << h.size() << ", designed in " << secs << " s\n";
    return 0;
}

// Hidden test hook: daphne modulator-test <fs> <osr> <order> <channels> <in.f64> <out.bin> [chunk] [--single]
// Runs the DSD modulator over raw interleaved float64 and writes the DSD bytes; prints the time and
// the protection counters. --single: both channels on the calling thread.
int cmd_modulator_test(std::vector<std::wstring> a) {
    bool single = false;
    a.erase(std::remove_if(a.begin(), a.end(), [&](const std::wstring& x) { return x == L"--single" ? (single = true) : false; }), a.end());
    if (a.size() < 6) return 1;
    const int fs = std::stoi(a[0]), osr = std::stoi(a[1]), order = std::stoi(a[2]), ch = std::stoi(a[3]);
    const int chunk = a.size() > 6 ? std::stoi(a[6]) : 1 << 30;
    std::ifstream in(a[4], std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), {});
    std::vector<double> x(raw.size() / 8);
    std::memcpy(x.data(), raw.data(), x.size() * 8);
    const int frames = static_cast<int>(x.size() / ch);
    daphne::dsp::DsdModulator m(fs, osr, order);
    m.set_threaded(!single);
    std::vector<uint8_t> out;
    out.reserve(static_cast<size_t>(static_cast<double>(frames) * m.dsd_rate() / fs / 8 * ch) + 64);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; i += chunk) {
        const int n = std::min(chunk, frames - i);
        m.process(x.data() + static_cast<size_t>(i) * ch, n, ch, out);
    }
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::ofstream o(a[5], std::ios::binary);
    o.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    std::cout << "order " << m.order() << " dsd " << m.dsd_rate() << " bytes " << out.size() << " seconds " << sec << " ("
              << (frames / double(fs)) / sec << "x realtime) clamped " << m.clamped_bits() << " resets " << m.loop_resets()
              << " input clips " << m.input_clips() << "\n";
    return 0;
}

// --settings <file>: loads the settings, prints the diagnostics; removes the option from args.
bool load_settings_arg(std::vector<std::wstring>& args, daphne::Settings& s) {
    std::vector<std::wstring> rest;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == L"--settings" && i + 1 < args.size()) {
            std::ifstream f(args[++i], std::ios::binary);
            if (!f) {
                std::cerr << "cannot read " << daphne::to_utf8(args[i]) << "\n";
                return false;
            }
            std::stringstream ss;
            ss << f.rdbuf();
            auto res = daphne::settings_from_string(ss.str());
            for (const auto& d : res.diagnostics)
                std::cerr << (d.level == daphne::Diagnostic::Level::Error ? "settings error   " : "settings warning ")
                          << (d.path.empty() ? "(root)" : d.path) << ": " << d.message << "\n";
            if (!res.ok()) return false;
            s = res.settings;
        } else {
            rest.push_back(args[i]);
        }
    }
    args = rest;
    return true;
}

int cmd_settings(const std::vector<std::wstring>& a) {
    if (!a.empty() && a[0] == L"default") {
        const auto text = daphne::settings_to_json(daphne::Settings{}).dump(2) + "\n";
        if (a.size() >= 2) {
            std::ofstream o(a[1], std::ios::binary);
            o << text;
        } else {
            std::cout << text;
        }
        return 0;
    }
    if (a.size() >= 2 && a[0] == L"check") {
        std::vector<std::wstring> args = {L"--settings", a[1]};
        daphne::Settings s;
        if (!load_settings_arg(args, s)) return 1;
        std::cout << "ok\n";
        return 0;
    }
    usage();
    return 1;
}

// Hidden test hook: daphne resampler-run <fs_in> <fs_out> <in.f64> <out.f64> [--settings file]
// Runs the resampler the player would build from the settings' filter (no other stage) over raw
// interleaved stereo float64, and prints the time it took.
int cmd_resampler_run(std::vector<std::wstring> args) {
    daphne::Settings s;
    if (!load_settings_arg(args, s) || args.size() < 4) return 1;
    const int fs_in = std::stoi(args[0]), fs_out = std::stoi(args[1]);
    std::ifstream fi(args[2], std::ios::binary | std::ios::ate);
    const auto bytes = static_cast<size_t>(fi.tellg());
    std::vector<double> x(bytes / sizeof(double), 0.0);
    fi.seekg(0);
    fi.read(reinterpret_cast<char*>(x.data()), static_cast<std::streamsize>(x.size() * sizeof(double)));
    const int frames = static_cast<int>(x.size() / 2);
    daphne::dsp::set_design_threads(s.design.threads);   // also the double-double phases' threads
    auto r = daphne::make_resampler(s.filter, fs_in, fs_out);
    r->prepare({fs_in, 2, 4096});
    std::vector<double> y(static_cast<size_t>(r->max_output_frames(frames) + r->max_output_frames(4096)) * 2);
    int done = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; i += 4096) {
        const int n = std::min(4096, frames - i);
        done += r->process(x.data() + static_cast<size_t>(i) * 2, n, y.data() + static_cast<size_t>(done) * 2);
    }
    done += r->flush(y.data() + static_cast<size_t>(done) * 2);
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::ofstream o(args[3], std::ios::binary);
    o.write(reinterpret_cast<const char*>(y.data()), static_cast<std::streamsize>(static_cast<size_t>(done) * 2 * sizeof(double)));
    std::cout << "frames " << done << " double_double " << r->double_double() << " seconds " << sec << "\n";
    return 0;
}

// ---------------------------------------------------------------- devices / play / render-dsp

int cmd_devices(bool probe) {
    std::string err;
    auto devs = daphne::list_wasapi_devices(probe, &err);
    std::cout << "WASAPI render endpoints (" << devs.size() << ")\n";
    if (!err.empty()) std::cout << "  error: " << err << "\n";
    for (size_t i = 0; i < devs.size(); ++i) {
        const auto& d = devs[i];
        std::cout << "  [" << i << "] " << d.name << (d.is_default ? "  (default)" : "") << "\n"
                  << "      id:  " << d.id << "\n"
                  << "      mix: " << d.mix_rate << " Hz, " << d.mix_channels << " ch, " << d.mix_bits << "-bit\n";
        if (probe) {
            std::cout << "      exclusive:";
            if (d.exclusive.empty()) std::cout << " (none)";
            int last_rate = 0;
            for (const auto& f : d.exclusive) {
                if (f.rate != last_rate) {
                    std::cout << "\n        " << f.rate << ":";
                    last_rate = f.rate;
                }
                std::cout << " " << f.bits;
                if (f.valid_bits != f.bits) std::cout << "/" << f.valid_bits;
            }
            std::cout << "\n";
        }
    }
    std::string aerr;
    auto asio = daphne::list_asio_drivers(&aerr);
    std::cout << "ASIO drivers (" << asio.size() << ")\n";
    if (!aerr.empty()) std::cout << "  " << aerr << "\n";
    for (const auto& a : asio)
        std::cout << "  " << a.name << (a.description.empty() || a.description == a.name ? "" : "  - " + a.description)
                  << "\n      clsid: " << a.clsid << "\n";
    return 0;
}

struct PlayConfig {
    daphne::Settings settings;
    daphne::OutputChoice out;
    std::vector<std::wstring> files;
    std::wstring settings_path;
    bool watch = false;
    double start = 0.0, length = 0.0;   // --start / --length seconds
    std::wstring next;                  // --next: gapless next track
    std::string script;                 // --script "2:pause,3:resume,5:seek=30" (seconds since start)
};

// Settings file + device options; called again when --watch sees the file change.
bool play_config(std::vector<std::wstring> args, PlayConfig& c) {
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == L"--settings") c.settings_path = args[i + 1];
    c.settings = {};
    if (!load_settings_arg(args, c.settings)) return false;
    c.out.api = c.settings.output.api;
    c.out.device = c.settings.output.device;
    c.files.clear();
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == L"--device" && i + 1 < args.size()) c.out.device = daphne::to_utf8(args[++i]);
        else if (args[i] == L"--shared") c.out.api = daphne::OutputApi::WasapiShared;
        else if (args[i] == L"--exclusive") c.out.api = daphne::OutputApi::WasapiExclusive;
        else if (args[i] == L"--watch") c.watch = true;
        else if (args[i] == L"--start" && i + 1 < args.size()) c.start = std::stod(args[++i]);
        else if (args[i] == L"--length" && i + 1 < args.size()) c.length = std::stod(args[++i]);
        else if (args[i] == L"--next" && i + 1 < args.size()) c.next = args[++i];
        else if (args[i] == L"--script" && i + 1 < args.size()) c.script = daphne::to_utf8(args[++i]);
        else if (args[i] == L"--asio") {
            c.out.api = daphne::OutputApi::Asio;
            // optional driver name / index right after --asio
            if (i + 1 < args.size() && args[i + 1].rfind(L"--", 0) != 0 && !std::filesystem::exists(args[i + 1]))
                c.out.device = daphne::to_utf8(args[++i]);
        } else c.files.push_back(args[i]);
    }
    return true;
}

int cmd_play(std::vector<std::wstring> args) {
    PlayConfig cfg;
    if (!play_config(args, cfg)) return 1;
    if (cfg.files.empty()) {
        usage();
        return 1;
    }
    if (cfg.watch && cfg.settings_path.empty()) {
        std::cerr << "--watch needs --settings <file>\n";
        return 1;
    }
    auto mtime = [&]() {
        std::error_code ec;
        const auto t = std::filesystem::last_write_time(cfg.settings_path, ec);
        return ec ? std::filesystem::file_time_type{} : t;
    };
    int failures = 0;
    for (const auto& f : cfg.files) {
        if (g_cancel) break;
        std::cout << "\n" << daphne::to_utf8(f) << "\n";
        daphne::PlayCallbacks cb;
        daphne::OutputApi api = cfg.out.api;
        cb.on_start = [&](const daphne::PlayReport& r) {
            if (r.source.dsd_rate > 0)
                std::cout << "  source: " << r.source.codec << " DSD" << r.source.dsd_rate / 44100 << " (" << r.source.dsd_rate << " Hz), "
                          << r.source.channels << " ch\n";
            else
                std::cout << "  source: " << r.source.codec << " " << r.source.rate << " Hz, " << r.source.channels << " ch"
                          << (r.source.bits ? ", " + std::to_string(r.source.bits) + "-bit" : "") << "\n";
            std::cout << "  device: " << r.device_name << " | "
                      << (api == daphne::OutputApi::Asio ? "ASIO" : api == daphne::OutputApi::WasapiShared ? "shared" : "exclusive")
                      << " | " << daphne::describe(r.device) << " | latency " << r.latency_ms << " ms\n"
                      << "  bit-perfect: " << (r.bit_perfect ? "yes" : "no (" + r.not_bit_perfect_reason + ")") << "\n";
            std::string chain;
            for (const auto& st : r.chain) chain += (chain.empty() ? "" : " -> ") + st;
            std::cout << "  filter: " << r.filter << "\n"
                      << "  dsp: " << (chain.empty() ? "none" : chain) << std::endl;
        };
        bool design_line = false;
        cb.on_event = [&](const std::string& e) {
            if (design_line) std::cout << "\n";
            design_line = false;
            std::cout << "  * " << e << std::endl;
        };
        cb.on_design = [&](double fr) {
            if (fr >= 1.0) {
                if (design_line) std::cout << "\r  designing filter done" << std::string(40, ' ') << std::endl;
                design_line = false;
                return;
            }
            const int pct = static_cast<int>(fr * 100.0);
            const int bar = static_cast<int>(fr * 30.0);
            std::cout << "\r  designing filter [" << std::string(bar, '#') << std::string(30 - bar, '.') << "] " << pct << "%   "
                      << std::flush;
            design_line = true;
        };
        cb.on_track_started = [&](const daphne::PlayReport& r) {
            std::cout << "  * now playing the next track (" << r.source.codec << ", " << r.source.rate << " Hz)" << std::endl;
        };
        daphne::Player player;
        std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        if (!player.start(f, cfg.settings, cfg.out, cb, &err, cfg.start)) {
            std::cout << "  error: " << err << "\n";
            ++failures;
            continue;
        }
        auto seen = mtime();
        if (!cfg.next.empty()) player.set_next(cfg.next);
        // --script: "t:pause", "t:resume", "t:seek=S" at t seconds after the start
        struct Step {
            double t;
            std::string cmd;
        };
        std::vector<Step> steps;
        for (size_t a = 0; a < cfg.script.size();) {
            size_t b = cfg.script.find(',', a);
            if (b == std::string::npos) b = cfg.script.size();
            const std::string item = cfg.script.substr(a, b - a);
            const size_t colon = item.find(':');
            if (colon != std::string::npos) steps.push_back({std::stod(item.substr(0, colon)), item.substr(colon + 1)});
            a = b + 1;
        }
        size_t next_step = 0;
        while (!player.finished()) {
            const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            while (next_step < steps.size() && el >= steps[next_step].t) {
                const std::string& cmd = steps[next_step++].cmd;
                if (cmd == "pause") player.pause(true);
                else if (cmd == "resume") player.pause(false);
                else if (cmd.rfind("seek=", 0) == 0) player.seek(std::stod(cmd.substr(5)));
                double ml = 0, mr = 0;
                player.meter(ml, mr);
                std::cout << "  > " << cmd << " at " << el << " s (position " << player.position_seconds() << " s, peak " << ml << ")"
                          << std::endl;
            }
            if (g_cancel) player.stop();
            if (cfg.length > 0 && el > cfg.length) player.stop();
            Sleep(200);
            if (cfg.watch && mtime() != seen) {
                seen = mtime();
                Sleep(100);   // let the editor finish writing
                PlayConfig nc;
                if (play_config(args, nc)) {
                    api = nc.out.api;
                    std::cout << "  * settings file changed\n";
                    player.apply(nc.settings, nc.out);
                } else {
                    std::cout << "  * settings file has errors; keeping the current settings\n";
                }
            }
        }
        const double end_pos = player.position_seconds();
        const bool ended = player.ended();
        daphne::PlayReport rep;
        if (!player.wait(&rep, &err)) {
            std::cout << "  error: " << err << "\n";
            ++failures;
            continue;
        }
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "  played " << rep.frames_played << " frames in " << secs << " s | underruns " << rep.underruns
                  << " | event timeouts " << rep.event_timeouts << " | max event interval " << rep.max_interval_us / 1000.0
                  << " ms | clipped " << rep.clipped << (g_cancel ? " | cancelled" : "") << (ended ? " | ended" : "")
                  << " | last position " << end_pos << " s\n";
    }
    return failures ? 2 : 0;
}

// daphne render-dsp <in> <out.wav> [--settings file] [--channels N] [--float]
// Runs the whole chain offline: a virtual device at the rate the core asks for, N channels (default:
// the source's), output.bits integer PCM (or float32). dsd core: the DoP stream (32-bit WAV at the
// DSD clock / 16) as the player sends it.
int cmd_render_dsp(std::vector<std::wstring> args) {
    daphne::Settings s;
    if (!load_settings_arg(args, s)) return 1;
    int channels = 0;
    bool as_float = false;
    std::vector<std::wstring> files;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == L"--channels" && i + 1 < args.size()) channels = std::stoi(args[++i]);
        else if (args[i] == L"--float") as_float = true;
        else files.push_back(args[i]);
    }
    if (files.size() != 2) {
        usage();
        return 1;
    }
    std::string err;
    auto d = daphne::open_decoder(files[0], &err);
    if (!d) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }
    const auto& in = d->info();
    daphne::DeviceFormat dev;
    dev.rate = daphne::pcm_output_rate(in.rate, s.output.rate);
    dev.channels = channels > 0 ? channels : in.channels;
    if (as_float) dev.type = daphne::SampleType::Float32, dev.container_bits = dev.valid_bits = 32;
    else if (s.output.bits == 16) dev.type = daphne::SampleType::Int16, dev.container_bits = dev.valid_bits = 16;
    else if (s.output.bits == 24) dev.type = daphne::SampleType::Packed24, dev.container_bits = dev.valid_bits = 24;
    else dev.type = daphne::SampleType::Int32, dev.container_bits = dev.valid_bits = 32;
    constexpr int kChunk = 4096;
    std::unique_ptr<daphne::DsdEncoder> dsd;
    daphne::DeviceFormat chain_fmt = dev;
    if (s.core == daphne::CoreMode::Dsd) {
        dev.rate = daphne::dop_rate(daphne::dsd_clock(s.modulator.osr));
        dev.type = daphne::SampleType::Int32, dev.container_bits = dev.valid_bits = 32;
        chain_fmt = dev;
        chain_fmt.rate = daphne::modulator_input_rate(in.rate);
        chain_fmt.type = daphne::SampleType::Float64, chain_fmt.container_bits = chain_fmt.valid_bits = 64;
        dsd = std::make_unique<daphne::DsdEncoder>();
        if (!dsd->init(chain_fmt.rate, s.modulator.osr, s.modulator.order, dev, daphne::DsdTransport::Dop, &err)) {
            std::cerr << "error: " << err << "\n";
            return 1;
        }
    }
    daphne::DspChain chain;
    if (!chain.build(s, in, chain_fmt, kChunk, &err)) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }
    std::string stages;
    for (const auto& st : chain.stages()) stages += (stages.empty() ? "" : " -> ") + st;
    std::cout << in.codec << " " << in.rate << " Hz " << in.channels << " ch -> " << daphne::describe(dev) << "\n"
              << "dsp: " << (stages.empty() ? "none" : stages) << "\n";
    if (dsd) std::cout << "dsd: " << dsd->describe() << "\n";

    drwav_data_format fmt{};
    fmt.container = drwav_container_riff;
    fmt.format = as_float ? DR_WAVE_FORMAT_IEEE_FLOAT : DR_WAVE_FORMAT_PCM;
    fmt.channels = static_cast<drwav_uint32>(dev.channels);
    fmt.sampleRate = static_cast<drwav_uint32>(dev.rate);
    fmt.bitsPerSample = static_cast<drwav_uint32>(dev.container_bits);
    drwav w;
    if (!drwav_init_file_write_w(&w, files[1].c_str(), &fmt, nullptr)) {
        std::cerr << "error: cannot write " << daphne::to_utf8(files[1]) << "\n";
        return 1;
    }
    std::vector<double> buf(static_cast<size_t>(kChunk) * in.channels);
    std::vector<double> out(static_cast<size_t>(chain.max_output_frames()) * dev.channels);
    std::vector<uint8_t> bytes(static_cast<size_t>(chain.max_output_frames()) * dev.block_align());
    int64_t total = 0, clipped = 0;
    std::vector<uint8_t> dop;
    auto emit = [&](int n) {
        if (dsd) {
            dop.clear();
            dsd->encode(out.data(), n, dop);
            drwav_write_pcm_frames(&w, static_cast<drwav_uint64>(dop.size() / dev.block_align()), dop.data());
            total += static_cast<int64_t>(dop.size() / dev.block_align());
            return;
        }
        clipped += daphne::convert_to_device(out.data(), static_cast<size_t>(n) * dev.channels, dev, bytes.data());
        drwav_write_pcm_frames(&w, static_cast<drwav_uint64>(n), bytes.data());
        total += n;
    };
    for (;;) {
        const int n = d->read(buf.data(), kChunk);
        if (n <= 0) break;
        emit(chain.process(buf.data(), n, out.data()));
    }
    emit(chain.flush(out.data()));
    drwav_uninit(&w);
    std::cout << "rendered " << total << " frames, clipped " << clipped << "\n";
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    std::vector<std::wstring> a(argv + 1, argv + argc);
    if (a.empty() || a[0] == L"help" || a[0] == L"--help" || a[0] == L"-h") {
        usage();
        return a.empty() ? 1 : 0;
    }
    const std::vector<std::wstring> rest(a.begin() + 1, a.end());
    if (a[0] == L"version") {
        std::cout << "daphne " << kVersion << "\n";
        return 0;
    }
    if (a[0] == L"decode-test") return cmd_decode_test(rest);
    if (a[0] == L"devices") return cmd_devices(!rest.empty() && rest[0] == L"--probe");
    if (a[0] == L"play") return cmd_play(rest);
    if (a[0] == L"render-dsp") return cmd_render_dsp(rest);
    if (a[0] == L"filter-dump") return cmd_filter_dump(rest);
    if (a[0] == L"resampler-run") return cmd_resampler_run(rest);
    if (a[0] == L"modulator-test") return cmd_modulator_test(rest);
    if (a[0] == L"settings") return cmd_settings(rest);
    if (a[0] == L"render") {
        std::vector<std::wstring> r = rest;
        int64_t start = 0;
        int dsd_lowpass = 0;
        for (size_t i = 0; i + 1 < r.size();)
            if (r[i] == L"--start" || r[i] == L"--dsd-lowpass") {
                if (r[i] == L"--start") start = std::stoll(r[i + 1]);
                else dsd_lowpass = std::stoi(r[i + 1]);
                r.erase(r.begin() + static_cast<std::ptrdiff_t>(i), r.begin() + static_cast<std::ptrdiff_t>(i) + 2);
            } else {
                ++i;
            }
        if (r.size() != 2) {
            usage();
            return 1;
        }
        return cmd_render(r[0], r[1], start, dsd_lowpass);
    }
    usage();
    return 1;
}

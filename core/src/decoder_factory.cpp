#include <algorithm>
#include <cwctype>

#include "daphne/engine/decoder.h"
#include "daphne/engine/dsd_file.h"
#include "daphne/engine/live_source.h"

namespace daphne {
namespace {

std::wstring lower_suffix(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    const auto dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return {};
    std::wstring s = path.substr(dot + 1);
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

bool any_of(const std::wstring& s, std::initializer_list<const wchar_t*> list) {
    return std::any_of(list.begin(), list.end(), [&](const wchar_t* x) { return s == x; });
}

}  // namespace

std::unique_ptr<Decoder> open_decoder(const std::wstring& path, std::string* error) {
    if (path == kLivePath) {   // the host's live input (live_source.h)
        auto l = make_live_decoder();
        if (l->open(path, error)) return l;
        return nullptr;
    }
    const std::wstring ext = lower_suffix(path);
    std::unique_ptr<Decoder> d;
    const bool dsd = is_dsd_path(path);
    if (any_of(ext, {L"wav", L"wave", L"bwf", L"aiff", L"aif", L"aifc"})) d = make_wav_decoder();
    else if (any_of(ext, {L"flac", L"fla"})) d = make_flac_decoder();
    else if (any_of(ext, {L"mp3", L"mp2", L"mp1"})) d = make_mp3_decoder();
    else if (dsd) d = make_dsd_pcm_decoder();
    else d = make_ffmpeg_decoder();   // FFmpeg probes the content
    std::string e;
    if (d->open(path, &e)) return d;
    // a file the dedicated decoder cannot read (an unusual WAV codec, FLAC data named .wav): FFmpeg once more
    if (!dsd) {
        auto f = make_ffmpeg_decoder();
        if (f->open(path, nullptr)) return f;
    }
    if (error) *error = e;
    return nullptr;
}

}  // namespace daphne

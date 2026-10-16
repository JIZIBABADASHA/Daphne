#pragma once
// Core Audio HAL property helpers shared by the output and the device list (macOS only).

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <cmath>
#include <string>
#include <vector>

namespace daphne::ca {

// kAudioObjectPropertyElementMain (macOS 12 SDK; ...Master before it, deprecated since): the value is 0
constexpr AudioObjectPropertyElement kMain = 0;

inline AudioObjectPropertyAddress addr(AudioObjectPropertySelector s, AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    return {s, scope, kMain};
}

template <class T>
bool get(AudioObjectID o, AudioObjectPropertySelector s, T& v, AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    const auto a = addr(s, scope);
    UInt32 n = sizeof(T);
    return AudioObjectHasProperty(o, &a) && AudioObjectGetPropertyData(o, &a, 0, nullptr, &n, &v) == noErr && n == sizeof(T);
}

template <class T>
OSStatus set(AudioObjectID o, AudioObjectPropertySelector s, const T& v, AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    const auto a = addr(s, scope);
    return AudioObjectSetPropertyData(o, &a, 0, nullptr, sizeof(T), &v);
}

template <class T>
std::vector<T> list(AudioObjectID o, AudioObjectPropertySelector s, AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    const auto a = addr(s, scope);
    UInt32 n = 0;
    if (!AudioObjectHasProperty(o, &a) || AudioObjectGetPropertyDataSize(o, &a, 0, nullptr, &n) != noErr || n == 0) return {};
    std::vector<T> v(n / sizeof(T));
    if (AudioObjectGetPropertyData(o, &a, 0, nullptr, &n, v.data()) != noErr) return {};
    v.resize(n / sizeof(T));
    return v;
}

inline std::string utf8(CFStringRef s) {
    if (!s) return {};
    const CFIndex len = CFStringGetLength(s);
    std::string out(static_cast<size_t>(CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8)) + 1, '\0');
    if (!CFStringGetCString(s, out.data(), static_cast<CFIndex>(out.size()), kCFStringEncodingUTF8)) return {};
    out.resize(std::char_traits<char>::length(out.c_str()));
    return out;
}

inline std::string string_prop(AudioObjectID o, AudioObjectPropertySelector s) {
    CFStringRef v = nullptr;
    if (!get(o, s, v) || !v) return {};
    std::string r = utf8(v);
    CFRelease(v);
    return r;
}

inline std::vector<AudioStreamID> output_streams(AudioDeviceID d) {
    return list<AudioStreamID>(d, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput);
}

inline int output_channels(AudioDeviceID d) {
    int n = 0;
    for (auto s : output_streams(d)) {
        AudioStreamBasicDescription f{};
        if (get(s, kAudioStreamPropertyVirtualFormat, f)) n += static_cast<int>(f.mChannelsPerFrame);
    }
    return n;
}

inline std::vector<AudioDeviceID> output_devices() {
    std::vector<AudioDeviceID> r;
    for (auto d : list<AudioDeviceID>(kAudioObjectSystemObject, kAudioHardwarePropertyDevices))
        if (!output_streams(d).empty()) r.push_back(d);
    return r;
}

inline AudioDeviceID default_output() {
    AudioDeviceID d = kAudioObjectUnknown;
    get(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, d);
    return d;
}

inline double nominal_rate(AudioDeviceID d) {
    Float64 r = 0;
    get(d, kAudioDevicePropertyNominalSampleRate, r);
    return r;
}

inline bool same_rate(double a, double b) { return std::fabs(a - b) < 0.5; }

// The standard rates the device offers (its nominal rate ranges).
inline std::vector<int> offered_rates(AudioDeviceID d) {
    static const int kStd[] = {44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000};
    const auto ranges = list<AudioValueRange>(d, kAudioDevicePropertyAvailableNominalSampleRates);
    std::vector<int> r;
    for (int s : kStd)
        for (const auto& g : ranges)
            if (s >= g.mMinimum - 0.5 && s <= g.mMaximum + 0.5) {
                r.push_back(s);
                break;
            }
    return r;
}

inline pid_t hog_owner(AudioDeviceID d) {
    pid_t p = -1;
    get(d, kAudioDevicePropertyHogMode, p);
    return p;
}

inline bool is_integer_pcm(const AudioStreamBasicDescription& f) {
    return f.mFormatID == kAudioFormatLinearPCM && !(f.mFormatFlags & kAudioFormatFlagIsFloat);
}

inline bool is_float32_pcm(const AudioStreamBasicDescription& f) {
    return f.mFormatID == kAudioFormatLinearPCM && (f.mFormatFlags & kAudioFormatFlagIsFloat) && f.mBitsPerChannel == 32 &&
           !(f.mFormatFlags & kAudioFormatFlagIsBigEndian);
}

inline std::string format_name(const AudioStreamBasicDescription& f) {
    if (f.mFormatID != kAudioFormatLinearPCM) return "non-PCM";
    return std::to_string(f.mBitsPerChannel) + "-bit " + ((f.mFormatFlags & kAudioFormatFlagIsFloat) ? "float" : "integer") + ", " +
           std::to_string(f.mChannelsPerFrame) + " ch, " + std::to_string(static_cast<int>(f.mSampleRate)) + " Hz";
}

}  // namespace daphne::ca

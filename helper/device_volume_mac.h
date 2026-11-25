#pragma once
// The hardware volume of the device this process holds in hog mode (macOS). While a device is hogged Core Audio refuses
// volume changes from every other process (the Sound menu, the volume keys, the host), so the process that holds it
// makes them. The samples are not touched: this is the device's own volume.
#include <CoreAudio/CoreAudio.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace daphne::mac {

struct DeviceVolume {
    bool ok = false;   // a device is held and has a settable volume
    double scalar = 0, db = 0, min_db = 0, max_db = 0;
    bool muted = false, can_mute = false;
};

// Reads it; scalar (0..1), db or mute, where given (non-null), are set first. Channels keep their balance.
inline DeviceVolume device_volume(const double* scalar, const double* db, const bool* mute) {
    using Addr = AudioObjectPropertyAddress;
    const auto at = [](AudioObjectPropertySelector s, UInt32 el) { return Addr{s, kAudioDevicePropertyScopeOutput, el}; };
    const auto read = [](AudioObjectID o, const Addr& a, auto& v) {
        UInt32 n = sizeof v;
        return AudioObjectGetPropertyData(o, &a, 0, nullptr, &n, &v) == noErr;
    };
    const auto settable = [](AudioObjectID o, const Addr& a) {
        Boolean w = 0;
        return AudioObjectHasProperty(o, &a) && AudioObjectIsPropertySettable(o, &a, &w) == noErr && w;
    };
    DeviceVolume r;

    Addr list{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, 0};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &list, 0, nullptr, &size) != noErr) return r;
    std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
    if (devices.empty() || AudioObjectGetPropertyData(kAudioObjectSystemObject, &list, 0, nullptr, &size, devices.data()) != noErr) return r;
    devices.resize(size / sizeof(AudioDeviceID));
    AudioDeviceID dev = kAudioObjectUnknown;
    for (AudioDeviceID d : devices) {
        pid_t owner = -1;
        if (read(d, Addr{kAudioDevicePropertyHogMode, kAudioObjectPropertyScopeGlobal, 0}, owner) && owner == getpid()) dev = d;
    }
    if (!dev) return r;

    // the main control, else one per channel
    std::vector<UInt32> els;
    if (settable(dev, at(kAudioDevicePropertyVolumeScalar, 0))) els.push_back(0);
    else
        for (UInt32 c = 1; c <= 32; ++c)
            if (settable(dev, at(kAudioDevicePropertyVolumeScalar, c))) els.push_back(c);
    if (els.empty()) return r;

    // the loudest channel is the level; the others keep their distance to it
    const auto level = [&](AudioObjectPropertySelector s) {
        Float32 top = -1000;
        for (UInt32 e : els) {
            Float32 v = 0;
            if (read(dev, at(s, e), v)) top = std::max(top, v);
        }
        return top;
    };
    AudioValueRange range{};
    const bool has_db = read(dev, at(kAudioDevicePropertyVolumeRangeDecibels, els[0]), range);

    if (scalar && std::isfinite(*scalar)) {
        const Float32 want = static_cast<Float32>(std::clamp(*scalar, 0.0, 1.0)), top = level(kAudioDevicePropertyVolumeScalar);
        for (UInt32 e : els) {
            Float32 v = want, cur = 0;
            const Addr a = at(kAudioDevicePropertyVolumeScalar, e);
            if (top > 0 && read(dev, a, cur)) v = std::clamp(want * cur / top, 0.0f, 1.0f);
            AudioObjectSetPropertyData(dev, &a, 0, nullptr, sizeof v, &v);
        }
    } else if (db && std::isfinite(*db) && has_db) {
        const Float32 lo = static_cast<Float32>(range.mMinimum), hi = static_cast<Float32>(range.mMaximum);
        const Float32 want = std::clamp(static_cast<Float32>(*db), lo, hi), top = level(kAudioDevicePropertyVolumeDecibels);
        for (UInt32 e : els) {
            const Addr a = at(kAudioDevicePropertyVolumeDecibels, e);
            Float32 v = want, cur = 0;
            if (read(dev, a, cur)) v = std::clamp(want + (cur - top), lo, hi);
            AudioObjectSetPropertyData(dev, &a, 0, nullptr, sizeof v, &v);
        }
    }
    const Addr mute_addr = at(kAudioDevicePropertyMute, 0);
    r.can_mute = settable(dev, mute_addr);
    if (mute && r.can_mute) {
        UInt32 v = *mute ? 1 : 0;
        AudioObjectSetPropertyData(dev, &mute_addr, 0, nullptr, sizeof v, &v);
    }

    r.ok = true;
    r.scalar = std::clamp(static_cast<double>(level(kAudioDevicePropertyVolumeScalar)), 0.0, 1.0);
    r.min_db = has_db ? range.mMinimum : -80;
    r.max_db = has_db ? range.mMaximum : 0;
    r.db = has_db ? level(kAudioDevicePropertyVolumeDecibels) : (r.scalar > 0 ? 20 * std::log10(r.scalar) : -80);
    UInt32 m = 0;
    if (r.can_mute && read(dev, mute_addr, m)) r.muted = m != 0;
    return r;
}

}  // namespace daphne::mac

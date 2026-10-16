// Core Audio device list (devices.h).
#include "coreaudio_util.h"
#include "daphne/engine/devices.h"

namespace daphne {

namespace {

std::string transport_name(UInt32 t) {
    switch (t) {
        case kAudioDeviceTransportTypeBuiltIn: return "built-in";
        case kAudioDeviceTransportTypeUSB: return "usb";
        case kAudioDeviceTransportTypeBluetooth:
        case kAudioDeviceTransportTypeBluetoothLE: return "bluetooth";
        case kAudioDeviceTransportTypeAirPlay: return "airplay";
        case kAudioDeviceTransportTypeHDMI: return "hdmi";
        case kAudioDeviceTransportTypeDisplayPort: return "displayport";
        case kAudioDeviceTransportTypeThunderbolt: return "thunderbolt";
        case kAudioDeviceTransportTypeFireWire: return "firewire";
        case kAudioDeviceTransportTypeVirtual: return "virtual";
        case kAudioDeviceTransportTypeAggregate: return "aggregate";
        default: return "other";
    }
}

}  // namespace

std::vector<CoreAudioDevice> list_coreaudio_devices(std::string* error) {
    std::vector<CoreAudioDevice> out;
    const AudioDeviceID def = ca::default_output();
    for (AudioDeviceID d : ca::output_devices()) {
        CoreAudioDevice c;
        c.uid = ca::string_prop(d, kAudioDevicePropertyDeviceUID);
        c.name = ca::string_prop(d, kAudioObjectPropertyName);
        c.is_default = d == def;
        c.rate = static_cast<int>(ca::nominal_rate(d) + 0.5);
        c.channels = ca::output_channels(d);
        c.rates = ca::offered_rates(d);
        UInt32 t = 0;
        ca::get(d, kAudioDevicePropertyTransportType, t);
        c.transport = transport_name(t);
        c.hog_pid = ca::hog_owner(d);
        out.push_back(std::move(c));
    }
    if (out.empty() && error) *error = "no Core Audio output device";
    return out;
}

}  // namespace daphne

#pragma once
// Output device enumeration: WASAPI and ASIO (Windows), Core Audio (macOS). Elsewhere each list is empty
// with an error.

#include <string>
#include <vector>

namespace daphne {

struct ExclusiveFormat {
    int rate;
    int bits;       // container bits
    int valid_bits;
};

struct WasapiDevice {
    std::string id;             // endpoint ID (UTF-8)
    std::string name;           // friendly name (UTF-8)
    bool is_default = false;
    int mix_rate = 0;           // shared-mode mix format
    int mix_channels = 0;
    int mix_bits = 0;
    std::vector<ExclusiveFormat> exclusive;  // filled only when probed
};

struct CoreAudioDevice {
    std::string uid;            // kAudioDevicePropertyDeviceUID (stable across restarts): the output.device to use
    std::string name;           // UTF-8
    bool is_default = false;    // the system output
    int rate = 0;               // nominal rate now
    int channels = 0;           // output channels
    std::vector<int> rates;     // standard rates (44.1k ... 768k) it offers
    std::string transport;      // "built-in", "usb", "bluetooth", "airplay", "virtual", ...
    int hog_pid = -1;           // the process holding it in hog mode (exclusive), -1 none
};

struct AsioDriver {
    std::string name;           // registry key name
    std::string description;
    std::string clsid;
};

// Active render endpoints. probe_exclusive: test common PCM formats in exclusive mode
// (stereo; 44.1k–768k × 16/24/32-bit) with IAudioClient::IsFormatSupported.
std::vector<WasapiDevice> list_wasapi_devices(bool probe_exclusive, std::string* error = nullptr);

// Installed ASIO drivers from HKLM\SOFTWARE\ASIO (no SDK needed for listing).
std::vector<AsioDriver> list_asio_drivers(std::string* error = nullptr);

// Core Audio devices with output channels.
std::vector<CoreAudioDevice> list_coreaudio_devices(std::string* error = nullptr);

}  // namespace daphne

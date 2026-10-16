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

// macOS, exclusive mode: the device kept between tracks, as MIKU's own Core Audio output does. While on, an output
// closed in exclusive mode stays open (hog mode, rate and DAC format; the IO runs, silent) and the next open of the
// same device takes it over: a new rate is set on it directly (one DAC relock instead of putting the old rate back and
// setting the new one) and macOS does not move the system output away and back in between. Another device, shared
// mode or a removed device puts it back first. release_kept_output_device() puts it back (rate, DAC format) and
// releases hog mode (true when one was kept); off releases it too. No effect on the other platforms.
void keep_output_devices(bool on);
bool release_kept_output_device();
bool output_device_kept();

}  // namespace daphne

#pragma once
// DSD files: DSF and DFF (uncompressed DSD; DST-compressed DFF is refused).
//
//   - playback ends at the sample count: the zero padding of the last DSF block and any chunk after
//     the sound data (ID3 metadata) are never played as DSD
//   - DSF with 1 bit per sample (LSB first, the common case) and 8 bits per sample (MSB first)
//   - DFF channel layout from its CHNL chunk
//
// read() hands out frames of one byte per channel (8 DSD bits, the oldest bit in the MSB),
// interleaved, as DsdEncoder expects.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "daphne/engine/decoder.h"

namespace daphne {

struct DsdInfo {
    int dsd_rate = 0;          // bits per second per channel (2822400 = DSD64)
    int channels = 0;
    int64_t frames = 0;        // bytes per channel (sample count / 8)
    uint32_t channel_mask = 0;
    std::string codec;         // "DSF" | "DFF"
};

class DsdFile {
public:
    DsdFile();
    ~DsdFile();
    DsdFile(const DsdFile&) = delete;
    DsdFile& operator=(const DsdFile&) = delete;

    bool open(const std::filesystem::path& path, std::string* error);
    const DsdInfo& info() const { return info_; }
    // Interleaved bytes, one per channel and frame; returns frames read (0 = end).
    int read(uint8_t* out, int frames);
    bool seek(int64_t frame);
    int64_t position() const { return pos_; }

private:
    bool open_dsf(std::string* error);
    bool open_dff(std::string* error);
    bool fill_block();   // DSF: the block holding pos_, every channel

    FILE* f_ = nullptr;
    DsdInfo info_;
    bool dsf_ = true, lsb_first_ = true;
    int64_t data_off_ = 0;
    int64_t file_size_ = -1;           // bytes, -1 when unknown: chunk sizes beyond it are cut to what is there
    int block_ = 4096;                 // DSF bytes per channel and block
    std::vector<uint8_t> blk_;         // DSF: current block, channel after channel
    int64_t blk_index_ = -1;
    int64_t pos_ = 0;
};

// The extension says DSD (.dsf / .dff).
bool is_dsd_path(const std::filesystem::path& path);

// DSD -> PCM, for devices that take neither DoP nor native DSD, and for rendering: two linear-phase
// Kaiser low-passes (120 dB), decimated to out_rate (0 = 176.4 / 192 kHz by the DSD rate family; it
// must divide the DSD rate / 8). lowpass_khz sets pass band / stop band: 24 -> 24 / 30 kHz,
// 30 -> 30 / 60 kHz, 50 -> 50 / 80 kHz, each at most 0.4535 / 0.5 out_rate. DSD's shaped noise rises
// steeply above 20 kHz: with DSD64 the 30 kHz setting leaves about -58 dBFS RMS of it at 40-50 kHz,
// the 24 kHz setting removes it.
// Gain +6.02 dB: DSD 50% modulation (the SACD 0 dB reference level) comes out at 0 dBFS, as loud as
// PCM releases of the same material. Peaks above the reference exceed 1.0; the float64 chain carries
// them to the volume stage.
std::unique_ptr<Decoder> make_dsd_pcm_decoder(int out_rate = 0, int lowpass_khz = 24);

}  // namespace daphne

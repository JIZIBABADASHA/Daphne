#pragma once
// Device sample formats and the final conversion from float64.
//
// Integers: scale by 2^(N-1), round half to even, clamp — an N-bit source played into an N-bit (or
// wider) container comes out bit for bit. No dither here (the quantizer stage in front of it
// decides that). A NaN sample becomes 0 and is counted with the clipped samples, for every type.

#include <cstdint>
#include <string>

namespace daphne {

// Float64 and Int32Low (valid_bits right-aligned in a 32-bit container, ASIO Int32LSB16..24) come
// from ASIO drivers, as do the DSD types (ASIO DSD mode: 8 DSD bits per byte, the first bit in the
// most / least significant bit; one byte per channel and frame).
enum class SampleType { Float32 = 0, Int32 = 1, Int24In32 = 2, Packed24 = 3, Int16 = 4, Float64 = 5, Int32Low = 6, Dsd8Msb = 7, Dsd8Lsb = 8 };

inline bool is_float(SampleType t) { return t == SampleType::Float32 || t == SampleType::Float64; }
inline bool is_dsd(SampleType t) { return t == SampleType::Dsd8Msb || t == SampleType::Dsd8Lsb; }

struct DeviceFormat {
    int rate = 0;
    int channels = 0;
    SampleType type = SampleType::Int32;
    int container_bits = 32;
    int valid_bits = 32;
    bool big_endian = false;   // ASIO MSB sample types (bytes swapped by the output)
    int block_align() const { return channels * container_bits / 8; }
    // frames (block_align bytes each) per second: DSD types carry 8 samples per byte
    int frame_rate() const { return is_dsd(type) ? rate / 8 : rate; }
};

const char* sample_type_name(SampleType t);
std::string describe(const DeviceFormat& f);

// Integer resolution of the format (float32 counts as 24 bits of mantissa).
int effective_bits(const DeviceFormat& f);

// Converts interleaved float64 samples to the device byte layout (little-endian). Returns the
// count of clipped (or NaN) samples.
int64_t convert_to_device(const double* in, size_t samples, const DeviceFormat& f, uint8_t* out);

}  // namespace daphne

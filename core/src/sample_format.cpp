#include "daphne/engine/sample_format.h"

#include <cmath>
#include <cstring>

namespace daphne {

const char* sample_type_name(SampleType t) {
    switch (t) {
        case SampleType::Float32: return "float32";
        case SampleType::Int32: return "int32";
        case SampleType::Int24In32: return "int24-in-32";
        case SampleType::Packed24: return "packed24";
        case SampleType::Int16: return "int16";
        case SampleType::Float64: return "float64";
        case SampleType::Int32Low: return "int32-low";
        case SampleType::Dsd8Msb: return "DSD (MSB first)";
        case SampleType::Dsd8Lsb: return "DSD (LSB first)";
    }
    return "?";
}

std::string describe(const DeviceFormat& f) {
    std::string t = sample_type_name(f.type);
    if (f.type == SampleType::Int32Low) t = "int" + std::to_string(f.valid_bits) + "-in-32 (low)";
    if (f.big_endian) t += " big-endian";
    return std::to_string(f.rate) + " Hz, " + std::to_string(f.channels) + " ch, " + t;
}

int effective_bits(const DeviceFormat& f) {
    if (f.type == SampleType::Float32) return 24;
    if (f.type == SampleType::Float64) return 53;
    if (is_dsd(f.type)) return 1;
    return f.valid_bits;
}

namespace {

// round half to even (the default FP mode), clamp; NaN -> 0
struct ToInt {
    double scale, hi, lo;
    explicit ToInt(int bits)
        : scale(static_cast<double>(int64_t(1) << (bits - 1))),
          hi(static_cast<double>((int64_t(1) << (bits - 1)) - 1)),
          lo(-static_cast<double>(int64_t(1) << (bits - 1))) {}
    int64_t operator()(double x, int64_t& clipped) const {
        if (std::isnan(x)) {   // a NaN from upstream (a diverging filter, a broken decoder): silence, counted
            ++clipped;
            return 0;
        }
        const double v = std::nearbyint(x * scale);
        if (v > hi) {
            ++clipped;
            return static_cast<int64_t>(hi);
        }
        if (v < lo) {
            ++clipped;
            return static_cast<int64_t>(lo);
        }
        return static_cast<int64_t>(v);
    }
};

inline double finite_or_zero(double x, int64_t& clipped) {
    if (std::isnan(x)) {
        ++clipped;
        return 0.0;
    }
    return x;
}

}  // namespace

int64_t convert_to_device(const double* in, size_t samples, const DeviceFormat& f, uint8_t* out) {
    int64_t clipped = 0;
    switch (f.type) {
        case SampleType::Float32:
            for (size_t i = 0; i < samples; ++i) {
                const float v = static_cast<float>(finite_or_zero(in[i], clipped));
                std::memcpy(out + i * 4, &v, 4);
            }
            break;
        case SampleType::Float64:
            for (size_t i = 0; i < samples; ++i) {
                const double v = finite_or_zero(in[i], clipped);
                std::memcpy(out + i * 8, &v, 8);
            }
            break;
        case SampleType::Int32: {
            const ToInt q(32);
            for (size_t i = 0; i < samples; ++i) {
                const int32_t v = static_cast<int32_t>(q(in[i], clipped));
                std::memcpy(out + i * 4, &v, 4);
            }
            break;
        }
        case SampleType::Int24In32: {
            const ToInt q(24);
            for (size_t i = 0; i < samples; ++i) {
                const int32_t v = static_cast<int32_t>(static_cast<uint32_t>(q(in[i], clipped)) << 8);
                std::memcpy(out + i * 4, &v, 4);
            }
            break;
        }
        case SampleType::Packed24: {
            const ToInt q(24);
            for (size_t i = 0; i < samples; ++i) {
                const int32_t v = static_cast<int32_t>(q(in[i], clipped));
                out[i * 3 + 0] = static_cast<uint8_t>(v);
                out[i * 3 + 1] = static_cast<uint8_t>(v >> 8);
                out[i * 3 + 2] = static_cast<uint8_t>(v >> 16);
            }
            break;
        }
        case SampleType::Int16: {
            const ToInt q(16);
            for (size_t i = 0; i < samples; ++i) {
                const int16_t v = static_cast<int16_t>(q(in[i], clipped));
                std::memcpy(out + i * 2, &v, 2);
            }
            break;
        }
        case SampleType::Int32Low: {
            const ToInt q(f.valid_bits);
            for (size_t i = 0; i < samples; ++i) {
                const int32_t v = static_cast<int32_t>(q(in[i], clipped));
                std::memcpy(out + i * 4, &v, 4);
            }
            break;
        }
        case SampleType::Dsd8Msb:
        case SampleType::Dsd8Lsb: break;   // DSD bytes come from the DSD encoder, not from PCM
    }
    return clipped;
}

}  // namespace daphne

#include "daphne/dsp/fma.h"

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86)) && !defined(_M_ARM64EC)
#include <immintrin.h>
#include <intrin.h>
#endif

#include "daphne/platform/platform.h"

namespace daphne::dsp {
namespace {

// FMA3 on x86 (and the OS saving the YMM state its VEX encoding uses); ARM64 always has it.
bool cpu_has_fma() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86)) && !defined(_M_ARM64EC)
    int r[4];
    __cpuid(r, 1);
    const bool fma = (r[2] >> 12) & 1, osxsave = (r[2] >> 27) & 1, avx = (r[2] >> 28) & 1;
    return fma && osxsave && avx && (_xgetbv(0) & 0x6) == 0x6;
#elif (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
    __builtin_cpu_init();
    return __builtin_cpu_supports("fma");
#else
    return true;   // ARM64 (fmadd is in the base instruction set) and others: std::fma as before
#endif
}

}  // namespace

namespace detail {
bool g_hw_fma = cpu_has_fma() && !platform::get_env("DAPHNE_FMA_EMULATE").has_value();
}  // namespace detail

bool fma_in_hardware() { return detail::g_hw_fma; }

}  // namespace daphne::dsp

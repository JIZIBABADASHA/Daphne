// The parts of platform.h that depend on the CPU, not on the OS.
#include "daphne/platform/platform.h"

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#define DAPHNE_FP_X86 1
#include <xmmintrin.h>
#elif (defined(__aarch64__) || defined(__arm64__)) && (defined(__GNUC__) || defined(__clang__))
#define DAPHNE_FP_ARM64 1
#endif

namespace daphne::platform {

uint64_t fp_control() {
#if defined(DAPHNE_FP_X86)
    return _mm_getcsr();
#elif defined(DAPHNE_FP_ARM64)
    uint64_t v = 0;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(v));
    return v;
#else
    return 0;
#endif
}

void set_fp_control(uint64_t v) {
#if defined(DAPHNE_FP_X86)
    _mm_setcsr(static_cast<unsigned int>(v));
#elif defined(DAPHNE_FP_ARM64)
    __asm__ __volatile__("msr fpcr, %0" : : "r"(v));
#else
    (void)v;
#endif
}

uint64_t with_flush_to_zero(uint64_t v) {
#if defined(DAPHNE_FP_X86)
    return v | 0x8040;            // FTZ (bit 15) + DAZ (bit 6)
#elif defined(DAPHNE_FP_ARM64)
    return v | (uint64_t{1} << 24);   // FZ: denormal inputs and results are zero
#else
    return v;
#endif
}

}  // namespace daphne::platform

# daphne_dsp: signal processing blocks and filter design
add_library(daphne_dsp STATIC
    src/dd.cpp
    src/dd_conv.cpp
    src/crossfeed.cpp
    src/downmix.cpp
    src/eq.cpp
    src/filter_design.cpp
    src/gain.cpp
    src/limiter.cpp
    src/modulator.cpp
    src/progress.cpp
    src/quantizer.cpp
    src/resampler.cpp
)
target_include_directories(daphne_dsp PUBLIC include)
target_link_libraries(daphne_dsp PUBLIC daphne_vendor)
target_link_libraries(daphne_dsp PUBLIC daphne_platform)
# the double-double kernels use AVX2 + FMA3 (x86 only); the rest stays on the baseline ISA and the
# kernels are only called when the CPU has both (dd_conv.cpp checks with cpuid). Other CPUs (Apple
# Silicon, ARM64 Linux) use the scalar path, which gives the same bits.
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" DAPHNE_CPU)
if(DAPHNE_CPU MATCHES "^(x86_64|amd64|x64|i[3-6]86|x86)$")
    target_sources(daphne_dsp PRIVATE src/dd_conv_avx2.cpp)
    target_compile_definitions(daphne_dsp PRIVATE DAPHNE_DD_AVX2=1)
    if(MSVC)
        set_source_files_properties(src/dd_conv_avx2.cpp PROPERTIES COMPILE_OPTIONS /arch:AVX2)
    else()
        set_source_files_properties(src/dd_conv_avx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma")
    endif()
else()
    target_compile_definitions(daphne_dsp PRIVATE DAPHNE_DD_AVX2=0)
endif()

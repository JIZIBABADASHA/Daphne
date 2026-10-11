# daphne_dsp: signal processing blocks and filter design
add_library(daphne_dsp STATIC
    src/dd.cpp
    src/dd_conv.cpp
    src/dd_conv_avx2.cpp
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
# the double-double kernels use AVX2 + FMA3; the rest stays on the baseline ISA and the kernels are
# only called when the CPU has both (dd_conv.cpp checks with cpuid)
if(MSVC)
    set_source_files_properties(src/dd_conv_avx2.cpp PROPERTIES COMPILE_OPTIONS /arch:AVX2)
else()
    set_source_files_properties(src/dd_conv_avx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma")
endif()
if(WIN32)
    target_link_libraries(daphne_dsp PRIVATE avrt)   # the modulator's worker thread joins MMCSS
endif()

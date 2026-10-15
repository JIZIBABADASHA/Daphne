# daphne_dsp: signal processing blocks and filter design
add_library(daphne_dsp STATIC
    src/dd.cpp
    src/dd_conv.cpp
    src/fma.cpp
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
# The double-double kernels use AVX2 + FMA3 on x86; the rest stays on the baseline ISA and the kernels
# are only called when the CPU has both (dd_conv.cpp checks). dd_conv_avx2.cpp is always compiled and is
# empty for other CPUs (dd_conv.h decides from the compiler's target); those (Apple Silicon, ARM64) use
# the scalar path, which gives the same bits. The flags go to x86 targets only, known from the target,
# not the build machine: MSVC reports its compiler's architecture, an Apple build lists its
# architectures (universal: the flags for the x86_64 slice only).
target_sources(daphne_dsp PRIVATE src/dd_conv_avx2.cpp)
if(MSVC)
    string(TOLOWER "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}" DAPHNE_MSVC_ARCH)
    if(DAPHNE_MSVC_ARCH MATCHES "^(x64|x86)$")
        set_source_files_properties(src/dd_conv_avx2.cpp PROPERTIES COMPILE_OPTIONS /arch:AVX2)
    endif()
elseif(APPLE)
    set(DAPHNE_ARCHS ${CMAKE_OSX_ARCHITECTURES})
    if(NOT DAPHNE_ARCHS)
        set(DAPHNE_ARCHS ${CMAKE_SYSTEM_PROCESSOR})
    endif()
    if("x86_64" IN_LIST DAPHNE_ARCHS)
        set_source_files_properties(src/dd_conv_avx2.cpp PROPERTIES COMPILE_OPTIONS
                                    "-Xarch_x86_64;-mavx2;-Xarch_x86_64;-mfma")
    endif()
else()
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" DAPHNE_CPU)
    if(DAPHNE_CPU MATCHES "^(x86_64|amd64|x64|i[3-6]86|x86)$")
        set_source_files_properties(src/dd_conv_avx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma")
    endif()
endif()
daphne_warnings(daphne_dsp)

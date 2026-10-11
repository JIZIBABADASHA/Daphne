# daphne_engine: decoders, processing chain, device output, playback
add_library(daphne_engine STATIC
    src/text.cpp
    src/ring_feeder.cpp
    src/player.cpp
    src/wasapi_output.cpp
    src/asio_output.cpp
    src/devices_wasapi.cpp
    src/devices_asio.cpp
    src/chain.cpp
    src/decoder_factory.cpp
    src/decoder_flac.cpp
    src/decoder_wav.cpp
    src/decoder_mp3.cpp
    src/decoder_ffmpeg.cpp
    src/dsd_file.cpp
    src/dsd_output.cpp
    src/filters.cpp
    src/sample_format.cpp
    src/settings.cpp
    src/stages.cpp
)
target_include_directories(daphne_engine PUBLIC include)
target_link_libraries(daphne_engine PUBLIC daphne_dsp daphne_vendor)
if(WIN32)
    target_link_libraries(daphne_engine PRIVATE ole32 advapi32 avrt winmm propsys)
endif()

# ASIO (optional): the Steinberg ASIO SDK (dual licence: proprietary / GPLv3) unpacked to
# third_party/asiosdk (scripts/fetch_asiosdk.ps1); it is not part of the repository.
set(DAPHNE_ASIO_DIR ${PROJECT_SOURCE_DIR}/third_party/asiosdk)
if(EXISTS ${DAPHNE_ASIO_DIR}/common/iasiodrv.h)
    message(STATUS "daphne: ASIO SDK found in ${DAPHNE_ASIO_DIR}")
    target_compile_definitions(daphne_engine PRIVATE DAPHNE_HAVE_ASIO)
    target_include_directories(daphne_engine SYSTEM PRIVATE ${DAPHNE_ASIO_DIR}/common)
else()
    message(STATUS "daphne: ASIO SDK not found (ASIO output disabled); run scripts/fetch_asiosdk.ps1")
endif()

# FFmpeg (optional): scripts/fetch_ffmpeg.ps1 puts the shared development package in third_party/ffmpeg
set(DAPHNE_FFMPEG_DIR ${PROJECT_SOURCE_DIR}/third_party/ffmpeg)
if(EXISTS ${DAPHNE_FFMPEG_DIR}/include/libavcodec/avcodec.h)
    message(STATUS "daphne: FFmpeg found in ${DAPHNE_FFMPEG_DIR}")
    target_compile_definitions(daphne_engine PRIVATE DAPHNE_HAVE_FFMPEG)
    target_include_directories(daphne_engine SYSTEM PRIVATE ${DAPHNE_FFMPEG_DIR}/include)
    target_link_libraries(daphne_engine PRIVATE
        ${DAPHNE_FFMPEG_DIR}/lib/avformat.lib ${DAPHNE_FFMPEG_DIR}/lib/avcodec.lib
        ${DAPHNE_FFMPEG_DIR}/lib/avutil.lib ${DAPHNE_FFMPEG_DIR}/lib/swresample.lib)
    file(GLOB DAPHNE_FFMPEG_DLLS ${DAPHNE_FFMPEG_DIR}/bin/avformat-*.dll ${DAPHNE_FFMPEG_DIR}/bin/avcodec-*.dll
         ${DAPHNE_FFMPEG_DIR}/bin/avutil-*.dll ${DAPHNE_FFMPEG_DIR}/bin/swresample-*.dll)
    set(DAPHNE_FFMPEG_DLLS ${DAPHNE_FFMPEG_DLLS} PARENT_SCOPE)
else()
    message(STATUS "daphne: FFmpeg not found (formats without a dedicated decoder disabled); run scripts/fetch_ffmpeg.ps1")
endif()

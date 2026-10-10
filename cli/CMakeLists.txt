add_executable(daphne main.cpp)
target_link_libraries(daphne PRIVATE daphne_engine bcrypt)
if(MSVC)
    target_link_options(daphne PRIVATE /ENTRY:wmainCRTStartup)
endif()
# the FFmpeg DLLs next to the executable
foreach(dll ${DAPHNE_FFMPEG_DLLS})
    add_custom_command(TARGET daphne POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${dll} $<TARGET_FILE_DIR:daphne>)
endforeach()

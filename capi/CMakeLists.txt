# daphne_core: C ABI of the player for host applications
add_library(daphne_core SHARED c_api.cpp)
target_include_directories(daphne_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(daphne_core PRIVATE DC_BUILDING_DLL)
target_link_libraries(daphne_core PRIVATE daphne_engine)
foreach(dll ${DAPHNE_FFMPEG_DLLS})
    add_custom_command(TARGET daphne_core POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${dll} $<TARGET_FILE_DIR:daphne_core>)
endforeach()
daphne_warnings(daphne_core)

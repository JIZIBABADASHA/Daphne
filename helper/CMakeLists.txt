# daphne-helper: the player as a child process speaking JSON lines (helper/main.cpp), on the C API
add_executable(daphne-helper main.cpp)
target_link_libraries(daphne-helper PRIVATE daphne_core daphne_vendor)
if(APPLE)
    target_link_libraries(daphne-helper PRIVATE "-framework CoreAudio")   # device_volume_mac.h
endif()
# finds libdaphne_core next to itself (both go to bin/)
if(APPLE)
    set_target_properties(daphne-helper PROPERTIES BUILD_RPATH "@loader_path" INSTALL_RPATH "@loader_path")
elseif(UNIX)
    set_target_properties(daphne-helper PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
endif()
daphne_warnings(daphne-helper)

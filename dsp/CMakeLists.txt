# daphne_dsp: signal processing blocks and filter design
add_library(daphne_dsp STATIC
    src/progress.cpp
)
target_include_directories(daphne_dsp PUBLIC include)
target_link_libraries(daphne_dsp PUBLIC daphne_vendor)

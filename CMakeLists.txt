cmake_minimum_required(VERSION 3.25)
project(daphne VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

option(DAPHNE_BUILD_TESTS "Build unit tests" ON)

# header-only / vendored dependencies
add_library(daphne_vendor INTERFACE)
target_include_directories(daphne_vendor SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

add_subdirectory(dsp)
add_subdirectory(core)
add_subdirectory(cli)

if(DAPHNE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

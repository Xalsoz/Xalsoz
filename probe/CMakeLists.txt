cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(gddiff-window-probe VERSION 0.2.0)

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Set the GEODE_SDK environment variable to the Geode SDK path")
endif()

add_library(${PROJECT_NAME} SHARED
    src/main.cpp
    src/core/probe_core.cpp
    src/core/solve_core.cpp
    src/core/difficulty.cpp
)
target_include_directories(${PROJECT_NAME} PRIVATE src)

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)
setup_geode_mod(${PROJECT_NAME})

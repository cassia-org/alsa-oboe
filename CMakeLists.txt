cmake_minimum_required(VERSION 3.10.2)
project(PCM_OBOE LANGUAGES CXX VERSION 1.0.0)
set(CMAKE_CXX_STANDARD 17)

# Includes
include(CheckSymbolExists)
find_package(PkgConfig REQUIRED)

# Libraries

## ALSA
pkg_check_modules(alsa REQUIRED IMPORTED_TARGET alsa)
link_directories(${alsa_LIBRARY_DIRS})

## Oboe (built as a static library)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/oboe)

# Targets

## ALSA Plugin
add_library(asound_module_pcm_oboe SHARED pcm_oboe.cpp)
target_link_libraries(asound_module_pcm_oboe PkgConfig::alsa oboe)
### ALSA requires PIC for dynamically linked plugins, so we need to define it.
target_compile_definitions(asound_module_pcm_oboe PRIVATE -DPIC=1)
set_property(TARGET asound_module_pcm_oboe PROPERTY POSITION_INDEPENDENT_CODE ON)

install(TARGETS asound_module_pcm_oboe DESTINATION lib/alsa-lib)
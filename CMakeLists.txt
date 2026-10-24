# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

cmake_minimum_required(VERSION 3.27)

set(WFG_TARGET "host" CACHE STRING "Build target: host, rt1170 or rt1064")
set_property(CACHE WFG_TARGET PROPERTY STRINGS host rt1170 rt1064)

if(WFG_TARGET MATCHES "^rt(1170|1064)$")
    project(WaveformGenerator VERSION 0.2.0 LANGUAGES C CXX ASM)
else()
    project(WaveformGenerator VERSION 0.2.0 LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(WFG_HOST_JOBS 4 CACHE STRING "Worker limit for host build and analysis tools")
if(NOT WFG_HOST_JOBS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "WFG_HOST_JOBS must be a positive integer")
endif()

file(REAL_PATH "${CMAKE_CURRENT_SOURCE_DIR}" WFG_ROOT)
file(REAL_PATH "${CMAKE_CURRENT_BINARY_DIR}" WFG_BUILD_ROOT)

set(WFG_BUILD_ID "wfg-${PROJECT_VERSION}" CACHE STRING "Build label included in device and artifact metadata")
# The 48-character cap is part of the JSON STATUS? budget (rt1170/status_contract.hpp).
string(LENGTH "${WFG_BUILD_ID}" wfg_build_id_length)
if(NOT WFG_BUILD_ID MATCHES "^[A-Za-z0-9_.-]+$" OR wfg_build_id_length GREATER 48)
    message(FATAL_ERROR "WFG_BUILD_ID must be 1 to 48 letters, digits, underscores, dots or hyphens")
endif()

# Plugins first: the build identity records whether plugin sources are compiled in.
include("${WFG_ROOT}/cmake/encoder_plugins.cmake")
include("${WFG_ROOT}/cmake/wfg_build_info.cmake")

# Portable finite-stream scheduling foundation. Compiled on ARM as well as
# host, but not yet connected to the player's card/audio owner.
add_library(wfg_finite_playback STATIC common/finite_playback.cpp common/status.cpp)
target_include_directories(wfg_finite_playback PUBLIC "${WFG_ROOT}")
target_compile_features(wfg_finite_playback PUBLIC cxx_std_23)
if(WFG_TARGET MATCHES "^rt(1170|1064)$")
    target_compile_options(wfg_finite_playback PRIVATE
        -mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16
        -ffunction-sections -fdata-sections -fno-threadsafe-statics -fstack-usage
        $<$<CONFIG:Release,flexspi_nor_release>:-O3>
        $<$<CONFIG:Debug,flexspi_nor_debug>:-Og>)
endif()

if(WFG_TARGET STREQUAL "host")
    include(CTest)
    enable_testing()
    # Catch2 (third_party/Catch2) for host unit tests; no firmware build includes it.
    include("${WFG_ROOT}/cmake/wfg_catch2.cmake")
    add_subdirectory(signal-lab)
    # Portable storage boundary for already-processed PCM, shared with the
    # firmware build; it does not own a filesystem or start an audio stream.
    add_library(wfg_pcm24_artifact STATIC common/pcm24_artifact.cpp common/status.cpp)
    target_include_directories(wfg_pcm24_artifact PUBLIC "${WFG_ROOT}")
    target_compile_features(wfg_pcm24_artifact PUBLIC cxx_std_23)
    add_subdirectory(waveform-source)
    add_library(wfg_encoder_plugin STATIC)
    wfg_attach_encoder_plugin(wfg_encoder_plugin)
    target_link_libraries(wfg_encoder_plugin PUBLIC wfg_waveform_source)
    if(WFG_ENABLE_ENCODER_PLUGINS AND EXISTS "${WFG_ENCODER_PLUGIN_DIR}/CMakeLists.txt")
        add_subdirectory("${WFG_ENCODER_PLUGIN_DIR}" "encoder-plugin")
    endif()
    add_subdirectory(host/capture)
    add_subdirectory(host/render)
    add_subdirectory(tests)
elseif(WFG_TARGET MATCHES "^rt(1170|1064)$")
    add_subdirectory(rt1170)
else()
    message(FATAL_ERROR "Unsupported WFG_TARGET '${WFG_TARGET}'")
endif()

include("${WFG_ROOT}/cmake/project_options.cmake")
wfg_apply_directory_options("${WFG_ROOT}")

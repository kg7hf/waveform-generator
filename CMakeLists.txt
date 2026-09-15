# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Paul R. Decker

cmake_minimum_required(VERSION 3.27)

set(WFG_TARGET "host" CACHE STRING "Build target: host or rt1170")
set_property(CACHE WFG_TARGET PROPERTY STRINGS host rt1170)

if(WFG_TARGET STREQUAL "rt1170")
    project(WaveformGenerator VERSION 0.2.0 LANGUAGES C CXX ASM)
else()
    project(WaveformGenerator VERSION 0.2.0 LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

file(REAL_PATH "${CMAKE_CURRENT_SOURCE_DIR}" WFG_ROOT)
file(REAL_PATH "${CMAKE_CURRENT_BINARY_DIR}" WFG_BUILD_ROOT)

set(WFG_BUILD_ID "wfg-${PROJECT_VERSION}" CACHE STRING "Build label included in device and artifact metadata")
if(NOT WFG_BUILD_ID MATCHES "^[A-Za-z0-9_.-]+$")
    message(FATAL_ERROR "WFG_BUILD_ID must contain only letters, digits, underscore, dot or hyphen")
endif()

include("${WFG_ROOT}/cmake/encoder_plugins.cmake")

if(WFG_TARGET STREQUAL "host")
    include(CTest)
    enable_testing()
    add_subdirectory(signal-lab)
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
elseif(WFG_TARGET STREQUAL "rt1170")
    add_subdirectory(rt1170)
else()
    message(FATAL_ERROR "Unsupported WFG_TARGET '${WFG_TARGET}'")
endif()

include("${WFG_ROOT}/cmake/project_options.cmake")
wfg_apply_directory_options("${WFG_ROOT}")

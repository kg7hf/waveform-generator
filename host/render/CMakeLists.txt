# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Paul R. Decker

add_executable(signal_lab_render signal_lab_render.cpp live_replay.cpp "${WFG_ROOT}/common/live_protocol.cpp")
target_include_directories(signal_lab_render PRIVATE "${WFG_ROOT}")
target_link_libraries(signal_lab_render PRIVATE wfg_signal_lab)
target_compile_features(signal_lab_render PRIVATE cxx_std_23)
if(MSVC)
    target_compile_options(signal_lab_render PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(signal_lab_render PRIVATE -Wall -Wextra -Wpedantic -Werror ${WFG_SIGNAL_LAB_COMPILE_OPTIONS})
endif()
set_target_properties(signal_lab_render PROPERTIES ENABLE_EXPORTS OFF)

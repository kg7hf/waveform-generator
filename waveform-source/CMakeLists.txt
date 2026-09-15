# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Paul R. Decker

add_library(wfg_waveform_source STATIC wav_generator.cpp "${WFG_ROOT}/common/status.cpp")
target_include_directories(wfg_waveform_source PUBLIC "${WFG_ROOT}")
target_link_libraries(wfg_waveform_source PUBLIC wfg_signal_lab)
target_compile_features(wfg_waveform_source PUBLIC cxx_std_23)
if(MSVC)
    target_compile_options(wfg_waveform_source PRIVATE /W4 /WX /permissive- /fp:strict)
else()
    target_compile_options(wfg_waveform_source PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
endif()
add_executable(wfg_waveform_source_tests tests.cpp)
target_link_libraries(wfg_waveform_source_tests PRIVATE wfg_waveform_source)
target_compile_features(wfg_waveform_source_tests PRIVATE cxx_std_23)
set_target_properties(wfg_waveform_source_tests PROPERTIES ENABLE_EXPORTS OFF)
add_test(NAME wfg-waveform-source COMMAND wfg_waveform_source_tests)

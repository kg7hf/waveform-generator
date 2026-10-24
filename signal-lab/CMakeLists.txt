# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

include("${CMAKE_CURRENT_SOURCE_DIR}/sources.cmake")

# common/status.cpp gives the host library m110::critical_error, which Result::value()
# names (the firmware compiles it with its own sources, so sources.cmake leaves it out).
add_library(wfg_signal_lab STATIC ${WFG_SIGNAL_LAB_SOURCES} "${CMAKE_SOURCE_DIR}/common/status.cpp")
target_include_directories(wfg_signal_lab PUBLIC
    "${WFG_SIGNAL_LAB_INCLUDE}"
    "${CMAKE_SOURCE_DIR}"
)
target_compile_features(wfg_signal_lab PUBLIC cxx_std_23)
if(MSVC)
    target_compile_options(wfg_signal_lab PRIVATE /W4 /WX /permissive- /fp:strict)
else()
    target_compile_options(wfg_signal_lab PRIVATE -Wall -Wextra -Wpedantic -Werror ${WFG_SIGNAL_LAB_COMPILE_OPTIONS})
endif()

add_executable(wfg_signal_lab_tests tests/signal_lab_tests.cpp)
target_link_libraries(wfg_signal_lab_tests PRIVATE wfg_signal_lab)
target_compile_features(wfg_signal_lab_tests PRIVATE cxx_std_23)
if(MSVC)
    target_compile_options(wfg_signal_lab_tests PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(wfg_signal_lab_tests PRIVATE -Wall -Wextra -Wpedantic -Werror ${WFG_SIGNAL_LAB_COMPILE_OPTIONS})
endif()
set_target_properties(wfg_signal_lab_tests PROPERTIES ENABLE_EXPORTS OFF)
add_test(NAME wfg-signal-lab-cpp COMMAND wfg_signal_lab_tests)

add_executable(wfg_live_control_tests tests/live_control_tests.cpp)
target_link_libraries(wfg_live_control_tests PRIVATE wfg_signal_lab)
target_compile_features(wfg_live_control_tests PRIVATE cxx_std_23)
if(NOT MSVC)
    target_compile_options(wfg_live_control_tests PRIVATE -Wall -Wextra -Wpedantic -Werror ${WFG_SIGNAL_LAB_COMPILE_OPTIONS})
endif()
add_test(NAME wfg-live-control-cpp COMMAND wfg_live_control_tests)

# Catch2 unit tests, one source per component (cmake/wfg_catch2.cmake); each
# TEST_CASE is its own ctest test. They take the library's determinism flags.
set(wfg_signal_lab_unit_test_options "")
if(NOT MSVC)
    set(wfg_signal_lab_unit_test_options ${WFG_SIGNAL_LAB_COMPILE_OPTIONS})
endif()
wfg_add_catch2_test(wfg_signal_lab_unit_tests
    SOURCES
        tests/det_math_tests.cpp
        tests/units_tests.cpp
        tests/det_gaussian_tests.cpp
        tests/kaiser_tests.cpp
        tests/analytic_filter_tests.cpp
        tests/doppler_filter_tests.cpp
        tests/watterson_tests.cpp
        tests/watterson_scenario_tests.cpp
        tests/awgn_tests.cpp
        tests/watterson_engine_tests.cpp
        tests/frequency_shift_tests.cpp
        tests/frequency_shift_scenario_tests.cpp
        tests/noise_bandwidth_tests.cpp
        tests/wideband_engine_tests.cpp
        tests/cw_pulse_tests.cpp
        tests/cw_pulse_scenario_tests.cpp
        tests/cw_sweep_tests.cpp
        tests/cw_sweep_scenario_tests.cpp
        tests/markov_gate_tests.cpp
        tests/cw_markov_scenario_tests.cpp
        tests/cw_fsk_tests.cpp
        tests/cw_fsk_scenario_tests.cpp
        tests/cw_faded_tests.cpp
        tests/cw_faded_scenario_tests.cpp
        tests/stream_identity_tests.cpp
        tests/stream_identity_scenario_tests.cpp
        tests/second_source_tests.cpp
        tests/second_source_scenario_tests.cpp
        tests/second_source_engine_tests.cpp
        tests/full_chain_tests.cpp
        tests/allocation_tests.cpp
        tests/allocation_counter.cpp
    LIBRARIES wfg_signal_lab
    COMPILE_OPTIONS ${wfg_signal_lab_unit_test_options})

# The host driver of the Watterson validation battery (docs/design/watterson-validation.md):
# the engine with a static WattersonChannel, a WAV streamed in and PCM24 out, block by
# block, with the channel truth on request. signal_lab_render passes no channel yet
# (design WP7), so the battery has its own driver; like everything here it is host only.
add_executable(wfg_watterson_render validation/watterson_render.cpp "${CMAKE_SOURCE_DIR}/common/host_critical_error.cpp")
target_link_libraries(wfg_watterson_render PRIVATE wfg_signal_lab)
# File names reach main() as written, wildcards included (cmake/wfg_noglob.cmake).
include("${CMAKE_SOURCE_DIR}/cmake/wfg_noglob.cmake")
wfg_take_arguments_as_written(wfg_watterson_render)
target_compile_features(wfg_watterson_render PRIVATE cxx_std_23)
if(MSVC)
    target_compile_options(wfg_watterson_render PRIVATE /W4 /WX /permissive- /fp:strict)
else()
    target_compile_options(wfg_watterson_render PRIVATE -Wall -Wextra -Wpedantic -Werror ${WFG_SIGNAL_LAB_COMPILE_OPTIONS})
endif()

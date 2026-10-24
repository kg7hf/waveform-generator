# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

if(WIN32)
    # common/status.cpp gives m110::critical_error, which Result::value() names,
    # and common/host_critical_error.cpp the handler main() installs.
    add_executable(waveform_capture
        capture_main.cpp
        winmm_audio.cpp
        wasapi_audio.cpp
        "${WFG_ROOT}/common/status.cpp"
        "${WFG_ROOT}/common/host_critical_error.cpp")
    target_include_directories(waveform_capture PRIVATE "${WFG_ROOT}")
    target_compile_features(waveform_capture PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(waveform_capture PRIVATE /W4 /WX /permissive-)
    else()
        target_compile_options(waveform_capture PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    target_link_libraries(waveform_capture PRIVATE winmm ole32 uuid)
    set_target_properties(waveform_capture PROPERTIES ENABLE_EXPORTS OFF)
    # File names reach main() as written, wildcards included (cmake/wfg_noglob.cmake).
    include("${WFG_ROOT}/cmake/wfg_noglob.cmake")
    wfg_take_arguments_as_written(waveform_capture)
    # MinGW emits an --out-implib path for executables even when no exports are
    # present. Keep that generated path materialized so the standalone audit can
    # account for the complete link command without treating it as source.
    endif()

if(WIN32)
    add_executable(waveform_capture
        capture_main.cpp
        winmm_audio.cpp
        wasapi_audio.cpp)
    target_include_directories(waveform_capture PRIVATE "${WFG_ROOT}")
    target_compile_features(waveform_capture PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(waveform_capture PRIVATE /W4 /WX /permissive-)
    else()
        target_compile_options(waveform_capture PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    target_link_libraries(waveform_capture PRIVATE winmm ole32 uuid)
    set_target_properties(waveform_capture PROPERTIES ENABLE_EXPORTS OFF)
    # MinGW emits an --out-implib path for executables even when no exports are
    # present. Keep that generated path materialized so the standalone audit can
    # account for the complete link command without treating it as source.
    add_custom_command(TARGET waveform_capture POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E touch
                "${CMAKE_CURRENT_BINARY_DIR}/libwaveform_capture.dll.a")
endif()

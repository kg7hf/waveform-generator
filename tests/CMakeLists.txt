# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

add_test(NAME wfg-summarize-prepare-play
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_summarize_prepare_play.py")

wfg_add_catch2_test(wfg_operation_timing_tests SOURCES operation_timing_tests.cpp)
target_include_directories(wfg_operation_timing_tests PRIVATE "${WFG_ROOT}")

wfg_add_catch2_test(wfg_finite_playback_tests
    SOURCES finite_playback_tests.cpp
    LIBRARIES wfg_finite_playback wfg_pcm24_artifact)
add_test(NAME wfg-prepared-artifacts
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_prepared_artifacts.py")
add_test(NAME wfg-compare-prepared
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_compare_prepared.py"
            --renderer "$<TARGET_FILE:signal_lab_render>")

wfg_add_catch2_test(wfg_pcm24_artifact_tests
    SOURCES pcm24_artifact_tests.cpp
    LIBRARIES wfg_pcm24_artifact)
wfg_add_catch2_test(wfg_stream_wav_tests
    SOURCES stream_wav_tests.cpp
    LIBRARIES wfg_signal_lab wfg_pcm24_artifact)
add_test(NAME wfg-retain-waveform
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_retain_waveform.py"
            --renderer "$<TARGET_FILE:signal_lab_render>")

add_executable(wfg_secondary_wav_host_tests secondary_wav_host_tests.cpp)
target_include_directories(wfg_secondary_wav_host_tests BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/rt1170_host_stubs")
target_include_directories(wfg_secondary_wav_host_tests PRIVATE "${WFG_ROOT}")
target_link_libraries(wfg_secondary_wav_host_tests PRIVATE wfg_signal_lab)
add_test(NAME wfg-secondary-wav-provider COMMAND wfg_secondary_wav_host_tests)

add_executable(wfg_prepared_stream_host_tests prepared_stream_host_tests.cpp "${WFG_ROOT}/rt1170/prepared_stream.cpp")
target_include_directories(wfg_prepared_stream_host_tests BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/rt1170_host_stubs")
target_include_directories(wfg_prepared_stream_host_tests PRIVATE "${WFG_ROOT}" "${WFG_ROOT}/rt1170")
target_link_libraries(wfg_prepared_stream_host_tests PRIVATE wfg_pcm24_artifact wfg_finite_playback Catch2::Catch2WithMain)
target_compile_features(wfg_prepared_stream_host_tests PRIVATE cxx_std_23)
catch_discover_tests(wfg_prepared_stream_host_tests
    TEST_PREFIX "wfg-prepared-stream-owner: "
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS catch2)

add_executable(wfg_prepare_job_host_tests prepare_job_host_tests.cpp "${WFG_ROOT}/rt1170/prepare_job.cpp"
    "${WFG_ROOT}/rt1170/prepared_stream.cpp")
target_include_directories(wfg_prepare_job_host_tests BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/rt1170_host_stubs")
target_include_directories(wfg_prepare_job_host_tests PRIVATE "${WFG_ROOT}" "${WFG_ROOT}/rt1170")
target_link_libraries(wfg_prepare_job_host_tests PRIVATE wfg_signal_lab wfg_pcm24_artifact)
wfg_use_build_info(wfg_prepare_job_host_tests)
if(NOT MSVC)
    target_compile_options(wfg_prepare_job_host_tests PRIVATE -ffp-contract=off -fno-fast-math)
endif()
add_test(NAME wfg-prepare-job-owner COMMAND wfg_prepare_job_host_tests)

add_executable(wfg_encoder_plugin_tests encoder_plugin_tests.cpp)
target_link_libraries(wfg_encoder_plugin_tests PRIVATE wfg_encoder_plugin)
if(WFG_ENABLE_ENCODER_PLUGINS)
    target_compile_definitions(wfg_encoder_plugin_tests PRIVATE WFG_TEST_EXPECT_PLUGIN=1)
endif()
add_test(NAME wfg-encoder-plugin COMMAND wfg_encoder_plugin_tests)

add_executable(wfg_encoder_registry_tests encoder_registry_tests.cpp "${WFG_ROOT}/waveform-source/encoder_registry.cpp")
target_link_libraries(wfg_encoder_registry_tests PRIVATE wfg_waveform_source)
target_compile_definitions(wfg_encoder_registry_tests PRIVATE WFG_ENABLE_ENCODER_PLUGINS=1)
add_test(NAME wfg-encoder-registry COMMAND wfg_encoder_registry_tests)

# The public example is independently usable; exercise its complete PCM24 path.
add_executable(wfg_example_encoder_tests encoder_plugin_tests.cpp
    "${WFG_ROOT}/waveform-source/encoder_registry.cpp"
    "${WFG_ROOT}/examples/encoder-plugin/registry.cpp")
target_link_libraries(wfg_example_encoder_tests PRIVATE wfg_waveform_source)
target_compile_definitions(wfg_example_encoder_tests PRIVATE WFG_ENABLE_ENCODER_PLUGINS=1
    WFG_TEST_EXPECT_PLUGIN=1 WFG_TEST_EXAMPLE_PLUGIN=1)
add_test(NAME wfg-example-encoder COMMAND wfg_example_encoder_tests)

foreach(variant IN ITEMS disabled readonly)
    add_executable(wfg_tx_${variant}_tests tx_unavailable_tests.cpp "${WFG_ROOT}/rt1170/tx_artifact_unavailable.cpp")
    target_include_directories(wfg_tx_${variant}_tests BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/rt1170_host_stubs")
    # error_map.hpp, which names the BUSY refusal, reads the live controller's refusals.
    target_include_directories(wfg_tx_${variant}_tests PRIVATE "${WFG_ROOT}" "${WFG_ROOT}/rt1170" "${WFG_ROOT}/signal-lab/include")
    if(variant STREQUAL "readonly")
        target_compile_definitions(wfg_tx_${variant}_tests PRIVATE WFG_MSC_READ_ONLY=1)
    endif()
    add_test(NAME wfg-tx-${variant} COMMAND wfg_tx_${variant}_tests)
endforeach()
add_executable(wfg_safety_contract_tests safety_contract_tests.cpp)
target_link_libraries(wfg_safety_contract_tests PRIVATE wfg_waveform_source wfg_signal_lab)
add_test(NAME wfg-safety-contracts COMMAND wfg_safety_contract_tests)
add_test(NAME wfg-dependencies
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_dependencies.py")
add_test(NAME wfg-build-info
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_build_info.py"
            --cmake "${CMAKE_COMMAND}" --git "${GIT_EXECUTABLE}"
            --script "${WFG_ROOT}/cmake/wfg_build_info.cmake")

# common/status.hpp: Result over std::expected<T, Status>, for any T.
wfg_add_catch2_test(wfg_status_tests
    SOURCES status_tests.cpp "${WFG_ROOT}/common/status.cpp")
target_include_directories(wfg_status_tests PRIVATE "${WFG_ROOT}")
# The critical-error path: value() of an error reaches the installed handler
# (common/status.cpp) with its Status and call site, and the firmware's
# handler (rt1170/critical_error.cpp, over the test's platform calls) records
# them. One run per value() form, whose test handler ends the process: exit 0
# when right. "direct" also checks the call site, and "installed" runs the
# firmware's own handler as install_critical_error_handler() installs it (a
# process with no handler installed traps).
add_executable(wfg_critical_error_tests critical_error_tests.cpp "${WFG_ROOT}/common/status.cpp"
    "${WFG_ROOT}/rt1170/critical_error.cpp")
target_include_directories(wfg_critical_error_tests PRIVATE "${WFG_ROOT}" "${WFG_ROOT}/rt1170")
add_test(NAME wfg-critical-error COMMAND wfg_critical_error_tests)
foreach(form IN ITEMS lvalue const rvalue direct installed)
    add_test(NAME wfg-critical-error-${form} COMMAND wfg_critical_error_tests --value-of-error ${form})
endforeach()
# The host tools' handler (common/host_critical_error.cpp): a test build of
# signal_lab_render (WFG_CRITICAL_ERROR_PROBE) takes the value of a failed
# Result on --critical-error-probe, and the test checks the report line on
# stderr and the exit status; the tool itself has no such option, and every
# host tool's main() installs the handler first (the test reads the sources).
add_executable(wfg_signal_lab_render_probe "${WFG_ROOT}/host/render/signal_lab_render.cpp"
    "${WFG_ROOT}/host/render/live_replay.cpp" "${WFG_ROOT}/common/live_protocol.cpp"
    "${WFG_ROOT}/common/host_critical_error.cpp")
target_include_directories(wfg_signal_lab_render_probe PRIVATE "${WFG_ROOT}")
target_link_libraries(wfg_signal_lab_render_probe PRIVATE wfg_signal_lab wfg_pcm24_artifact)
target_compile_definitions(wfg_signal_lab_render_probe PRIVATE WFG_CRITICAL_ERROR_PROBE=1)
if(NOT MSVC)
    target_compile_options(wfg_signal_lab_render_probe PRIVATE -Wall -Wextra -Wpedantic -Werror ${WFG_SIGNAL_LAB_COMPILE_OPTIONS})
endif()
add_test(NAME wfg-host-critical-error
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_host_critical_error.py"
            --probe "$<TARGET_FILE:wfg_signal_lab_render_probe>" --tool "$<TARGET_FILE:signal_lab_render>")
# The application boundary's error map (rt1170/error_map.hpp) against the
# names, numbers and refusal codes docs/live-control-protocol.md publishes.
wfg_add_catch2_test(wfg_error_map_tests
    SOURCES error_map_tests.cpp "${WFG_ROOT}/common/status.cpp")
target_include_directories(wfg_error_map_tests PRIVATE "${WFG_ROOT}" "${WFG_ROOT}/rt1170" "${WFG_ROOT}/signal-lab/include")
target_compile_definitions(wfg_error_map_tests PRIVATE WFG_PROTOCOL_DOCUMENT=\"${WFG_ROOT}/docs/live-control-protocol.md\")

wfg_add_catch2_test(wfg_live_protocol_tests
    SOURCES live_protocol_tests.cpp "${WFG_ROOT}/common/live_protocol.cpp" "${WFG_ROOT}/common/status.cpp")
target_include_directories(wfg_live_protocol_tests PRIVATE "${WFG_ROOT}")
# The request corpus (live_protocol_corpus.hpp): each seed's outcome and the
# digest of its edits' outcomes, against the record file the parser wrote.
add_executable(wfg_live_protocol_corpus_tests live_protocol_corpus_tests.cpp "${WFG_ROOT}/common/live_protocol.cpp" "${WFG_ROOT}/common/status.cpp")
target_include_directories(wfg_live_protocol_corpus_tests PRIVATE "${WFG_ROOT}")
target_compile_features(wfg_live_protocol_corpus_tests PRIVATE cxx_std_23)
add_test(NAME wfg-live-protocol-corpus COMMAND wfg_live_protocol_corpus_tests "${CMAKE_CURRENT_SOURCE_DIR}/live_protocol_corpus.txt")
# The CUT injector's portable logic (common/cut_schedule.hpp). Its --predict
# mode is what the Python predictor (host/cut_schedule.py) is checked against:
# the host test finds it through WFG_CUT_PREDICTOR.
add_executable(wfg_cut_schedule_tests cut_schedule_tests.cpp "${WFG_ROOT}/common/status.cpp")
target_include_directories(wfg_cut_schedule_tests PRIVATE "${WFG_ROOT}")
target_compile_features(wfg_cut_schedule_tests PRIVATE cxx_std_23)
add_test(NAME wfg-cut-schedule COMMAND wfg_cut_schedule_tests)
# PLAYLIST batch mode's portable logic (common/playlist.hpp): the file parser,
# the item table, the result ring and the timing rules.
add_executable(wfg_playlist_tests playlist_tests.cpp "${WFG_ROOT}/common/status.cpp")
target_include_directories(wfg_playlist_tests PRIVATE "${WFG_ROOT}")
target_compile_features(wfg_playlist_tests PRIVATE cxx_std_23)
add_test(NAME wfg-playlist COMMAND wfg_playlist_tests)
# The host's playlist tool (host/make_playlist.py) against the same rules, file
# by file against the firmware's parser through its --check mode, and its
# start offsets against the firmware's next_start_index() (--next-start); its
# scenario predictions through the host renderer.
add_test(NAME wfg-playlist-host COMMAND "${Python3_EXECUTABLE}" -m unittest tests.test_make_playlist -v
    WORKING_DIRECTORY "${WFG_ROOT}")
set_tests_properties(wfg-playlist-host PROPERTIES ENVIRONMENT
    "WFG_PLAYLIST_CHECKER=$<TARGET_FILE:wfg_playlist_tests>;WFG_SIGNAL_LAB_RENDER=$<TARGET_FILE:signal_lab_render>")
# The host predictor against the firmware's cut stage, on clean cards and on
# scenario renders (the cuts act on the engine's output). With WFG_FSRAND_WAV,
# image A's CUT card, it and the player (wfg-player-live-owner) also
# reproduce image A's CUT rows; the card is not in the repository, so without
# it those rows are skipped.
set(WFG_FSRAND_WAV "" CACHE FILEPATH "FSRAND.WAV, image A's CUT card (2880000 frames, data CRC-32 0x93B97321), for the image A CUT rows")
add_test(NAME wfg-cut-host COMMAND "${Python3_EXECUTABLE}" -m unittest tests.test_cut_schedule -v
    WORKING_DIRECTORY "${WFG_ROOT}")
set_tests_properties(wfg-cut-host PROPERTIES ENVIRONMENT
    "WFG_CUT_PREDICTOR=$<TARGET_FILE:wfg_cut_schedule_tests>;WFG_SIGNAL_LAB_RENDER=$<TARGET_FILE:signal_lab_render>;WFG_FSRAND_WAV=${WFG_FSRAND_WAV}")
add_test(NAME wfg-live-host COMMAND "${Python3_EXECUTABLE}" -m unittest tests.test_live_control -v
    WORKING_DIRECTORY "${WFG_ROOT}")
add_test(NAME wfg-usb-storm-host COMMAND "${Python3_EXECUTABLE}" -m unittest tests.test_usb_storm -v
    WORKING_DIRECTORY "${WFG_ROOT}")
add_test(NAME wfg-media-eject-host COMMAND "${Python3_EXECUTABLE}" -m unittest tests.test_media_eject -v
    WORKING_DIRECTORY "${WFG_ROOT}")
add_test(NAME wfg-usb-bench-host COMMAND "${Python3_EXECUTABLE}" -m unittest tests.test_usb_bench -v
    WORKING_DIRECTORY "${WFG_ROOT}")
add_test(NAME wfg-tx-upload-host COMMAND "${Python3_EXECUTABLE}" -m unittest tests.test_tx_upload -v
    WORKING_DIRECTORY "${WFG_ROOT}")
add_executable(wfg_tx_protocol_tests tx_protocol_tests.cpp
    "${WFG_ROOT}/common/tx_protocol.cpp"
    "${WFG_ROOT}/common/dd008/cdc_mux_protocol.cpp"
    "${WFG_ROOT}/common/dd008/cdc_mux_session.cpp"
    "${WFG_ROOT}/common/status.cpp")
target_include_directories(wfg_tx_protocol_tests PRIVATE "${WFG_ROOT}")
add_test(NAME wfg-tx-protocol COMMAND wfg_tx_protocol_tests)

add_executable(wfg_tx_usb_host_tests tx_usb_host_tests.cpp
    "${WFG_ROOT}/common/tx_protocol.cpp"
    "${WFG_ROOT}/common/dd008/cdc_mux_protocol.cpp"
    "${WFG_ROOT}/common/dd008/cdc_mux_session.cpp"
    "${WFG_ROOT}/common/status.cpp")
target_include_directories(wfg_tx_usb_host_tests BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/rt1170_host_stubs")
target_include_directories(wfg_tx_usb_host_tests PRIVATE "${WFG_ROOT}" "${WFG_ROOT}/rt1170")
target_compile_definitions(wfg_tx_usb_host_tests PRIVATE
    WFG_BUILD_ID="wfg-test")
add_test(NAME wfg-tx-usb-owner COMMAND wfg_tx_usb_host_tests)

# The player owner loop and CDC service, built once per player image: INFO
# reports each image's own PID, and the read-only one says so. The USB sound
# card's stream (usb_audio.cpp) is the real one, behind a software IN FIFO and
# a simulated USB ISR in place of TinyUSB. The mono build (WFG_UAC_MONO_ALT,
# writable PID) also plays through the 16-bit mono alternate. The sd24 build is
# a WFG_SD_CLOCK_24MHZ player (WFG_SD_USE_BOOTCLOCKRUN_USDHC1, writable PID),
# which INFO marks with the sd-clock-24mhz capability.
foreach(variant IN ITEMS writable readonly mono sd24)
    if(variant STREQUAL "writable")
        set(player_live_target wfg_player_live_host_tests)
        set(player_live_test wfg-player-live-owner)
    else()
        set(player_live_target wfg_player_live_${variant}_host_tests)
        set(player_live_test wfg-player-live-owner-${variant})
    endif()
    add_executable(${player_live_target} player_live_host_tests.cpp "${WFG_ROOT}/common/live_protocol.cpp"
        "${WFG_ROOT}/waveform-source/encoder_registry.cpp" "${WFG_ROOT}/rt1170/usb_audio.cpp" "${WFG_ROOT}/rt1170/usb_ep0_order.cpp"
        "${WFG_ROOT}/common/status.cpp" "${WFG_ROOT}/rt1170/card_listing.cpp")
    target_include_directories(${player_live_target} BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/rt1170_host_stubs")
    target_include_directories(${player_live_target} PRIVATE "${WFG_ROOT}" "${WFG_ROOT}/rt1170")
    target_link_libraries(${player_live_target} PRIVATE wfg_signal_lab wfg_pcm24_artifact wfg_finite_playback Catch2::Catch2)
    target_compile_features(${player_live_target} PRIVATE cxx_std_23)
    target_sources(${player_live_target} PRIVATE "${WFG_ROOT}/rt1170/prepared_playback.cpp")
    target_compile_definitions(${player_live_target} PRIVATE WFG_PLAYER_IMAGE=1
        WFG_BUILD_ID="wfg-test")
    if(variant STREQUAL "readonly")
        target_compile_definitions(${player_live_target} PRIVATE WFG_MSC_READ_ONLY=1)
    elseif(variant STREQUAL "mono")
        target_compile_definitions(${player_live_target} PRIVATE WFG_UAC_MONO_ALT=1)
    elseif(variant STREQUAL "sd24")
        target_compile_definitions(${player_live_target} PRIVATE WFG_SD_USE_BOOTCLOCKRUN_USDHC1=1)
    endif()
    if(variant STREQUAL "readonly")
        target_sources(${player_live_target} PRIVATE "${WFG_ROOT}/rt1170/prepare_job_unavailable.cpp"
            "${WFG_ROOT}/rt1170/prepared_stream_unavailable.cpp")
    else()
        target_sources(${player_live_target} PRIVATE "${WFG_ROOT}/rt1170/prepare_job.cpp"
            "${WFG_ROOT}/rt1170/prepared_stream.cpp")
    endif()
    wfg_use_build_info(${player_live_target})
    if(NOT MSVC)
        target_compile_options(${player_live_target} PRIVATE -ffp-contract=off -fno-fast-math)
    endif()
    # The writable image also plays image A's CUT rows on FSRAND, when given,
    # and runs the USB task's entry after TinyUSB's start fails (the task
    # suspends itself) and succeeds (it runs TinyUSB's task): each run ends at
    # that call (--usb-task).
    if(variant STREQUAL "writable")
        catch_discover_tests(${player_live_target}
            TEST_PREFIX "${player_live_test}: "
            DISCOVERY_MODE PRE_TEST
            PROPERTIES LABELS catch2 ENVIRONMENT "WFG_FSRAND_WAV=${WFG_FSRAND_WAV}")
        foreach(usb_task_form IN ITEMS stack-fails stack-starts)
            add_test(NAME wfg-usb-task-${usb_task_form} COMMAND ${player_live_target} --usb-task ${usb_task_form})
        endforeach()
    else()
        catch_discover_tests(${player_live_target}
            TEST_PREFIX "${player_live_test}: "
            DISCOVERY_MODE PRE_TEST
            PROPERTIES LABELS catch2)
    endif()
endforeach()

wfg_add_catch2_test(wfg_crc32_tests
    SOURCES crc32_tests.cpp)
target_include_directories(wfg_crc32_tests PRIVATE "${WFG_ROOT}")

# The player's SD clocks (rt1170/platform/waveform_sd_clock.h, a C header the
# firmware's waveform_sd.c includes) against the SDK's divider rule.
add_executable(wfg_sd_clock_tests sd_clock_tests.cpp)
target_include_directories(wfg_sd_clock_tests PRIVATE "${WFG_ROOT}")
add_test(NAME wfg-sd-clock COMMAND wfg_sd_clock_tests)

# Sound-card (UAC2) stream and control logic, with a software FIFO behind the
# m110_tinyusb_audio_in_* shim in place of TinyUSB. The FreeRTOS stubs stand
# in for the critical section and tick of TinyUSB's assert hook. Built once as
# the player images are by default, and once with the mono alternate
# (WFG_UAC_MONO_ALT), which changes the alternates the C entry point opens.
foreach(variant IN ITEMS stereo mono)
    if(variant STREQUAL "stereo")
        set(usb_audio_target wfg_usb_audio_tests)
        set(usb_audio_test wfg-usb-audio)
    else()
        set(usb_audio_target wfg_usb_audio_${variant}_tests)
        set(usb_audio_test wfg-usb-audio-${variant})
    endif()
    add_executable(${usb_audio_target} usb_audio_tests.cpp
        "${WFG_ROOT}/rt1170/usb_audio.cpp"
        "${WFG_ROOT}/common/status.cpp")
    target_include_directories(${usb_audio_target} BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/rt1170_host_stubs")
    target_include_directories(${usb_audio_target} PRIVATE "${WFG_ROOT}" "${WFG_ROOT}/rt1170")
    if(variant STREQUAL "mono")
        target_compile_definitions(${usb_audio_target} PRIVATE WFG_UAC_MONO_ALT=1)
    endif()
    add_test(NAME ${usb_audio_test} COMMAND ${usb_audio_target})
endforeach()

# Owned firmware C compiled for the host keeps the firmware's C policy (C11,
# warnings as errors). The descriptor test reads TinyUSB's headers, which need
# GCC or Clang, as the firmware does.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    enable_language(C)
    set(wfg_host_c_options -Wall -Wextra -Wpedantic -Werror)
    set_source_files_properties(
        "${WFG_ROOT}/rt1170/platform/waveform_msc.c"
        "${WFG_ROOT}/rt1170/usb/usb_descriptors.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/media_msc_stubs/media_msc_host.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/usb_descriptor_stubs/usb_descriptor_bus.c"
        PROPERTIES COMPILE_OPTIONS "${wfg_host_c_options}")
    foreach(variant IN ITEMS writable readonly)
        # MSC ownership, the host-eject latch and the chunked transfers
        # (rt1170/platform/waveform_msc.c) behind C stand-ins for the SD stack,
        # FreeRTOS, the cycle counter and TinyUSB's sense setter. The stand-in
        # tusb.h reads the player's TinyUSB configuration (rt1170/usb), so the
        # MSC chunk size is the firmware's.
        add_executable(wfg_media_msc_${variant}_tests media_msc_tests.cpp "${WFG_ROOT}/rt1170/platform/waveform_msc.c"
            media_msc_stubs/media_msc_host.c)
        target_include_directories(wfg_media_msc_${variant}_tests BEFORE PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/media_msc_stubs" "${CMAKE_CURRENT_SOURCE_DIR}/rt1170_host_stubs")
        target_include_directories(wfg_media_msc_${variant}_tests PRIVATE "${WFG_ROOT}/rt1170" "${WFG_ROOT}/rt1170/platform"
            "${WFG_ROOT}/rt1170/usb")
        target_compile_definitions(wfg_media_msc_${variant}_tests PRIVATE WFG_PLAYER_IMAGE=1 M110_USB_HIGH_SPEED=1
            CFG_TUSB_MCU=OPT_MCU_MIMXRT1XXX)
        set_target_properties(wfg_media_msc_${variant}_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
        if(variant STREQUAL "readonly")
            target_compile_definitions(wfg_media_msc_${variant}_tests PRIVATE WFG_MSC_READ_ONLY=1)
        endif()
        add_test(NAME wfg-media-msc-${variant} COMMAND wfg_media_msc_${variant}_tests)
        # The media's start runs once per process: main()'s, and MEDIA HOST's
        # before it, each in a process of its own.
        add_test(NAME wfg-media-msc-${variant}-boot COMMAND wfg_media_msc_${variant}_tests boot)
        add_test(NAME wfg-media-msc-${variant}-host-first COMMAND wfg_media_msc_${variant}_tests host-first)
    endforeach()
    # The EVK microSD socket's card detect (rt1170/platform/waveform_sd.c) with
    # the player image's SD definitions, behind C stand-ins for NXP's clock,
    # GPIO, IOMUXC and SD host drivers.
    wfg_add_catch2_test(wfg_sd_socket_tests
        SOURCES sd_socket_tests.cpp "${WFG_ROOT}/rt1170/platform/waveform_sd.c" sd_socket_stubs/sd_socket_host.c)
    set_source_files_properties("${WFG_ROOT}/rt1170/platform/waveform_sd.c" "${CMAKE_CURRENT_SOURCE_DIR}/sd_socket_stubs/sd_socket_host.c"
        PROPERTIES COMPILE_OPTIONS "${wfg_host_c_options}")
    target_include_directories(wfg_sd_socket_tests BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/sd_socket_stubs")
    target_include_directories(wfg_sd_socket_tests PRIVATE "${WFG_ROOT}/rt1170" "${WFG_ROOT}/rt1170/platform")
    target_compile_definitions(wfg_sd_socket_tests PRIVATE WFG_PLAYER_IMAGE=1 WFG_SD_DISABLE_HOST_VOLTAGE_CONTROL=1
        WFG_SD_ALWAYS_PRESENT_CARD_DETECT=1 WFG_SD_IDENTIFICATION_CLOCK_HZ=103125U)
    set_target_properties(wfg_sd_socket_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    # The player's composite USB descriptors (rt1170/usb/usb_descriptors.c)
    # compiled with the firmware's TinyUSB configuration: once per player PID,
    # and once with the mono alternate (WFG_UAC_MONO_ALT) on the writable PID.
    foreach(variant IN ITEMS writable readonly mono)
        add_executable(wfg_usb_descriptor_${variant}_tests usb_descriptor_tests.cpp "${WFG_ROOT}/rt1170/usb/usb_descriptors.c"
            usb_descriptor_stubs/usb_descriptor_bus.c)
        target_include_directories(wfg_usb_descriptor_${variant}_tests BEFORE PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/usb_descriptor_stubs" "${CMAKE_CURRENT_SOURCE_DIR}/rt1170_host_stubs")
        target_include_directories(wfg_usb_descriptor_${variant}_tests PRIVATE "${WFG_ROOT}/rt1170" "${WFG_ROOT}/rt1170/usb")
        target_include_directories(wfg_usb_descriptor_${variant}_tests SYSTEM PRIVATE "${WFG_ROOT}/third_party/tinyusb/src")
        target_compile_definitions(wfg_usb_descriptor_${variant}_tests PRIVATE WFG_PLAYER_IMAGE=1 M110_USB_HIGH_SPEED=1
            CFG_TUSB_MCU=OPT_MCU_MIMXRT1XXX M110_TINYUSB_SINGLE_OWNER_OS_NONE=1)
        set_target_properties(wfg_usb_descriptor_${variant}_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
        if(variant STREQUAL "readonly")
            target_compile_definitions(wfg_usb_descriptor_${variant}_tests PRIVATE WFG_MSC_READ_ONLY=1)
        elseif(variant STREQUAL "mono")
            target_compile_definitions(wfg_usb_descriptor_${variant}_tests PRIVATE WFG_UAC_MONO_ALT=1)
        endif()
        add_test(NAME wfg-usb-descriptors-${variant} COMMAND wfg_usb_descriptor_${variant}_tests)
    endforeach()
    # The USB interrupt's EP0 order (rt1170/usb_ep0_order.*, usb/usb_ep0_order_glue.c)
    # and the race it closes (#32) on the pinned TinyUSB: its device stack and
    # CDC driver, built for the host on the ChipIdea controller's numbers and
    # without an RTOS, with usb_ep0_race_stubs/usb_ep0_race.c in the driver's
    # place. TinyUSB's sources keep their upstream compiler policy.
    set(wfg_tinyusb_src "${WFG_ROOT}/third_party/tinyusb/src")
    add_executable(wfg_usb_ep0_order_tests usb_ep0_order_tests.cpp "${WFG_ROOT}/rt1170/usb_ep0_order.cpp"
        "${WFG_ROOT}/common/status.cpp" "${WFG_ROOT}/rt1170/usb/usb_ep0_order_glue.c" usb_ep0_race_stubs/usb_ep0_race.c
        "${wfg_tinyusb_src}/tusb.c" "${wfg_tinyusb_src}/common/tusb_fifo.c" "${wfg_tinyusb_src}/device/usbd.c"
        "${wfg_tinyusb_src}/class/cdc/cdc_device.c")
    set_source_files_properties("${WFG_ROOT}/rt1170/usb/usb_ep0_order_glue.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/usb_ep0_race_stubs/usb_ep0_race.c"
        PROPERTIES COMPILE_OPTIONS "${wfg_host_c_options}")
    target_include_directories(wfg_usb_ep0_order_tests BEFORE PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/usb_ep0_race_stubs" "${CMAKE_CURRENT_SOURCE_DIR}/rt1170_host_stubs")
    target_include_directories(wfg_usb_ep0_order_tests PRIVATE "${WFG_ROOT}" "${WFG_ROOT}/rt1170" "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(wfg_usb_ep0_order_tests SYSTEM PRIVATE "${wfg_tinyusb_src}")
    set_target_properties(wfg_usb_ep0_order_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    add_test(NAME wfg-usb-ep0-order COMMAND wfg_usb_ep0_order_tests)
    # A null setup given to wfg_usb_ep0_order_release() with a SETUP held is a
    # critical error (the test's handler checks it and exits 0), not "none held".
    add_test(NAME wfg-usb-ep0-order-release-without-setup COMMAND wfg_usb_ep0_order_tests --release-without-setup)
    # The TinyUSB port's bring-up (rt1170/usb/usb_tinyusb_port.c) as the player
    # image builds it, on the pinned TinyUSB's headers, behind C stand-ins for
    # the USB clocks, the PHY's and the controller's registers, TinyUSB's
    # device stack and the EP0 order's entry points.
    add_executable(wfg_usb_port_tests usb_port_tests.cpp "${WFG_ROOT}/rt1170/usb/usb_tinyusb_port.c" usb_port_stubs/usb_port_host.c)
    set_source_files_properties("${WFG_ROOT}/rt1170/usb/usb_tinyusb_port.c" "${CMAKE_CURRENT_SOURCE_DIR}/usb_port_stubs/usb_port_host.c"
        PROPERTIES COMPILE_OPTIONS "${wfg_host_c_options}")
    target_include_directories(wfg_usb_port_tests BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/usb_port_stubs")
    target_include_directories(wfg_usb_port_tests PRIVATE "${WFG_ROOT}/rt1170" "${WFG_ROOT}/rt1170/usb")
    target_include_directories(wfg_usb_port_tests SYSTEM PRIVATE "${wfg_tinyusb_src}")
    target_compile_definitions(wfg_usb_port_tests PRIVATE WFG_PLAYER_IMAGE=1 M110_USB_HIGH_SPEED=1 CFG_TUSB_MCU=OPT_MCU_MIMXRT1XXX
        M110_TINYUSB_SINGLE_OWNER_OS_NONE=1)
    set_target_properties(wfg_usb_port_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    add_test(NAME wfg-usb-port COMMAND wfg_usb_port_tests)
endif()

add_executable(wfg_tx_artifact_host_tests tx_artifact_host_tests.cpp
    "${WFG_ROOT}/rt1170/tx_artifact.cpp" "${WFG_ROOT}/waveform-source/encoder_registry.cpp" tx_artifact_test_encoder.cpp "${WFG_ROOT}/common/tx_protocol.cpp")
target_include_directories(wfg_tx_artifact_host_tests BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/rt1170_host_stubs")
target_include_directories(wfg_tx_artifact_host_tests PRIVATE "${WFG_ROOT}" "${WFG_ROOT}/rt1170")
target_link_libraries(wfg_tx_artifact_host_tests PRIVATE wfg_waveform_source)
target_compile_definitions(wfg_tx_artifact_host_tests PRIVATE WFG_ENABLE_ENCODER_PLUGINS=1
    WFG_BUILD_ID="wfg-test")
if(NOT MSVC)
    target_compile_options(wfg_tx_artifact_host_tests PRIVATE -ffp-contract=off -fno-fast-math)
endif()
add_test(NAME wfg-tx-artifact-owner COMMAND wfg_tx_artifact_host_tests)

add_executable(wfg_contract_tests contract_tests.cpp)
target_include_directories(wfg_contract_tests PRIVATE "${WFG_ROOT}")
target_compile_features(wfg_contract_tests PRIVATE cxx_std_23)

if(MSVC)
    target_compile_options(wfg_contract_tests PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(wfg_contract_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_test(NAME wfg-contract-cpp COMMAND wfg_contract_tests)
# ENGINE?'s reason budget (status_contract.hpp engine_reason_max_chars) against
# every string literal of the signal-lab library and the error map, so a
# message ENGINE? would cut fails when it is written.
add_test(NAME wfg-engine-reason-budget COMMAND "${Python3_EXECUTABLE}" -m unittest tests.test_engine_reason_budget -v
    WORKING_DIRECTORY "${WFG_ROOT}")

wfg_add_catch2_test(wfg_capture_core_tests
    SOURCES capture_core_tests.cpp)
target_include_directories(wfg_capture_core_tests PRIVATE "${WFG_ROOT}")
set_target_properties(wfg_capture_core_tests PROPERTIES ENABLE_EXPORTS OFF)
if(WIN32)
    wfg_add_catch2_test(wfg_winmm_audio_tests
        SOURCES winmm_audio_tests.cpp "${WFG_ROOT}/host/capture/winmm_audio.cpp"
            "${WFG_ROOT}/common/status.cpp"
        LIBRARIES winmm)
    target_include_directories(wfg_winmm_audio_tests PRIVATE "${WFG_ROOT}")
    set_target_properties(wfg_winmm_audio_tests PROPERTIES ENABLE_EXPORTS OFF)
endif()
if(MSVC)
    target_compile_options(wfg_capture_core_tests PRIVATE /W4 /WX /permissive-)
    if(WIN32)
        target_compile_options(wfg_winmm_audio_tests PRIVATE /W4 /WX /permissive-)
    endif()
else()
    target_compile_options(wfg_capture_core_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    if(WIN32)
        target_compile_options(wfg_winmm_audio_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endif()
add_test(
    NAME wfg-contract-json
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_contracts.py"
            --root "${WFG_ROOT}")
add_test(
    NAME wfg-stage-wav
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_stage_wav.py")
add_test(
    NAME wfg-analyze-capture
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_analyze_capture.py")
add_test(
    NAME wfg-signal-lab
    COMMAND "${Python3_EXECUTABLE}" -m unittest tests.test_signal_lab tests.test_cw_pulse tests.test_cw_sweep tests.test_cw_markov tests.test_cw_fsk tests.test_cw_faded tests.test_stream_identity -v
    WORKING_DIRECTORY "${WFG_ROOT}")
# The Watterson validation driver's rules (signal-lab/validation/
# run_watterson_validation.py): criterion 5's scoring and render reuse,
# criterion 7's coverage judgement, on fake simulators and canned results;
# no simulator or render runs.
add_test(
    NAME wfg-watterson-validation-rules
    COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover -s "${WFG_ROOT}/signal-lab/validation" -p "test_*.py" -v
    WORKING_DIRECTORY "${WFG_ROOT}")
add_test(NAME wfg-pcm24-python
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_pcm24.py")
# The firmware's post-link placement check (scripts/check-memory-placement.py):
# the MSC chunk buffers in the non-cacheable OCRAM window.
add_test(NAME wfg-memory-placement
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_memory_placement.py")
# And its post-link stack check (scripts/check-stack-usage.py): the deepest
# chain against the stack and its reserve, owned recursion, frames that are not
# static, and the indirect-call rules, on call-graph excerpts; the USB task's
# rules against TinyUSB's class driver table (third_party/tinyusb usbd.c) and
# rt1170/usb/tusb_config.h, and both tasks checked by rt1170/CMakeLists.txt.
add_test(NAME wfg-stack-usage
    COMMAND "${Python3_EXECUTABLE}" -B -m unittest tests.test_check_stack_usage -v
    WORKING_DIRECTORY "${WFG_ROOT}")
# The host CI's style gate (scripts/check-style.py) on its own rules: synthetic
# K&R and Allman lines in a scratch Git repository, so a pattern that stops
# catching a K&R form fails here although the tree has none (needs git).
add_test(NAME wfg-check-style-rules COMMAND "${Python3_EXECUTABLE}" -m unittest tests.test_check_style -v
    WORKING_DIRECTORY "${WFG_ROOT}")
# And the complexity ratchet (scripts/check-complexity.py) on canned lizard
# reports: the file table, the sparse-file rule, a listed function lizard no
# longer measures, and --update (lizard is not run).
add_test(NAME wfg-check-complexity-rules COMMAND "${Python3_EXECUTABLE}" -m unittest tests.test_check_complexity -v
    WORKING_DIRECTORY "${WFG_ROOT}")
# The ratchet on the tree itself, as the host CI's checks job runs it, where
# this Python has the lizard version the baseline was made with: a commit that
# changes a function count or a CCN without --update then fails here too, not
# only in CI. Elsewhere (the CI's host job installs no lizard) it is not
# registered, and configure says so.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${WFG_ROOT}/scripts/complexity-baseline.json")
file(READ "${WFG_ROOT}/scripts/complexity-baseline.json" wfg_complexity_baseline)
string(JSON wfg_lizard_pinned GET "${wfg_complexity_baseline}" lizard)
execute_process(COMMAND "${Python3_EXECUTABLE}" -m lizard --version RESULT_VARIABLE wfg_lizard_result
    OUTPUT_VARIABLE wfg_lizard_found OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(wfg_lizard_result EQUAL 0 AND wfg_lizard_found STREQUAL wfg_lizard_pinned)
    add_test(NAME wfg-check-complexity COMMAND "${Python3_EXECUTABLE}" "${WFG_ROOT}/scripts/check-complexity.py" --root "${WFG_ROOT}")
else()
    message(STATUS "lizard ${wfg_lizard_pinned} is not installed for ${Python3_EXECUTABLE}: wfg-check-complexity "
        "(the ratchet on the tree) is not registered; the host CI's checks job runs it")
endif()
# The error-style gate (scripts/check-error-style.py, #50) on the tree: no owned
# function reports a failure through an error out-parameter (const char**, a
# std::string& error ...) or keeps its text for an accessor (a set_error() and
# error() pair), only as a Status or Result; and its rules on synthetic
# sources and a fixture repository. Both need git.
add_test(NAME wfg-check-error-style COMMAND "${Python3_EXECUTABLE}" "${WFG_ROOT}/scripts/check-error-style.py" --root "${WFG_ROOT}")
add_test(NAME wfg-check-error-style-rules COMMAND "${Python3_EXECUTABLE}" -m unittest tests.test_check_error_style -v
    WORKING_DIRECTORY "${WFG_ROOT}")
# The bool-return gate (scripts/check-bool-returns.py, #50) on the tree:
# every owned function that returns bool is listed in
# scripts/bool-returns-baseline.json as a predicate or a vendor callback, and
# the fallible list is empty; and its rules on synthetic sources and a fixture
# repository. Both need git.
add_test(NAME wfg-check-bool-returns COMMAND "${Python3_EXECUTABLE}" "${WFG_ROOT}/scripts/check-bool-returns.py" --root "${WFG_ROOT}")
add_test(NAME wfg-check-bool-returns-rules COMMAND "${Python3_EXECUTABLE}" -m unittest tests.test_check_bool_returns -v
    WORKING_DIRECTORY "${WFG_ROOT}")
# The clang tools the checks below run, each at LLVM 19 or later
# (cmake/wfg_clang_tools.cmake): <tool>-19 first, then <tool>, and a candidate
# that reports an older LLVM is passed over, as ubuntu-24.04's /usr/bin/clang-tidy
# (clang-tidy 18, which rejects --exclude-header-filter) is. wfg-clang-tools-version
# holds the search to that, and each tool found to LLVM 19 or later.
include("${WFG_ROOT}/cmake/wfg_clang_tools.cmake")
wfg_find_clang_tool(WFG_CLANG_QUERY clang-query DOC "clang-query 19 or later, for the C status truth check (#51)")
wfg_find_clang_tool(WFG_CLANG_TIDY clang-tidy DOC "clang-tidy 19 or later, for scripts/clang-tidy.ps1")
add_test(NAME wfg-clang-tools-version
    COMMAND "${CMAKE_COMMAND}" "-DWFG_ROOT=${WFG_ROOT}" "-DWFG_CLANG_TIDY=${WFG_CLANG_TIDY}"
            "-DWFG_CLANG_QUERY=${WFG_CLANG_QUERY}" -P "${WFG_ROOT}/tests/test_clang_tools.cmake")
# The AST check of a C status read as a truth value (scripts/check-c-status-truth.py,
# #51), with clang-query, on this build's own compile database: every owned unit,
# each of its compile commands, and every form the text gate cannot see (a cast,
# assert(), a stored status); and its rules on tests/c_status_truth and on canned
# clang-query output. Without clang-query 19 (clang-tools-19 on Ubuntu, beside
# clang-tidy-19) neither is registered, and configure says so.
if(WFG_CLANG_QUERY)
    add_test(NAME wfg-check-c-status-truth
        COMMAND "${Python3_EXECUTABLE}" "${WFG_ROOT}/scripts/check-c-status-truth.py" --root "${WFG_ROOT}"
                --build-dir "${CMAKE_BINARY_DIR}" --clang-query "${WFG_CLANG_QUERY}" --jobs "${WFG_HOST_JOBS}")
    add_test(NAME wfg-check-c-status-truth-rules COMMAND "${Python3_EXECUTABLE}" -m unittest tests.test_check_c_status_truth -v
        WORKING_DIRECTORY "${WFG_ROOT}")
    set_tests_properties(wfg-check-c-status-truth-rules PROPERTIES ENVIRONMENT "WFG_CLANG_QUERY=${WFG_CLANG_QUERY}")
else()
    message(STATUS "clang-query 19 or later is not installed: wfg-check-c-status-truth and its rules are not registered; "
        "the host CI's host job runs them")
endif()
# scripts/clang-tidy.ps1 reads a compile database that carries the post-link
# stack check's GCC flags, as every firmware database does, through a copy
# without them: -fcallgraph-info=su, which clang rejects, and -fstack-usage,
# which only asks for a .su file. A command string and an arguments list, each
# over one owned host source. Needs pwsh and clang-tidy 19; without them it is
# not registered.
find_program(WFG_PWSH NAMES pwsh DOC "PowerShell 7, for scripts/clang-tidy.ps1")
if(WFG_PWSH AND WFG_CLANG_TIDY)
    add_test(NAME wfg-clang-tidy-gcc-flags COMMAND "${Python3_EXECUTABLE}" -m unittest tests.test_clang_tidy_database -v
        WORKING_DIRECTORY "${WFG_ROOT}")
    set_tests_properties(wfg-clang-tidy-gcc-flags PROPERTIES ENVIRONMENT "WFG_CLANG_TIDY=${WFG_CLANG_TIDY}")
else()
    message(STATUS "pwsh or clang-tidy 19 or later is not installed: wfg-clang-tidy-gcc-flags is not registered")
endif()
add_test(
    NAME wfg-signal-lab-render
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_signal_lab_render.py"
            --renderer "$<TARGET_FILE:signal_lab_render>"
            --watterson-renderer "$<TARGET_FILE:wfg_watterson_render>")
  add_test(
      NAME wfg-wideband-profiles
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_wideband_profiles.py"
              --renderer "$<TARGET_FILE:signal_lab_render>")
add_test(
    NAME wfg-wideband-response
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_wideband_response.py"
            --renderer "$<TARGET_FILE:signal_lab_render>")
add_test(
    NAME wfg-wideband-snr
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_wideband_snr.py"
            --renderer "$<TARGET_FILE:signal_lab_render>")
add_test(
    NAME wfg-cw-pulse-render
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_cw_pulse_render.py"
            --renderer "$<TARGET_FILE:signal_lab_render>"
            --watterson-renderer "$<TARGET_FILE:wfg_watterson_render>")
add_test(
    NAME wfg-cw-sweep-render
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_cw_sweep_render.py"
            --renderer "$<TARGET_FILE:signal_lab_render>"
            --watterson-renderer "$<TARGET_FILE:wfg_watterson_render>")
add_test(
    NAME wfg-cw-markov-render
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_cw_markov_render.py"
            --renderer "$<TARGET_FILE:signal_lab_render>"
            --watterson-renderer "$<TARGET_FILE:wfg_watterson_render>")
add_test(
    NAME wfg-cw-fsk-render
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_cw_fsk_render.py"
            --renderer "$<TARGET_FILE:signal_lab_render>"
            --watterson-renderer "$<TARGET_FILE:wfg_watterson_render>")
add_test(
    NAME wfg-cw-faded-render
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_cw_faded_render.py"
            --renderer "$<TARGET_FILE:signal_lab_render>"
            --watterson-renderer "$<TARGET_FILE:wfg_watterson_render>")
add_test(
    NAME wfg-stream-identity-render
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_stream_identity_render.py"
            --renderer "$<TARGET_FILE:signal_lab_render>"
            --watterson-renderer "$<TARGET_FILE:wfg_watterson_render>")
add_test(
    NAME wfg-second-source-render
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_second_source_render.py"
            --renderer "$<TARGET_FILE:signal_lab_render>")

add_test(
    NAME wfg-compose-stereo-wav
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_compose_stereo_wav.py"
            --renderer "$<TARGET_FILE:signal_lab_render>")
# wfg_watterson_render takes its file names as written too (#46): a wildcard
# in an output or a source name matches no file.
add_test(
    NAME wfg-watterson-render-arguments
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_watterson_render_arguments.py"
            --renderer "$<TARGET_FILE:wfg_watterson_render>")
# Its refusals by exit status and text (#50): a source that ends inside its
# data, an output, sidecar or truth file that cannot be created, bad options.
add_test(
    NAME wfg-watterson-render-refusals
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_watterson_render_refusals.py"
            --renderer "$<TARGET_FILE:wfg_watterson_render>")
  add_test(
      NAME wfg-watterson-frequency-truth
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_watterson_frequency_truth.py"
              --renderer "$<TARGET_FILE:wfg_watterson_render>")
# Under MinGW each host tool that takes file names links the runtime's
# CRT_noglob.o (cmake/wfg_noglob.cmake), which turns its wildcard expansion
# off. A tool whose wfg_take_arguments_as_written() call is lost fails here,
# waveform_capture included, which cannot run without a capture device.
if(MINGW)
    foreach(tool IN ITEMS signal_lab_render wfg_watterson_render waveform_capture)
        add_test(NAME wfg-arguments-as-written-${tool}
            COMMAND "${CMAKE_COMMAND}" -E echo "$<JOIN:$<TARGET_PROPERTY:${tool},LINK_LIBRARIES>, >")
        set_tests_properties(wfg-arguments-as-written-${tool} PROPERTIES PASS_REGULAR_EXPRESSION "CRT_noglob[.]o")
    endforeach()
endif()
# waveform_capture's refusals that need no capture device (#50), by exit status
# and text: a command line it refuses, and an input no device has, which it
# looks up without opening anything. The tool is built on Windows only.
if(TARGET waveform_capture)
    add_test(NAME wfg-capture-refusals
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_waveform_capture.py"
                --capture "$<TARGET_FILE:waveform_capture>")
endif()
add_test(
    NAME wfg-project-layout
    COMMAND "${Python3_EXECUTABLE}" "${WFG_ROOT}/scripts/check-project.py"
            --root "${WFG_ROOT}")

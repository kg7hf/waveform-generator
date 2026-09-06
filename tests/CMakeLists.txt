find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

add_executable(wfg_live_protocol_tests live_protocol_tests.cpp "${WFG_ROOT}/common/live_protocol.cpp")
target_include_directories(wfg_live_protocol_tests PRIVATE "${WFG_ROOT}")
target_compile_features(wfg_live_protocol_tests PRIVATE cxx_std_23)
add_test(NAME wfg-live-protocol COMMAND wfg_live_protocol_tests)
add_test(NAME wfg-live-host COMMAND "${Python3_EXECUTABLE}" -m unittest tests.test_live_control -v
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
    WFG_SOURCE_MANIFEST_SHA256="0000000000000000000000000000000000000000000000000000000000000000")
add_test(NAME wfg-tx-usb-owner COMMAND wfg_tx_usb_host_tests)

add_executable(wfg_player_live_host_tests player_live_host_tests.cpp "${WFG_ROOT}/common/live_protocol.cpp")
target_include_directories(wfg_player_live_host_tests BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/rt1170_host_stubs")
target_include_directories(wfg_player_live_host_tests PRIVATE "${WFG_ROOT}" "${WFG_ROOT}/rt1170")
target_link_libraries(wfg_player_live_host_tests PRIVATE wfg_signal_lab)
target_compile_definitions(wfg_player_live_host_tests PRIVATE WFG_PLAYER_IMAGE=1
    WFG_SOURCE_MANIFEST_SHA256="0000000000000000000000000000000000000000000000000000000000000000")
if(NOT MSVC)
    target_compile_options(wfg_player_live_host_tests PRIVATE -ffp-contract=off -fno-fast-math)
endif()
add_test(NAME wfg-player-live-owner COMMAND wfg_player_live_host_tests)

add_executable(wfg_tx_artifact_host_tests tx_artifact_host_tests.cpp
    "${WFG_ROOT}/rt1170/tx_artifact.cpp" "${WFG_ROOT}/rt1170/encoder_catalog.cpp" "${WFG_ROOT}/common/tx_protocol.cpp")
target_include_directories(wfg_tx_artifact_host_tests BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/rt1170_host_stubs")
target_include_directories(wfg_tx_artifact_host_tests PRIVATE "${WFG_ROOT}" "${WFG_ROOT}/rt1170")
target_link_libraries(wfg_tx_artifact_host_tests PRIVATE wfg_native_m110)
target_compile_definitions(wfg_tx_artifact_host_tests PRIVATE
    WFG_SOURCE_MANIFEST_SHA256="0000000000000000000000000000000000000000000000000000000000000000")
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

add_executable(wfg_capture_core_tests capture_core_tests.cpp)
target_include_directories(wfg_capture_core_tests PRIVATE "${WFG_ROOT}")
target_compile_features(wfg_capture_core_tests PRIVATE cxx_std_23)
set_target_properties(wfg_capture_core_tests PROPERTIES ENABLE_EXPORTS OFF)
if(MSVC)
    target_compile_options(wfg_capture_core_tests PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(wfg_capture_core_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME wfg-capture-core COMMAND wfg_capture_core_tests)
add_custom_command(TARGET wfg_capture_core_tests POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E touch
            "${CMAKE_CURRENT_BINARY_DIR}/libwfg_capture_core_tests.dll.a")
add_custom_command(TARGET wfg_contract_tests POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E touch
            "${CMAKE_CURRENT_BINARY_DIR}/libwfg_contract_tests.dll.a")
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
    COMMAND "${Python3_EXECUTABLE}" -m unittest tests.test_signal_lab -v
    WORKING_DIRECTORY "${WFG_ROOT}")
add_test(
    NAME wfg-signal-lab-render
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_signal_lab_render.py"
            --renderer "$<TARGET_FILE:signal_lab_render>")
add_test(
    NAME wfg-source-boundary
    COMMAND "${Python3_EXECUTABLE}" "${WFG_ROOT}/scripts/verify-standalone.py"
            --root "${WFG_ROOT}" --check-provenance)

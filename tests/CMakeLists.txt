find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

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
    NAME wfg-source-boundary
    COMMAND "${Python3_EXECUTABLE}" "${WFG_ROOT}/scripts/verify-standalone.py"
            --root "${WFG_ROOT}" --check-provenance)

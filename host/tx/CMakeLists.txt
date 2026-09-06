add_executable(wfg_m110_tx main.cpp)
target_link_libraries(wfg_m110_tx PRIVATE wfg_native_m110)
target_compile_features(wfg_m110_tx PRIVATE cxx_std_23)
target_compile_definitions(wfg_m110_tx PRIVATE
    WFG_TX_BUILD_ID="${WFG_BUILD_ID}"
    WFG_TX_SOURCE_MANIFEST_SHA256="${WFG_SOURCE_MANIFEST_SHA256}")
if(MSVC)
    target_compile_options(wfg_m110_tx PRIVATE /W4 /WX /permissive- /fp:strict)
else()
    target_compile_options(wfg_m110_tx PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
endif()
set_target_properties(wfg_m110_tx PROPERTIES ENABLE_EXPORTS OFF)
add_custom_command(TARGET wfg_m110_tx POST_BUILD COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_CURRENT_BINARY_DIR}/libwfg_m110_tx.dll.a")
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
add_test(NAME wfg-native-m110-artifacts COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_artifacts.py"
    --tx "$<TARGET_FILE:wfg_m110_tx>" --scratch "${CMAKE_CURRENT_BINARY_DIR}/test-artifacts")

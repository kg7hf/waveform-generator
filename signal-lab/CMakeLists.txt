include("${CMAKE_CURRENT_SOURCE_DIR}/sources.cmake")

add_library(wfg_signal_lab STATIC ${WFG_SIGNAL_LAB_SOURCES})
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
add_custom_command(TARGET wfg_signal_lab_tests POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E touch
            "${CMAKE_CURRENT_BINARY_DIR}/libwfg_signal_lab_tests.dll.a")
add_test(NAME wfg-signal-lab-cpp COMMAND wfg_signal_lab_tests)

add_executable(wfg_live_control_tests tests/live_control_tests.cpp)
target_link_libraries(wfg_live_control_tests PRIVATE wfg_signal_lab)
target_compile_features(wfg_live_control_tests PRIVATE cxx_std_23)
if(NOT MSVC)
    target_compile_options(wfg_live_control_tests PRIVATE -Wall -Wextra -Wpedantic -Werror ${WFG_SIGNAL_LAB_COMPILE_OPTIONS})
endif()
add_test(NAME wfg-live-control-cpp COMMAND wfg_live_control_tests)

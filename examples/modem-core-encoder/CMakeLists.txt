# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

if(WFG_TARGET STREQUAL "host" AND BUILD_TESTING)
    wfg_add_catch2_test(wfg_modem_core_encoder_tests
        SOURCES modem_core_encoder_tests.cpp
        LIBRARIES wfg_encoder_plugin)
    target_include_directories(wfg_modem_core_encoder_tests PRIVATE "${WFG_ROOT}" "${WFG_MODEM_CORE_DIR}")
endif()

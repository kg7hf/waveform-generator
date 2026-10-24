# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

# Supply a local checkout of the public modem-core project. No modem algorithm
# source is copied into waveform-generator and CMake never fetches it.
set(WFG_MODEM_CORE_DIR "" CACHE PATH "Local kg7hf/modem-core checkout")
if(NOT EXISTS "${WFG_MODEM_CORE_DIR}/modem/m110a/transmitter.hpp")
    message(FATAL_ERROR "M110 plugin requires -DWFG_MODEM_CORE_DIR=<modem-core checkout>")
endif()

set(WFG_ENCODER_PLUGIN_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/registry.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/m110_encoder.cpp"
    "${WFG_MODEM_CORE_DIR}/modem/common/status.cpp"
    "${WFG_MODEM_CORE_DIR}/modem/common/types.cpp"
    "${WFG_MODEM_CORE_DIR}/modem/common/dsp/timing_recovery.cpp"
    "${WFG_MODEM_CORE_DIR}/modem/common/fec/convolutional_k7.cpp"
    "${WFG_MODEM_CORE_DIR}/modem/common/waveform/waveform.cpp"
    "${WFG_MODEM_CORE_DIR}/modem/m110a/transmitter.cpp"
    "${WFG_MODEM_CORE_DIR}/modem/m110a/body_waveform.cpp"
    "${WFG_MODEM_CORE_DIR}/modem/m110a/body_audio_stream_frontend.cpp"
    "${WFG_MODEM_CORE_DIR}/modem/m110a/scrambler.cpp"
    "${WFG_MODEM_CORE_DIR}/modem/m110a/synchronization.cpp"
    "${WFG_MODEM_CORE_DIR}/modem/m110a/demapper.cpp"
    "${WFG_MODEM_CORE_DIR}/modem/m110a/interleaver.cpp")
set(WFG_ENCODER_PLUGIN_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}" "${WFG_MODEM_CORE_DIR}")
set(WFG_ENCODER_PLUGIN_STACK_RULES "${CMAKE_CURRENT_LIST_DIR}/stack-usage-rules.json")

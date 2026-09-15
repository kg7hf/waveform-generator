# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Paul R. Decker

# Portable impairment engine sources shared by the host renderer, the host
# tests and the RT1170 player. The firmware target compiles these files with its
# own CPU/FPU flags; the host builds them into the wfg_signal_lab static library.
get_filename_component(WFG_SIGNAL_LAB_ROOT "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
set(WFG_SIGNAL_LAB_INCLUDE "${WFG_SIGNAL_LAB_ROOT}/include")
set(WFG_SIGNAL_LAB_SOURCES
    "${WFG_SIGNAL_LAB_ROOT}/src/det_math.cpp"
    "${WFG_SIGNAL_LAB_ROOT}/src/json.cpp"
    "${WFG_SIGNAL_LAB_ROOT}/src/scenario.cpp"
    "${WFG_SIGNAL_LAB_ROOT}/src/validation.cpp"
    "${WFG_SIGNAL_LAB_ROOT}/src/band_fir.cpp"
    "${WFG_SIGNAL_LAB_ROOT}/src/awgn.cpp"
    "${WFG_SIGNAL_LAB_ROOT}/src/cw.cpp"
    "${WFG_SIGNAL_LAB_ROOT}/src/impulse.cpp"
    "${WFG_SIGNAL_LAB_ROOT}/src/fade.cpp"
    "${WFG_SIGNAL_LAB_ROOT}/src/sample_slip.cpp"
    "${WFG_SIGNAL_LAB_ROOT}/src/mixer.cpp"
    "${WFG_SIGNAL_LAB_ROOT}/src/sample_stream.cpp"
    "${WFG_SIGNAL_LAB_ROOT}/src/live_control.cpp"
)
# Determinism across host and target: no floating-point contraction, no fast-math.
set(WFG_SIGNAL_LAB_COMPILE_OPTIONS -ffp-contract=off -fno-fast-math)

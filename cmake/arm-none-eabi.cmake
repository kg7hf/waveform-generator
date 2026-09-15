# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Paul R. Decker

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR cortex-m7)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(NOT DEFINED ARMGCC_DIR AND DEFINED ENV{ARMGCC_DIR})
    file(TO_CMAKE_PATH "$ENV{ARMGCC_DIR}" ARMGCC_DIR)
endif()

if(NOT ARMGCC_DIR)
    message(FATAL_ERROR
        "ARMGCC_DIR must identify an installed Arm GNU toolchain; the generator does not search a parent checkout")
endif()

file(REAL_PATH "${ARMGCC_DIR}" ARMGCC_DIR EXPAND_TILDE)
# Custom plugin build directories are often built without --preset. Retain the
# explicitly selected toolchain so an automatic CMake regeneration works too.
set(ARMGCC_DIR "${ARMGCC_DIR}" CACHE PATH "Installed Arm GNU toolchain" FORCE)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES ARMGCC_DIR)
set(_wfg_arm_bin "${ARMGCC_DIR}/bin")
if(WIN32)
    set(_wfg_exe ".exe")
else()
    set(_wfg_exe "")
endif()

foreach(tool IN ITEMS gcc g++ objcopy size)
    if(NOT EXISTS "${_wfg_arm_bin}/arm-none-eabi-${tool}${_wfg_exe}")
        message(FATAL_ERROR "Missing Arm GNU tool: ${_wfg_arm_bin}/arm-none-eabi-${tool}${_wfg_exe}")
    endif()
endforeach()

set(CMAKE_C_COMPILER "${_wfg_arm_bin}/arm-none-eabi-gcc${_wfg_exe}" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${_wfg_arm_bin}/arm-none-eabi-g++${_wfg_exe}" CACHE FILEPATH "" FORCE)
set(CMAKE_ASM_COMPILER "${_wfg_arm_bin}/arm-none-eabi-gcc${_wfg_exe}" CACHE FILEPATH "" FORCE)
set(CMAKE_OBJCOPY "${_wfg_arm_bin}/arm-none-eabi-objcopy${_wfg_exe}" CACHE FILEPATH "" FORCE)
set(CMAKE_SIZE "${_wfg_arm_bin}/arm-none-eabi-size${_wfg_exe}" CACHE FILEPATH "" FORCE)

unset(_wfg_arm_bin)
unset(_wfg_exe)

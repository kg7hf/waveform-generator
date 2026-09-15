# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Paul R. Decker

# One MCUX SDK executable is configured per build tree. Separate presets select
# the stopped scaffold or the optional deterministic tone checkpoint.
get_filename_component(WFG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(SdkRootDirPath "${WFG_ROOT}/third_party/mcux-sdk")
set(WFG_CMSIS_ROOT "${WFG_ROOT}/third_party/CMSIS")
set(WFG_FREERTOS_ROOT "${WFG_ROOT}/third_party/FreeRTOS-Kernel")
set(WFG_TINYUSB_ROOT "${WFG_ROOT}/third_party/tinyusb")
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
# Upstream submodules stay clean. Adapted vendor sources exist only in this
# build tree. Git manages upstream revisions; patch applicability is checked.
execute_process(COMMAND "${Python3_EXECUTABLE}" "${WFG_ROOT}/scripts/dependencies.py"
    --root "${WFG_ROOT}" --prepare "${CMAKE_BINARY_DIR}/vendor"
    RESULT_VARIABLE wfg_dependency_result)
if(NOT wfg_dependency_result EQUAL 0)
    message(FATAL_ERROR "Dependency setup failed. Run scripts/init-dependencies.ps1; see diagnostics above.")
endif()
set(WFG_SDMMC_ROOT "${CMAKE_BINARY_DIR}/vendor/sdmmc")
set(WFG_FATFS_ROOT "${CMAKE_BINARY_DIR}/vendor/fatfs")
set(ProjDirPath "${CMAKE_CURRENT_SOURCE_DIR}")

set(WFG_FIRMWARE_IMAGE "scaffold" CACHE STRING "RT1170 image: scaffold, tone, or player")
set_property(CACHE WFG_FIRMWARE_IMAGE PROPERTY STRINGS scaffold tone player sdcard)
option(WFG_SD_POWER_CONTROL_TEST
    "Use the SD-only image for the CMD8 socket-power diagnostic instead of read-only card identification"
    ON)
option(WFG_MSC_READ_ONLY
    "Expose the player microSD LUN as USB mass storage with all writes rejected"
    OFF)
if(NOT WFG_FIRMWARE_IMAGE STREQUAL "scaffold" AND
   NOT WFG_FIRMWARE_IMAGE STREQUAL "tone" AND
   NOT WFG_FIRMWARE_IMAGE STREQUAL "player" AND
   NOT WFG_FIRMWARE_IMAGE STREQUAL "sdcard")
    message(FATAL_ERROR "WFG_FIRMWARE_IMAGE must be scaffold, tone, player, or sdcard")
endif()
if(NOT DEFINED WFG_BUILD_ID)
    set(WFG_BUILD_ID "unidentified-local-build")
endif()

foreach(wfg_required_file IN ITEMS
        "${SdkRootDirPath}/devices/MIMXRT1176/all_lib_device.cmake"
        "${WFG_CMSIS_ROOT}/Core/Include/core_cm7.h"
        "${WFG_FREERTOS_ROOT}/tasks.c"
        "${WFG_TINYUSB_ROOT}/src/tusb.c")
    if(NOT EXISTS "${wfg_required_file}")
        message(FATAL_ERROR "Missing local local dependency: ${wfg_required_file}")
    endif()
endforeach()
if(WFG_FIRMWARE_IMAGE STREQUAL "player" OR WFG_FIRMWARE_IMAGE STREQUAL "sdcard")
    foreach(wfg_player_required_file IN ITEMS
            "${SdkRootDirPath}/drivers/usdhc/fsl_usdhc.c"
            "${SdkRootDirPath}/components/osa/fsl_os_abstraction_free_rtos.c"
            "${WFG_SDMMC_ROOT}/common/fsl_sdmmc_common.c"
            "${WFG_SDMMC_ROOT}/host/usdhc/non_blocking/fsl_sdmmc_host.c"
            "${WFG_SDMMC_ROOT}/osa/fsl_sdmmc_osa.c"
            "${WFG_SDMMC_ROOT}/sd/fsl_sd.c")
        if(NOT EXISTS "${wfg_player_required_file}")
            message(FATAL_ERROR "Missing local SD dependency: ${wfg_player_required_file}")
        endif()
    endforeach()
endif()
if(WFG_FIRMWARE_IMAGE STREQUAL "player" OR
   (WFG_FIRMWARE_IMAGE STREQUAL "sdcard" AND NOT WFG_SD_POWER_CONTROL_TEST))
    foreach(wfg_player_required_file IN ITEMS
            "${WFG_FATFS_ROOT}/source/diskio.c"
            "${WFG_FATFS_ROOT}/source/ff.c"
            "${WFG_FATFS_ROOT}/source/ffunicode.c"
            "${WFG_FATFS_ROOT}/source/fsl_sd_disk/fsl_sd_disk.c")
        if(NOT EXISTS "${wfg_player_required_file}")
            message(FATAL_ERROR "Missing local player dependency: ${wfg_player_required_file}")
        endif()
    endforeach()
endif()

set(MCUXPRESSO_CMAKE_FORMAT_MAJOR_VERSION 2)
set(MCUXPRESSO_CMAKE_FORMAT_MINOR_VERSION 0)
set(MCUX_BUILD_TYPES flexspi_nor_debug flexspi_nor_release)
set(MCUX_SDK_PROJECT_NAME waveform_generator.elf)
include("${WFG_ROOT}/cmake/rt1170_config.cmake")
include("${WFG_ROOT}/cmake/rt1170_flags.cmake")

if(NOT WFG_FIRMWARE_IMAGE STREQUAL "sdcard")
    set(CONFIG_USE_driver_lpi2c true)
    set(CONFIG_USE_driver_wm8960 true)
    set(CONFIG_USE_component_wm8960_adapter true)
    set(CONFIG_USE_driver_codec true)
    set(CONFIG_USE_component_lpi2c_adapter true)
    set(CONFIG_USE_component_codec_i2c true)
    set(CONFIG_USE_driver_sai true)
    set(CONFIG_USE_driver_sai_edma true)
    set(CONFIG_USE_driver_dmamux true)
    set(CONFIG_USE_driver_edma true)
endif()
set(CONFIG_USE_driver_memory true)

set(wfg_local_c_sources
    platform/board_pins.c
)
if(NOT WFG_FIRMWARE_IMAGE STREQUAL "sdcard")
    list(APPEND wfg_local_c_sources
        usb/usb_tinyusb_port.c
        usb/usb_descriptors.c)
endif()
add_executable(${MCUX_SDK_PROJECT_NAME}
    main.cpp
    platform/board.cpp
    platform/runtime.cpp
    os/task.cpp
    "${WFG_ROOT}/common/status.cpp"
    ${wfg_local_c_sources}
    "${WFG_FREERTOS_ROOT}/tasks.c"
    "${WFG_FREERTOS_ROOT}/queue.c"
    "${WFG_FREERTOS_ROOT}/list.c"
    "${WFG_FREERTOS_ROOT}/portable/GCC/ARM_CM7/r0p1/port.c"
    "${SdkRootDirPath}/boards/evkmimxrt1170/board.c"
    "${SdkRootDirPath}/boards/evkmimxrt1170/clock_config.c"
    "${SdkRootDirPath}/boards/evkmimxrt1170/dcd.c"
    "${SdkRootDirPath}/boards/evkmimxrt1170/xip/evkmimxrt1170_flexspi_nor_config.c"
)
if(NOT WFG_FIRMWARE_IMAGE STREQUAL "sdcard")
    target_sources(${MCUX_SDK_PROJECT_NAME} PRIVATE
        platform/wm8960_codec.cpp
        usb_service.cpp
        "${WFG_TINYUSB_ROOT}/src/tusb.c"
        "${WFG_TINYUSB_ROOT}/src/common/tusb_fifo.c"
        "${WFG_TINYUSB_ROOT}/src/device/usbd.c"
        "${WFG_TINYUSB_ROOT}/src/class/cdc/cdc_device.c"
        "${WFG_TINYUSB_ROOT}/src/portable/chipidea/ci_hs/dcd_ci_hs.c"
    )
endif()
if(WFG_FIRMWARE_IMAGE STREQUAL "tone")
    target_sources(${MCUX_SDK_PROJECT_NAME} PRIVATE tone.cpp)
    target_compile_definitions(${MCUX_SDK_PROJECT_NAME} PRIVATE WFG_TONE_IMAGE=1)
elseif(WFG_FIRMWARE_IMAGE STREQUAL "player")
    list(APPEND wfg_local_c_sources
        platform/waveform_sd.c
        platform/waveform_msc.c)
    # The portable impairment engine runs in the player task between the
    # SD read and the ring push. Same sources and float rules as the host renderer.
    include("${WFG_ROOT}/signal-lab/sources.cmake")
    set_source_files_properties(${WFG_SIGNAL_LAB_SOURCES} PROPERTIES
        COMPILE_OPTIONS "${WFG_SIGNAL_LAB_COMPILE_OPTIONS}")
    target_include_directories(${MCUX_SDK_PROJECT_NAME} PRIVATE "${WFG_SIGNAL_LAB_INCLUDE}")
    set_source_files_properties(player.cpp tx_artifact.cpp "${WFG_ROOT}/common/live_protocol.cpp" PROPERTIES
        COMPILE_OPTIONS "-ffp-contract=off;-fno-fast-math")
    target_sources(${MCUX_SDK_PROJECT_NAME} PRIVATE
        player.cpp
        tx_usb.cpp
        "${WFG_ROOT}/common/tx_protocol.cpp"
        "${WFG_ROOT}/common/dd008/cdc_mux_protocol.cpp"
        "${WFG_ROOT}/common/dd008/cdc_mux_session.cpp"
        "${WFG_ROOT}/common/live_protocol.cpp"
        ${WFG_SIGNAL_LAB_SOURCES}
        platform/waveform_sd.c
        platform/waveform_msc.c
        "${SdkRootDirPath}/drivers/usdhc/fsl_usdhc.c"
        "${SdkRootDirPath}/components/osa/fsl_os_abstraction_free_rtos.c"
        "${WFG_SDMMC_ROOT}/common/fsl_sdmmc_common.c"
        "${WFG_SDMMC_ROOT}/host/usdhc/non_blocking/fsl_sdmmc_host.c"
        "${WFG_SDMMC_ROOT}/osa/fsl_sdmmc_osa.c"
        "${WFG_SDMMC_ROOT}/sd/fsl_sd.c"
        "${WFG_FATFS_ROOT}/source/diskio.c"
        "${WFG_FATFS_ROOT}/source/ff.c"
        "${WFG_FATFS_ROOT}/source/ffunicode.c"
        "${WFG_FATFS_ROOT}/source/fsl_sd_disk/fsl_sd_disk.c"
        "${WFG_TINYUSB_ROOT}/src/class/msc/msc_device.c"
    )
    if(WFG_MSC_READ_ONLY OR NOT WFG_ENABLE_ENCODER_PLUGINS)
        target_sources(${MCUX_SDK_PROJECT_NAME} PRIVATE tx_artifact_unavailable.cpp
            "${WFG_ROOT}/waveform-source/encoder_registry.cpp")
    else()
        wfg_attach_encoder_plugin(${MCUX_SDK_PROJECT_NAME})
        target_sources(${MCUX_SDK_PROJECT_NAME} PRIVATE tx_artifact.cpp
            "${WFG_ROOT}/waveform-source/wav_generator.cpp")
        set_source_files_properties(${WFG_ENCODER_PLUGIN_SOURCES}
            "${WFG_ROOT}/waveform-source/wav_generator.cpp" PROPERTIES
            COMPILE_OPTIONS "-ffp-contract=off;-fno-fast-math")
    endif()
    target_compile_definitions(${MCUX_SDK_PROJECT_NAME} PRIVATE
        WFG_PLAYER_IMAGE=1
        WFG_SD_USE_BOOTCLOCKRUN_USDHC1=1
        WFG_SD_DISABLE_HOST_VOLTAGE_CONTROL=1
        WFG_SD_ALWAYS_PRESENT_CARD_DETECT=1
        WFG_SD_IDENTIFICATION_CLOCK_HZ=100000U
        SDK_OS_FREE_RTOS=1
        SD_ENABLED=1
    )
    if(WFG_MSC_READ_ONLY)
        target_compile_definitions(${MCUX_SDK_PROJECT_NAME} PRIVATE
            WFG_MSC_READ_ONLY=1)
    endif()
elseif(WFG_FIRMWARE_IMAGE STREQUAL "sdcard")
    list(APPEND wfg_local_c_sources
        platform/waveform_sd.c)
    target_sources(${MCUX_SDK_PROJECT_NAME} PRIVATE
        sdcard_probe.cpp
        platform/waveform_sd.c
        "${SdkRootDirPath}/drivers/usdhc/fsl_usdhc.c"
        "${SdkRootDirPath}/components/osa/fsl_os_abstraction_free_rtos.c"
        "${WFG_SDMMC_ROOT}/common/fsl_sdmmc_common.c"
        "${WFG_SDMMC_ROOT}/host/usdhc/non_blocking/fsl_sdmmc_host.c"
        "${WFG_SDMMC_ROOT}/osa/fsl_sdmmc_osa.c"
        "${WFG_SDMMC_ROOT}/sd/fsl_sd.c"
    )
    target_compile_definitions(${MCUX_SDK_PROJECT_NAME} PRIVATE
        WFG_SDCARD_IMAGE=1
        WFG_SD_USE_BOOTCLOCKRUN_USDHC1=1
        WFG_SD_DISABLE_HOST_VOLTAGE_CONTROL=1
        WFG_SD_ALWAYS_PRESENT_CARD_DETECT=1
        WFG_SD_IDENTIFICATION_CLOCK_HZ=100000U
        SDK_OS_FREE_RTOS=1
        SD_ENABLED=1
    )
    if(WFG_SD_POWER_CONTROL_TEST)
        target_compile_definitions(${MCUX_SDK_PROJECT_NAME} PRIVATE
            WFG_SD_POWER_CONTROL_TEST=1)
    else()
        target_compile_definitions(${MCUX_SDK_PROJECT_NAME} PRIVATE
            WFG_SD_READ_ONLY_IDENTIFY_TEST=1)
        target_sources(${MCUX_SDK_PROJECT_NAME} PRIVATE
            "${WFG_FATFS_ROOT}/source/diskio.c"
            "${WFG_FATFS_ROOT}/source/ff.c"
            "${WFG_FATFS_ROOT}/source/ffunicode.c"
            "${WFG_FATFS_ROOT}/source/fsl_sd_disk/fsl_sd_disk.c"
        )
    endif()
endif()

set_target_properties(${MCUX_SDK_PROJECT_NAME} PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    SUFFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
target_compile_definitions(${MCUX_SDK_PROJECT_NAME} PRIVATE "WFG_BUILD_ID=\"${WFG_BUILD_ID}\"")
set_source_files_properties(${wfg_local_c_sources} PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wpedantic;-Werror")
target_include_directories(${MCUX_SDK_PROJECT_NAME} PRIVATE
    "${WFG_ROOT}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/platform"
    "${CMAKE_CURRENT_SOURCE_DIR}/os"
    "${CMAKE_CURRENT_SOURCE_DIR}/usb"
)
target_include_directories(${MCUX_SDK_PROJECT_NAME} SYSTEM PRIVATE
    "${WFG_CMSIS_ROOT}/Core/Include"
    "${WFG_FREERTOS_ROOT}/include"
    "${WFG_FREERTOS_ROOT}/portable/GCC/ARM_CM7/r0p1"
    "${SdkRootDirPath}/boards/evkmimxrt1170"
    "${SdkRootDirPath}/boards/evkmimxrt1170/xip"
)
if(NOT WFG_FIRMWARE_IMAGE STREQUAL "sdcard")
    target_include_directories(${MCUX_SDK_PROJECT_NAME} SYSTEM PRIVATE
        "${WFG_TINYUSB_ROOT}/src"
    )
endif()
if(WFG_FIRMWARE_IMAGE STREQUAL "player" OR WFG_FIRMWARE_IMAGE STREQUAL "sdcard")
    target_include_directories(${MCUX_SDK_PROJECT_NAME} SYSTEM PRIVATE
        "${SdkRootDirPath}/drivers/usdhc"
        "${SdkRootDirPath}/components/osa"
        "${WFG_SDMMC_ROOT}/common"
        "${WFG_SDMMC_ROOT}/host/usdhc"
        "${WFG_SDMMC_ROOT}/osa"
        "${WFG_SDMMC_ROOT}/sd"
    )
endif()
if(WFG_FIRMWARE_IMAGE STREQUAL "player" OR
   (WFG_FIRMWARE_IMAGE STREQUAL "sdcard" AND NOT WFG_SD_POWER_CONTROL_TEST))
    target_include_directories(${MCUX_SDK_PROJECT_NAME} SYSTEM PRIVATE
        "${WFG_FATFS_ROOT}/source"
        "${WFG_FATFS_ROOT}/source/fsl_sd_disk"
    )
endif()
wfg_configure_rt1170_target(${MCUX_SDK_PROJECT_NAME})

# Retain the vendor component graph locally. Its source lists are part of the
# pinned dependency and are audited during fresh-clone acceptance.
include("${SdkRootDirPath}/devices/MIMXRT1176/all_lib_device.cmake")
get_target_property(wfg_all_includes ${MCUX_SDK_PROJECT_NAME} INCLUDE_DIRECTORIES)
foreach(wfg_include IN LISTS wfg_all_includes)
    cmake_path(IS_PREFIX WFG_ROOT "${wfg_include}" NORMALIZE wfg_inside_project)
    if(wfg_inside_project AND wfg_include MATCHES "/third_party/")
        target_include_directories(${MCUX_SDK_PROJECT_NAME} SYSTEM PRIVATE "${wfg_include}")
    endif()
endforeach()
target_link_libraries(${MCUX_SDK_PROJECT_NAME} PRIVATE
    -Wl,--start-group -lm -lc -lgcc -lnosys -Wl,--end-group)

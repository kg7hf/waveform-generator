# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

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
# Off until a bench shows Windows' usbaudio2 accepts a one-channel alternate
# beside the stereo ones on the stereo capture terminal
# (docs/modules/protocols-media-and-artifacts.md).
option(WFG_UAC_MONO_ALT
    "Offer UAC2 capture alternate 3: 16-bit mono (the left channel), besides 24- and 16-bit stereo"
    OFF)
# Off: the player clocks the SD controller from SYS_PLL2_PFD2 / 2 = 198 MHz and
# the card's High Speed bus runs at 49.5 MHz. On: from BOARD_BootClockRUN's
# 24 MHz USDHC1 root, as every earlier player image did, for a card or socket
# that fails at 49.5 MHz (docs/modules/protocols-media-and-artifacts.md, "MSC
# transfers and throughput"). The SD-only image always uses the 24 MHz root.
option(WFG_SD_CLOCK_24MHZ
    "Run the player's SD bus at 24 MHz from BOARD_BootClockRUN's USDHC1 root instead of 49.5 MHz from SYS_PLL2"
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

if(WFG_TARGET STREQUAL "rt1064")
    set(WFG_DEVICE MIMXRT1064)
    set(WFG_SDK_BOARD evkmimxrt1064)
    set(WFG_BOARD_PINS "${WFG_ROOT}/rt1064/board_pins.c")
    if(WFG_SD_CLOCK_24MHZ)
        message(FATAL_ERROR "WFG_SD_CLOCK_24MHZ is an RT1170-only clock option")
    endif()
else()
    set(WFG_DEVICE MIMXRT1176)
    set(WFG_SDK_BOARD evkmimxrt1170)
    set(WFG_BOARD_PINS platform/board_pins.c)
endif()

foreach(wfg_required_file IN ITEMS
        "${SdkRootDirPath}/devices/${WFG_DEVICE}/all_lib_device.cmake"
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
include("${WFG_ROOT}/cmake/${WFG_TARGET}_config.cmake")
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
    ${WFG_BOARD_PINS}
)
if(NOT WFG_FIRMWARE_IMAGE STREQUAL "sdcard")
    list(APPEND wfg_local_c_sources
        usb/usb_tinyusb_port.c
        usb/usb_descriptors.c)
endif()
add_executable(${MCUX_SDK_PROJECT_NAME}
    main.cpp
    critical_error.cpp
    platform/board.cpp
    platform/runtime.cpp
    os/task.cpp
    "${WFG_ROOT}/common/status.cpp"
    # Compile the portable retained-waveform writer with the actual ARM
    # policy too. No board command uses it yet; unused code is discarded.
    "${WFG_ROOT}/common/pcm24_artifact.cpp"
    ${wfg_local_c_sources}
    "${WFG_FREERTOS_ROOT}/tasks.c"
    "${WFG_FREERTOS_ROOT}/queue.c"
    "${WFG_FREERTOS_ROOT}/list.c"
    "${WFG_FREERTOS_ROOT}/portable/GCC/ARM_CM7/r0p1/port.c"
    "${SdkRootDirPath}/boards/${WFG_SDK_BOARD}/board.c"
    "${SdkRootDirPath}/boards/${WFG_SDK_BOARD}/clock_config.c"
    "${SdkRootDirPath}/boards/${WFG_SDK_BOARD}/dcd.c"
    "${SdkRootDirPath}/boards/${WFG_SDK_BOARD}/xip/${WFG_SDK_BOARD}_flexspi_nor_config.c"
)
if(WFG_TARGET STREQUAL "rt1064")
    target_sources(${MCUX_SDK_PROJECT_NAME} PRIVATE
        "${SdkRootDirPath}/devices/MIMXRT1064/gcc/startup_MIMXRT1064.S")
endif()
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
    # Every player image, read-only included, is one composite USB device: CDC,
    # MSC and the UAC2 capture function (usb_audio.cpp and its TinyUSB glue).
    list(APPEND wfg_local_c_sources
        platform/waveform_sd.c
        platform/waveform_msc.c
        usb/usb_audio_tinyusb.c
        usb/usb_ep0_order_glue.c)
    # The portable impairment engine runs in the player task between the
    # SD read and the ring push. Same sources and float rules as the host renderer.
    include("${WFG_ROOT}/signal-lab/sources.cmake")
    set_source_files_properties(${WFG_SIGNAL_LAB_SOURCES} PROPERTIES
        COMPILE_OPTIONS "${WFG_SIGNAL_LAB_COMPILE_OPTIONS}")
    target_include_directories(${MCUX_SDK_PROJECT_NAME} PRIVATE "${WFG_SIGNAL_LAB_INCLUDE}")
    set_source_files_properties(player.cpp prepare_job.cpp tx_artifact.cpp "${WFG_ROOT}/common/live_protocol.cpp" PROPERTIES
        COMPILE_OPTIONS "-ffp-contract=off;-fno-fast-math")
    target_sources(${MCUX_SDK_PROJECT_NAME} PRIVATE
        player.cpp
        card_listing.cpp
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
        usb_audio.cpp
        usb/usb_audio_tinyusb.c
        usb_ep0_order.cpp
        usb/usb_ep0_order_glue.c
        "${WFG_TINYUSB_ROOT}/src/class/audio/audio_device.c"
    )
    # The ChipIdea driver's events reach TinyUSB through the USB interrupt's
    # EP0 order first (usb/usb_tinyusb_port.c, rt1170/usb_ep0_order.hpp, #32):
    # its calls to dcd_event_handler go to __wrap_dcd_event_handler, and
    # __real_dcd_event_handler is TinyUSB's. TinyUSB's sources are unchanged.
    # The MSC driver's calls from the USB task go there too, so the stack
    # checks below follow the same list of wrapped symbols.
    set(wfg_wrapped_symbols dcd_event_handler)
    foreach(symbol IN LISTS wfg_wrapped_symbols)
        target_link_options(${MCUX_SDK_PROJECT_NAME} PRIVATE "LINKER:--wrap=${symbol}")
    endforeach()
    # TinyUSB's MSC endpoint buffer (the 16 KB chunk) lives in the
    # non-cacheable OCRAM window: usb/tusb_config.h sets CFG_TUD_MEM_SECTION
    # for this source alone, so TinyUSB's other buffers stay in DTCM.
    set_property(SOURCE "${WFG_TINYUSB_ROOT}/src/class/msc/msc_device.c"
        APPEND PROPERTY COMPILE_DEFINITIONS WFG_TUSB_MSC_DEVICE_UNIT=1)
    # After every link: both MSC chunk buffers lie in that window, and the
    # symbols BOARD_ConfigMPU makes MPU region 10 from describe it exactly.
    add_custom_command(TARGET ${MCUX_SDK_PROJECT_NAME} POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" "${WFG_ROOT}/scripts/check-memory-placement.py"
            --map "${CMAKE_CURRENT_BINARY_DIR}/waveform_generator.map"
            --region m_ncache_ocram
            --window-symbols __NCACHE_REGION_START __NCACHE_REGION_SIZE
            --section .ncache_ocram.wfg_msc_bounce
            --section .ncache_ocram.tusb_msc_epbuf
        VERBATIM)
    # And the CUT injector's state, the player's command mailbox that carries
    # a whole schedule and its reply mailbox, the request line it arrives on,
    # the PLAYLIST state (its 256-item table and its ring of result blocks),
    # the play's watch (its deadline misses and a looped item's assertion
    # count) (player.cpp, usb_service.cpp), the USB interrupt's EP0 order
    # (usb_ep0_order.cpp), the AWGN stage's noise-band kernel
    # (signal-lab/src/awgn.cpp, placed there under WFG_PLAYER_IMAGE) and the
    # critical-error record (critical_error.cpp, in every image) lie in
    # OCRAM2 (m_data2), out of DTCM. A section left out of the image (its
    # attribute lost, or the gate not defined) fails the check too.
    add_custom_command(TARGET ${MCUX_SDK_PROJECT_NAME} POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" "${WFG_ROOT}/scripts/check-memory-placement.py"
            --map "${CMAKE_CURRENT_BINARY_DIR}/waveform_generator.map"
            --region m_data2
            --section .ocram.player_cuts
            --section .ocram.player_engine
            --section .ocram.player_command
            --section .ocram.player_reply
            --section .ocram.control_line
            --section .ocram.playlist
            --section .ocram.player_watch
            --section .ocram.usb_ep0_order
            --section .ocram.noise_band_kernel
            --section .ocram.critical_error
        VERBATIM)
    # Keep the engine's stage workspaces in OCRAM2 and bounded. The 64 KiB
    # ceiling covers the current engine while catching future workspace growth.
    add_custom_command(TARGET ${MCUX_SDK_PROJECT_NAME} POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" "${WFG_ROOT}/scripts/check-memory-placement.py"
            --map "${CMAKE_CURRENT_BINARY_DIR}/waveform_generator.map"
            --region m_data2
            --section .ocram.player_engine
            --max-section-size .ocram.player_engine 65536
        VERBATIM)
    # A separate bounded PCM24 source file and packed read buffer, never a
    # whole WAV and never placed on the player task stack.
    add_custom_command(TARGET ${MCUX_SDK_PROJECT_NAME} POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" "${WFG_ROOT}/scripts/check-memory-placement.py"
            --map "${CMAKE_CURRENT_BINARY_DIR}/waveform_generator.map"
            --region m_data2
            --section .ocram.secondary_source
            --max-section-size .ocram.secondary_source 8192
        VERBATIM)
    # The preparation owner borrows the existing engine and float workspaces.
    # Keep its file/hash state and single packed block bounded in OCRAM2.
    add_custom_command(TARGET ${MCUX_SDK_PROJECT_NAME} POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" "${WFG_ROOT}/scripts/check-memory-placement.py"
            --map "${CMAKE_CURRENT_BINARY_DIR}/waveform_generator.map"
            --region m_data2
            --section .ocram.prepare_job
            --section .ocram.prepare_packed
            --max-section-size .ocram.prepare_job 8192
            --max-section-size .ocram.prepare_packed 6912
        VERBATIM)
    # Read-only listing pages stay fixed-size and off the player task stack.
    add_custom_command(TARGET ${MCUX_SDK_PROJECT_NAME} POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" "${WFG_ROOT}/scripts/check-memory-placement.py"
            --map "${CMAKE_CURRENT_BINARY_DIR}/waveform_generator.map"
            --region m_data2
            --section .ocram.card_listing
            --max-section-size .ocram.card_listing 2048
        VERBATIM)
    # Four shared selectable-band half-kernels; each is designed before use.
    add_custom_command(TARGET ${MCUX_SDK_PROJECT_NAME} POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" "${WFG_ROOT}/scripts/check-memory-placement.py"
            --map "${CMAKE_CURRENT_BINARY_DIR}/waveform_generator.map"
            --region m_data2
            --section .ocram.noise_band_kernel
            --max-section-size .ocram.noise_band_kernel 5120
        VERBATIM)
    # WattersonChannel must fit in the bounded portion of OCRAM2 reserved for
    # the channel state. The linker script places .ocram.* in its NOLOAD .ocram
    # output section; this check requires the named input section in m_data2
    # and caps its host-side size at 20 KiB.
    add_custom_command(TARGET ${MCUX_SDK_PROJECT_NAME} POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" "${WFG_ROOT}/scripts/check-memory-placement.py"
            --map "${CMAKE_CURRENT_BINARY_DIR}/waveform_generator.map"
            --region m_data2
            --section .ocram.watterson
            --max-section-size .ocram.watterson 20480
        VERBATIM)
    # Every C and C++ source writes its frames and calls (-fstack-usage,
    # -fcallgraph-info=su: a .su and a .ci file beside its object). After
    # every link of a release image (the image the bench runs) the player
    # task's deepest chain of calls from player_task_entry, with the reserve
    # for an exception and a context switch, must fit in player_task_stack,
    # and the USB task's from usb_task_entry in usb_task_stack (TinyUSB, its
    # class drivers' callbacks, the control line and the DD008 session), and
    # owned code must not recurse; the virtual and function-pointer calls on
    # the way are resolved by scripts/stack-usage-rules.json plus a narrowly
    # selected image-specific supplemental file (for the USB
    # task, the callbacks of the class drivers tusb_config.h builds), and a
    # call to a wrapped symbol goes to its wrapper, as the link sends it. A debug
    # image (-Og, DEBUG) is not checked: it has other frames, and its debug
    # console and SD driver make indirect calls the rules do not cover.
    target_compile_options(${MCUX_SDK_PROJECT_NAME} PRIVATE
        "$<$<COMPILE_LANGUAGE:C,CXX>:-fstack-usage;-fcallgraph-info=su>")
    if(CMAKE_BUILD_TYPE STREQUAL "flexspi_nor_release")
        set(wfg_stack_rule_args)
        if(NOT WFG_MSC_READ_ONLY AND WFG_ENABLE_ENCODER_PLUGINS)
            file(REAL_PATH "${WFG_ENCODER_PLUGIN_DIR}" wfg_encoder_plugin_real)
            file(REAL_PATH "${WFG_ROOT}/examples/encoder-plugin" wfg_public_tone_plugin_real)
            if("${wfg_encoder_plugin_real}" STREQUAL "${wfg_public_tone_plugin_real}")
                list(APPEND wfg_stack_rule_args --extra-rules
                    "${WFG_ROOT}/examples/encoder-plugin/stack-usage-rules.json")
            endif()
            if(WFG_ENCODER_PLUGIN_STACK_RULES)
                foreach(rule_file IN LISTS WFG_ENCODER_PLUGIN_STACK_RULES)
                    if(NOT EXISTS "${rule_file}")
                        message(FATAL_ERROR "Encoder plugin stack rules do not exist: ${rule_file}")
                    endif()
                    list(APPEND wfg_stack_rule_args --extra-rules "${rule_file}")
                endforeach()
            endif()
        endif()
        add_custom_command(TARGET ${MCUX_SDK_PROJECT_NAME} POST_BUILD
            COMMAND "${Python3_EXECUTABLE}" "${WFG_ROOT}/scripts/check-stack-usage.py"
                --map "${CMAKE_CURRENT_BINARY_DIR}/waveform_generator.map"
                --entry _ZN18waveform_generator12_GLOBAL__N_117player_task_entryEPv
                --stack-section .bss._ZN18waveform_generator12_GLOBAL__N_117player_task_stackE
                --wrap ${wfg_wrapped_symbols}
                ${wfg_stack_rule_args}
                --objects "$<TARGET_OBJECTS:${MCUX_SDK_PROJECT_NAME}>"
            COMMAND "${Python3_EXECUTABLE}" "${WFG_ROOT}/scripts/check-stack-usage.py"
                --map "${CMAKE_CURRENT_BINARY_DIR}/waveform_generator.map"
                --entry _ZN18waveform_generator14usb_task_entryEPv
                --stack-section .bss._ZN18waveform_generator14usb_task_stackE
                --wrap ${wfg_wrapped_symbols}
                ${wfg_stack_rule_args}
                --objects "$<TARGET_OBJECTS:${MCUX_SDK_PROJECT_NAME}>"
            COMMAND_EXPAND_LISTS
            VERBATIM)
    endif()
    target_sources(${MCUX_SDK_PROJECT_NAME} PRIVATE prepared_playback.cpp "${WFG_ROOT}/common/finite_playback.cpp")
    if(WFG_MSC_READ_ONLY)
        target_sources(${MCUX_SDK_PROJECT_NAME} PRIVATE prepare_job_unavailable.cpp prepared_stream_unavailable.cpp)
    else()
        target_sources(${MCUX_SDK_PROJECT_NAME} PRIVATE prepare_job.cpp prepared_stream.cpp)
    endif()
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
    # The identification clock request is waveform_sd_clock.h's (checked by
    # waveform_sd.c): 103125 Hz from the 198 MHz root, 100 kHz from the 24 MHz
    # one, within the SD specification's 100 to 400 kHz either way.
    target_compile_definitions(${MCUX_SDK_PROJECT_NAME} PRIVATE
        WFG_PLAYER_IMAGE=1
        WFG_SD_DISABLE_HOST_VOLTAGE_CONTROL=1
        WFG_SD_ALWAYS_PRESENT_CARD_DETECT=1
        WFG_SD_IDENTIFICATION_CLOCK_HZ=103125U
        SDK_OS_FREE_RTOS=1
        SD_ENABLED=1
    )
    if(WFG_SD_CLOCK_24MHZ)
        target_compile_definitions(${MCUX_SDK_PROJECT_NAME} PRIVATE
            WFG_SD_USE_BOOTCLOCKRUN_USDHC1=1)
    endif()
    if(WFG_MSC_READ_ONLY)
        target_compile_definitions(${MCUX_SDK_PROJECT_NAME} PRIVATE
            WFG_MSC_READ_ONLY=1)
    endif()
    if(WFG_UAC_MONO_ALT)
        target_compile_definitions(${MCUX_SDK_PROJECT_NAME} PRIVATE
            WFG_UAC_MONO_ALT=1)
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

# TinyUSB's TU_ASSERT and TU_BREAKPOINT() call the breakpoint hook
# (usb/tusb_config.h), which records the address it returns to, inside the
# asserting function, for USB? (usb_audio.cpp). Each call must be its own
# assert's:
# - In a function that returns nothing the hook call is a tail call, which -O3
#   turns into a jump: the hook would then return to, and record, that
#   function's caller. -fno-optimize-sibling-calls keeps it a call.
# - Every failure path is the same call, so -O3 merges them: tail merging
#   (GIMPLE) and cross-jumping (RTL) let one call serve the asserts of
#   several lines, and of several inlined functions, and addr2line then names
#   only one of them. -fno-tree-tail-merge and -fno-crossjumping keep one call
#   per assert: in the release player image, 97 hook calls instead of 46, for
#   about 1 KB more code.
# The TinyUSB sources are otherwise built as before.
get_target_property(wfg_target_sources ${MCUX_SDK_PROJECT_NAME} SOURCES)
foreach(wfg_source IN LISTS wfg_target_sources)
    cmake_path(IS_PREFIX WFG_TINYUSB_ROOT "${wfg_source}" NORMALIZE wfg_tinyusb_source)
    if(wfg_tinyusb_source)
        set_property(SOURCE "${wfg_source}" APPEND PROPERTY COMPILE_OPTIONS
            -fno-optimize-sibling-calls -fno-tree-tail-merge -fno-crossjumping)
    endif()
endforeach()

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
wfg_use_build_info(${MCUX_SDK_PROJECT_NAME})
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
    "${SdkRootDirPath}/boards/${WFG_SDK_BOARD}"
    "${SdkRootDirPath}/boards/${WFG_SDK_BOARD}/xip"
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
include("${SdkRootDirPath}/devices/${WFG_DEVICE}/all_lib_device.cmake")
get_target_property(wfg_all_includes ${MCUX_SDK_PROJECT_NAME} INCLUDE_DIRECTORIES)
foreach(wfg_include IN LISTS wfg_all_includes)
    cmake_path(IS_PREFIX WFG_ROOT "${wfg_include}" NORMALIZE wfg_inside_project)
    if(wfg_inside_project AND wfg_include MATCHES "/third_party/")
        target_include_directories(${MCUX_SDK_PROJECT_NAME} SYSTEM PRIVATE "${wfg_include}")
    endif()
endforeach()
target_link_libraries(${MCUX_SDK_PROJECT_NAME} PRIVATE
    -Wl,--start-group -lm -lc -lgcc -lnosys -Wl,--end-group)

# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

# Generator-local adaptation of the original-EVK FlexSPI build settings.

function(wfg_configure_rt1170_target target_name)
    # Generator-local copy of the vendor script plus a NOLOAD .ocram section in
    # m_data2 for the post-impairment output FIFO.
    if(WFG_TARGET STREQUAL "rt1064")
        set(wfg_linker_script "${WFG_ROOT}/cmake/MIMXRT1064xxxxx_flexspi_nor_wfg.ld")
        set(wfg_cpu CPU_MIMXRT1064DVL6A)
    else()
        set(wfg_linker_script "${WFG_ROOT}/cmake/MIMXRT1176xxxxx_cm7_flexspi_nor_wfg.ld")
        set(wfg_cpu CPU_MIMXRT1176DVMAA_cm7)
    endif()
    if(NOT EXISTS "${wfg_linker_script}")
        message(FATAL_ERROR "Missing generator-local linker script ${wfg_linker_script}")
    endif()
    # Main (MSP) stack: startup code, then every interrupt once FreeRTOS runs.
    # The USB ISR completes UAC2 packets and refills the IN FIFO, so the linker
    # script's 1 KB default (__stack_size__) is raised to 4 KB in DTCM.
    set(wfg_isr_stack_bytes 0x1000)

    target_compile_options(${target_name} PRIVATE
        -mcpu=cortex-m7
        -mthumb
        -mfloat-abi=hard
        -mfpu=fpv5-d16
        $<$<COMPILE_LANGUAGE:C,CXX>:-ffunction-sections>
        $<$<COMPILE_LANGUAGE:C,CXX>:-fdata-sections>
        $<$<COMPILE_LANGUAGE:C,CXX>:-fno-common>
        $<$<COMPILE_LANGUAGE:C>:-fno-builtin>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>
        $<$<COMPILE_LANGUAGE:CXX>:-Wall>
        $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
        $<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>
        $<$<COMPILE_LANGUAGE:CXX>:-Werror>
        $<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<CONFIG:Debug,flexspi_nor_debug>>:-Og>
        $<$<CONFIG:Debug,flexspi_nor_debug>:-g3>
        $<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<CONFIG:Release,flexspi_nor_release>>:-O3>
        # Line tables in the release ELF too, so a code address the image
        # reports (USB? usb_first_assert_pc, usb_last_assert_pc) resolves to its
        # source line with arm-none-eabi-addr2line. Debug sections are never
        # loaded onto the board, and -g does not change the generated code.
        $<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<CONFIG:Release,flexspi_nor_release>>:-g>
    )
    target_compile_definitions(${target_name} PRIVATE
        ${wfg_cpu}
        MCUXPRESSO_SDK
        XIP_EXTERNAL_FLASH=1
        XIP_BOOT_HEADER_ENABLE=1
        SDK_DEBUGCONSOLE=1
        CODEC_WM8960_ENABLE=1
        CFG_TUSB_MCU=OPT_MCU_MIMXRT1XXX
        M110_USB_HIGH_SPEED=1
        $<$<CONFIG:Debug,flexspi_nor_debug>:DEBUG>
        $<$<CONFIG:Release,flexspi_nor_release>:NDEBUG>
        $<$<COMPILE_LANGUAGE:ASM>:__STARTUP_CLEAR_BSS>
        $<$<COMPILE_LANGUAGE:ASM>:__STARTUP_INITIALIZE_RAMFUNCTION>
        $<$<COMPILE_LANGUAGE:ASM>:__STARTUP_INITIALIZE_NONCACHEDATA>
    )
    target_link_options(${target_name} PRIVATE
        -mcpu=cortex-m7
        -mthumb
        -mfloat-abi=hard
        -mfpu=fpv5-d16
        --specs=nano.specs
        --specs=nosys.specs
        # WFG-LIVE/1 publishes the effective reference RMS with enough digits
        # for deterministic replay. newlib-nano omits floating-point printf
        # unless this archive member is requested explicitly.
        -Wl,-u,_printf_float
        -static
        -Wl,--gc-sections
        "-Wl,--defsym=__stack_size__=${wfg_isr_stack_bytes}"
        "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/waveform_generator.map"
        -Wl,--print-memory-usage
        "-T${wfg_linker_script}"
    )
    set_property(TARGET ${target_name} APPEND PROPERTY LINK_DEPENDS "${wfg_linker_script}")
endfunction()

# Generator-local adaptation of the original-EVK FlexSPI build settings.
# P1.1 uses the vendor linker unchanged. No modem OCRAM or SDRAM allocation is
# implied by this scaffold; the dedicated SDRAM placement belongs to P1.3.

function(wfg_configure_rt1170_target target_name)
    set(wfg_linker_script
        "${SdkRootDirPath}/devices/MIMXRT1176/gcc/MIMXRT1176xxxxx_cm7_flexspi_nor.ld")
    if(NOT EXISTS "${wfg_linker_script}")
        message(FATAL_ERROR "Missing local RT1170 linker script; materialize locked dependencies first")
    endif()

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
    )
    target_compile_definitions(${target_name} PRIVATE
        CPU_MIMXRT1176DVMAA_cm7
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
        -static
        -Wl,--gc-sections
        "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/waveform_generator.map"
        -Wl,--print-memory-usage
        "-T${wfg_linker_script}"
    )
    set_property(TARGET ${target_name} APPEND PROPERTY LINK_DEPENDS "${wfg_linker_script}")
endfunction()

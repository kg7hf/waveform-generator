// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The SD controller's (USDHC1) clock in the player images: the root clock that
// feeds it, and the SD bus clock the SDK derives from that root.
// Guide: docs/modules/protocols-media-and-artifacts.md (MSC transfers and
// throughput).
// Ownership: constants only. waveform_sd.c selects the root from them; the
// host test wfg-sd-clock (tests/sd_clock_tests.cpp) checks them against the
// SDK's divider rule. The clock tree itself is BOARD_BootClockRUN's.

#pragma once

#include <assert.h>

#ifdef __cplusplus
extern "C"
{
#endif

enum
{
    // BOARD_BootClockRUN (the SDK's evkmimxrt1170 clock_config.c) runs
    // SYS_PLL2 at 528 MHz and sets its PFD2 fraction to 24. A PFD's output is
    // its PLL's times 18 over the fraction: 396 MHz.
    wfg_sd_sys_pll2_hz = 528000000,
    wfg_sd_pfd_numerator = 18,
    wfg_sd_sys_pll2_pfd2_fraction = 24,
    wfg_sd_sys_pll2_pfd2_hz = 396000000,
    // The player's USDHC1 root by default: SYS_PLL2_PFD2 divided by 2, the
    // root NXP's own SD board configuration uses on this EVK.
    wfg_sd_usdhc1_pll2_root_divider = 2,
    wfg_sd_usdhc1_pll2_root_hz = 198000000,
    // BOARD_BootClockRUN's USDHC1 root, OSC_RC_48M_DIV2: the fallback that
    // WFG_SD_CLOCK_24MHZ selects (WFG_SD_USE_BOOTCLOCKRUN_USDHC1), and the
    // root of every player image before the SYS_PLL2 one.
    wfg_sd_usdhc1_boot_root_hz = 24000000,
    // High Speed (SDR25), which SD_CardInit selects at 3.3 V: the SDK asks for
    // 50 MHz and USDHC_SetSdClock divides the root by the smallest total
    // divider whose result does not exceed that.
    wfg_sd_high_speed_max_hz = 50000000,
    wfg_sd_pll2_bus_divider = 4,
    wfg_sd_pll2_bus_hz = 49500000,
    wfg_sd_boot_bus_hz = wfg_sd_usdhc1_boot_root_hz,
    // The High Speed bus clock of this build's root: 24 MHz where
    // WFG_SD_USE_BOOTCLOCKRUN_USDHC1 keeps the 24 MHz root (a
    // WFG_SD_CLOCK_24MHZ player, and the SD-only image), else 49.5 MHz. The
    // build's reported identity (PID and commit) is the same either way,
    // so a player's INFO lists sd-clock-24mhz exactly on the 24 MHz build.
#if defined(WFG_SD_USE_BOOTCLOCKRUN_USDHC1)
    wfg_sd_image_bus_hz = wfg_sd_boot_bus_hz,
#else
    wfg_sd_image_bus_hz = wfg_sd_pll2_bus_hz,
#endif
    // Card identification runs the clock at 100 to 400 kHz (the SD
    // specification's range for a running clock). The player asks for
    // 103125 Hz (WFG_SD_IDENTIFICATION_CLOCK_HZ, rt1170/CMakeLists.txt): the
    // 198 MHz root divided by 1920 (prescaler 128, divisor 15), and from the
    // 24 MHz root the SDK's divider 240 gives 100 kHz, the clock every earlier
    // player image identified the card at. A 100 kHz request would give
    // 96.7 kHz from the 198 MHz root: the SDK's prescaler is a power of two.
    wfg_sd_identification_request_hz = 103125,
    wfg_sd_identification_pll2_divider = 1920,
    wfg_sd_identification_min_hz = 100000,
    wfg_sd_identification_max_hz = 400000,
};

static_assert(wfg_sd_sys_pll2_hz / wfg_sd_sys_pll2_pfd2_fraction * wfg_sd_pfd_numerator == wfg_sd_sys_pll2_pfd2_hz,
              "SYS_PLL2_PFD2 is SYS_PLL2 * 18 / 24");
static_assert(wfg_sd_sys_pll2_pfd2_hz / wfg_sd_usdhc1_pll2_root_divider == wfg_sd_usdhc1_pll2_root_hz,
              "the USDHC1 root is SYS_PLL2_PFD2 / 2");
static_assert(wfg_sd_usdhc1_pll2_root_hz / wfg_sd_pll2_bus_divider == wfg_sd_pll2_bus_hz &&
                  wfg_sd_pll2_bus_hz <= wfg_sd_high_speed_max_hz &&
                  wfg_sd_usdhc1_pll2_root_hz / (wfg_sd_pll2_bus_divider - 1) > wfg_sd_high_speed_max_hz,
              "4 is the smallest divider of the 198 MHz root within High Speed's 50 MHz: a 49.5 MHz bus");
static_assert(wfg_sd_boot_bus_hz <= wfg_sd_high_speed_max_hz,
              "the 24 MHz root needs no divider: a 24 MHz bus");
static_assert(wfg_sd_usdhc1_pll2_root_hz % wfg_sd_identification_request_hz == 0 &&
                  wfg_sd_usdhc1_pll2_root_hz / wfg_sd_identification_request_hz == wfg_sd_identification_pll2_divider &&
                  wfg_sd_identification_request_hz >= wfg_sd_identification_min_hz &&
                  wfg_sd_identification_request_hz <= wfg_sd_identification_max_hz,
              "the identification request is the 198 MHz root over 1920, within 100 to 400 kHz");

#ifdef __cplusplus
}
#endif

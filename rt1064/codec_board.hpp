// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// Board-specific WM8960 wiring and 48 kHz clock setup. The DMA pipeline,
// codec control and bounded audio task remain in the shared codec driver.
extern "C"
{
#include "fsl_clock.h"
#include "fsl_iomuxc.h"
}

namespace waveform_generator::rt1064
{
inline void codec_pins_initialize() noexcept
{
    CLOCK_EnableClock(kCLOCK_Iomuxc);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_00_LPI2C1_SCL, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_01_LPI2C1_SDA, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_09_SAI1_MCLK, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_12_SAI1_RX_DATA00, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_13_SAI1_TX_DATA00, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_14_SAI1_TX_BCLK, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_15_SAI1_TX_SYNC, 1U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B1_00_LPI2C1_SCL, 0xD8B0U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B1_01_LPI2C1_SDA, 0xD8B0U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B1_09_SAI1_MCLK, 0x10B0U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B1_12_SAI1_RX_DATA00, 0x10B0U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B1_13_SAI1_TX_DATA00, 0x10B0U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B1_14_SAI1_TX_BCLK, 0x10B0U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B1_15_SAI1_TX_SYNC, 0x10B0U);
    IOMUXC_GPR->GPR1 |= IOMUXC_GPR_GPR1_SAI1_MCLK_DIR_MASK;
}

inline void codec_clocks_initialize() noexcept
{
    clock_audio_pll_config_t audio_pll{};
    audio_pll.loopDivider = 32U;
    audio_pll.postDivider = 1U;
    audio_pll.numerator = 768U;
    audio_pll.denominator = 1000U;
    CLOCK_InitAudioPll(&audio_pll);
    // 786.432 MHz / 4 / 16 = 12.288 MHz MCLK; WM8960 supplies BCLK/LRCLK.
    CLOCK_SetMux(kCLOCK_Sai1Mux, 2U);
    CLOCK_SetDiv(kCLOCK_Sai1PreDiv, 3U);
    CLOCK_SetDiv(kCLOCK_Sai1Div, 15U);
    // USB PLL / 8 / 6 = 10 MHz codec-control source clock.
    CLOCK_SetMux(kCLOCK_Lpi2cMux, 0U);
    CLOCK_SetDiv(kCLOCK_Lpi2cDiv, 5U);
}
} // namespace waveform_generator::rt1064

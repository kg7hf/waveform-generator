// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#include "board_pins.h"

#include "fsl_clock.h"
#include "fsl_iomuxc.h"

void BOARD_InitWaveformPins(void)
{
    CLOCK_EnableClock(kCLOCK_Iomuxc);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_04_GPIO9_IO03, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_24_LPUART1_TXD, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_25_LPUART1_RXD, 1U);

#if defined(WFG_PLAYER_IMAGE) || defined(WFG_SDCARD_IMAGE)
    /*
     * Original-EVK microSD socket: uSDHC1 plus its mechanical card-detect and
     * active-low power switch. Values are adapted from NXP's
     * evkmimxrt1170/sdcard_fatfs_freertos CM7 example at MCUX_2.16.100.
     */
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_32_GPIO_MUX3_IO31, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_34_USDHC1_VSELECT, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_35_GPIO10_IO02, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_SD_B1_00_USDHC1_CMD, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_SD_B1_01_USDHC1_CLK, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_SD_B1_02_USDHC1_DATA0, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_SD_B1_03_USDHC1_DATA1, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_SD_B1_04_USDHC1_DATA2, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_SD_B1_05_USDHC1_DATA3, 1U);

    /* Route GPIO_MUX3_IO31 to the CM7 GPIO3 instance. */
    IOMUXC_GPR->GPR43 = (IOMUXC_GPR->GPR43 & ~0x8000U) |
                         IOMUXC_GPR_GPR43_GPIO_MUX3_GPIO_SEL_HIGH(0x8000U);

    IOMUXC_SetPinConfig(IOMUXC_GPIO_SD_B1_00_USDHC1_CMD, 0x04U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_SD_B1_01_USDHC1_CLK, 0x0CU);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_SD_B1_02_USDHC1_DATA0, 0x04U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_SD_B1_03_USDHC1_DATA1, 0x04U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_SD_B1_04_USDHC1_DATA2, 0x04U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_SD_B1_05_USDHC1_DATA3, 0x04U);
#endif
}

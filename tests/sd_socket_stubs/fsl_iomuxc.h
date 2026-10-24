// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Host stand-in for the NXP IOMUXC driver (drivers/fsl_iomuxc.h) as
// rt1170/platform/waveform_sd.c uses it: the DATA3 pad's configuration. The
// SDK spells a pad as its five mux and input register values, which the
// stand-in's pad leaves zero; sd_socket_host.c defines the function.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define IOMUXC_GPIO_SD_B1_05_USDHC1_DATA3 0U, 0U, 0U, 0U, 0U

void IOMUXC_SetPinConfig(uint32_t muxRegister, uint32_t muxMode, uint32_t inputRegister, uint32_t inputDaisy, uint32_t configRegister,
                         uint32_t configValue);

#ifdef __cplusplus
}
#endif

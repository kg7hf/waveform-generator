// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Host stand-in for the NXP RT1170 clock driver (drivers/fsl_clock.h) as
// rt1170/platform/waveform_sd.c uses it: the USDHC1 root clock it selects and
// reads. sd_socket_host.c defines the functions.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum
{
    kCLOCK_Root_Usdhc1 = 58,
} clock_root_t;

enum
{
    kCLOCK_USDHC1_ClockRoot_MuxSysPll2Pfd2 = 5,
};

typedef struct
{
    bool clockOff;
    uint8_t mux;
    uint8_t div;
} clock_root_config_t;

void CLOCK_SetRootClock(clock_root_t root, const clock_root_config_t* config);
uint32_t CLOCK_GetRootClockFreq(clock_root_t root);

#ifdef __cplusplus
}
#endif

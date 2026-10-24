// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Host stand-in for the MCUXpresso common header as rt1170/platform/waveform_sd.c
// uses it: the non-cacheable placement of the SD host's DMA descriptors (an
// ordinary array here), the busy-wait delay, and the interrupt priority call,
// which sd_socket_host.c defines.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define AT_NONCACHEABLE_SECTION_ALIGN(var, alignbytes) var

enum
{
    SDK_DEVICE_MAXIMUM_CPU_CLOCK_FREQUENCY = 996000000,
};

typedef int32_t status_t;

// The RT1176's USDHC1 interrupt number.
typedef enum
{
    USDHC1_IRQn = 110,
} IRQn_Type;

void SDK_DelayAtLeastUs(uint32_t delay_us, uint32_t core_clock_hz);
void NVIC_SetPriority(IRQn_Type irq, uint32_t priority);

#ifdef __cplusplus
}
#endif

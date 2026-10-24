// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Host stand-in for the NXP RT1170 clock driver (drivers/fsl_clock.h) as
// rt1170/usb/usb_tinyusb_port.c uses it: the USB PHY's PLL and the USB
// controller's clock, each of which can fail to start. usb_port_host.c
// defines the functions.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum
{
    kCLOCK_Usb480M = 0,
} clock_usb_src_t;

typedef enum
{
    kCLOCK_Usbphy480M = 0,
} clock_usb_phy_src_t;

bool CLOCK_EnableUsbhs0PhyPllClock(clock_usb_phy_src_t src, uint32_t freq);
bool CLOCK_EnableUsbhs0Clock(clock_usb_src_t src, uint32_t freq);

#ifdef __cplusplus
}
#endif

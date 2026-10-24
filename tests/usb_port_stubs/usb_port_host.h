// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The C++ test's view of the stand-ins in usb_port_host.c, which replace what
// rt1170/usb/usb_tinyusb_port.c calls: the USB clocks, the USB PHY's
// registers, the interrupt priority, TinyUSB's start and the EP0 order's
// reset. Functions only, so the C++ test never compiles TinyUSB's headers or
// the stand-in register header.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

// Whether the USB PHY's PLL, the USB controller's clock and TinyUSB's start
// (tusb_init) fail from now on; each starts at start.
void wfg_test_fail_usb_phy_pll(bool fail);
void wfg_test_fail_usb_clock(bool fail);
void wfg_test_fail_tinyusb_start(bool fail);

// Puts the USB PHY's registers at their reset values, the port's bring-up
// status (g_m110_usb_port_status) at 0, and clears the calls recorded.
void wfg_test_reset_usb_port(void);

// The calls the port made since the reset, in order, separated by a space:
// phy_pll(<source>,<crystal Hz>), usb_clock(<source>,<crystal Hz>),
// nvic(<interrupt>,<priority>), ep0_reset, and tusb_init(<port>,<role>,<speed>;<the
// bring-up status in hex as TinyUSB starts>).
const char* wfg_test_usb_port_calls(void);

// The port's bring-up status (g_m110_usb_port_status), and the USB PHY's
// CTRL, PWD and TX registers.
uint32_t wfg_test_usb_port_status(void);
uint32_t wfg_test_usb_phy_ctrl(void);
uint32_t wfg_test_usb_phy_pwd(void);
uint32_t wfg_test_usb_phy_tx(void);

#ifdef __cplusplus
}
#endif

// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Host stand-in for the EVK board's clock_config.h as
// rt1170/usb/usb_tinyusb_port.c uses it: the 24 MHz crystal the USB PHY's PLL
// runs from (the SDK's evkmimxrt1170 value).

#pragma once

enum
{
    BOARD_XTAL0_CLK_HZ = 24000000,
};

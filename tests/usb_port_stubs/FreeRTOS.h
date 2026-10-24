// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Host stand-in for FreeRTOS.h as rt1170/usb/usb_tinyusb_port.c uses it: the
// USB interrupt's priority, the highest that may call FreeRTOS
// (rt1170/os/FreeRTOSConfig.h's value).

#pragma once

enum
{
    configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5,
};

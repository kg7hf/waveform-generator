// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Host stand-ins for the hardware that rt1170/usb/usb_descriptors.c reads: the
// bus speed TinyUSB reports (tud_speed_get) and the OCOTP fuse words of the
// serial number. tests/usb_descriptor_tests.cpp selects the speed and expects
// the serial these fuse words spell.

#include "usb_descriptor_bus.h"

#include "MIMXRT1176_cm7.h"
#include "tusb.h"

enum
{
    serial_fuse_high = 0x01234567,
    serial_fuse_low = 0x76543210,
};

// The fuse bank the descriptors read through OCOTP, as the register block
// pointer of the device header does on the target.
static TestOcotp fuses = {{{0U}, {serial_fuse_high}, {serial_fuse_low}}};
TestOcotp* OCOTP = &fuses;

static bool* bus_high_speed(void)
{
    static bool high_speed = true;
    return &high_speed;
}

void wfg_test_set_high_speed(bool high_speed)
{
    *bus_high_speed() = high_speed;
}

tusb_speed_t tud_speed_get(void)
{
    return *bus_high_speed() ? TUSB_SPEED_HIGH : TUSB_SPEED_FULL;
}

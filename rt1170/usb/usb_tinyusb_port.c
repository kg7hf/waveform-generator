// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#include "usb/usb_tinyusb_port.h"

#include "FreeRTOS.h"
#include "clock_config.h"
#include "fsl_clock.h"
#include "fsl_device_registers.h"
#include "tusb.h"

enum
{
    usb_root_port = 0U,
    usb_status_hardware_ready = 0x55534201U,
    usb_status_controller_configured = 0x55534202U,
    usb_status_stack_initialized = 0x55534203U,
    usb_status_task_servicing = 0x55534204U,
    usb_status_controller_failed = 0x555342E2U,
    usb_status_stack_failed = 0x555342E3U,
};

volatile uint32_t g_m110_usb_port_status;

static bool initialize_usb_hardware(void)
{
    if (!CLOCK_EnableUsbhs0PhyPllClock(kCLOCK_Usbphy480M, BOARD_XTAL0_CLK_HZ))
    {
        return false;
    }

    if (!CLOCK_EnableUsbhs0Clock(kCLOCK_Usb480M, BOARD_XTAL0_CLK_HZ))
    {
        return false;
    }

    USBPHY1->CTRL |= USBPHY_CTRL_SET_ENUTMILEVEL2_MASK |
                     USBPHY_CTRL_SET_ENUTMILEVEL3_MASK;
    USBPHY1->PWD = 0U;

    uint32_t transmitter = USBPHY1->TX;
    transmitter &= ~(USBPHY_TX_D_CAL_MASK |
                     USBPHY_TX_TXCAL45DM_MASK |
                     USBPHY_TX_TXCAL45DP_MASK);
    transmitter |= USBPHY_TX_D_CAL(0x0CU) |
                   USBPHY_TX_TXCAL45DP(0x06U) |
                   USBPHY_TX_TXCAL45DM(0x06U);
    USBPHY1->TX = transmitter;

    NVIC_SetPriority(USB_OTG1_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
    return true;
}

bool m110_usb_hardware_initialize(void)
{
    if (!initialize_usb_hardware())
    {
        g_m110_usb_port_status = usb_status_controller_failed;
        return false;
    }

    g_m110_usb_port_status = usb_status_hardware_ready;
    return true;
}

bool m110_tinyusb_stack_initialize(void)
{
    g_m110_usb_port_status = usb_status_controller_configured;

    const tusb_rhport_init_t root_configuration = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_HIGH,
    };
    if (!tusb_init(usb_root_port, &root_configuration))
    {
        g_m110_usb_port_status = usb_status_stack_failed;
        return false;
    }

    g_m110_usb_port_status = usb_status_stack_initialized;
    return true;
}

void m110_tinyusb_task(uint32_t timeout_ms)
{
    g_m110_usb_port_status = usb_status_task_servicing;
    tud_task_ext(timeout_ms, false);
}

void m110_tinyusb_stack_deinitialize(void)
{
    (void)tud_deinit(usb_root_port);
    g_m110_usb_port_status = usb_status_hardware_ready;
}

uint32_t m110_tinyusb_cdc_available(void)
{
    return tud_cdc_available();
}

uint32_t m110_tinyusb_cdc_read(void* buffer, uint32_t size)
{
    return tud_cdc_read(buffer, size);
}

uint32_t m110_tinyusb_cdc_write(const void* buffer, uint32_t size)
{
    return tud_cdc_write(buffer, size);
}

void m110_tinyusb_cdc_flush(void)
{
    (void)tud_cdc_write_flush();
}

bool m110_tinyusb_cdc_connected(void)
{
    return tud_cdc_connected();
}

void USB_OTG1_IRQHandler(void)
{
    tusb_int_handler(usb_root_port, true);
}

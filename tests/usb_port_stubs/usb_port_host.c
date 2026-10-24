// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Host stand-ins for what rt1170/usb/usb_tinyusb_port.c calls, built with the
// player's TinyUSB configuration: the USB clocks, the USB PHY's and the USB
// controller's registers (plain memory), the interrupt priority, TinyUSB's
// device stack and CDC calls, and the EP0 order and audio entry points the
// player's interrupt glue uses. The clocks and TinyUSB's start fail when the
// test says so; the clocks, the priority, TinyUSB's start and the EP0 order's
// reset are recorded in order for tests/usb_port_tests.cpp, which reads them
// through usb_port_host.h. The interrupt paths are not exercised here.

#include "usb_port_host.h"

#include "clock_config.h"
#include "fsl_clock.h"
#include "fsl_device_registers.h"
#include "tusb.h"
#include "device/dcd.h"
#include "usb/usb_audio_callbacks.h"
#include "usb/usb_ep0_order.h"
#include "usb/usb_ep0_order_glue.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum
{
    call_log_bytes = 256,
};

// The RT1176 USB PHY's CTRL, PWD and TX at reset (its reference manual).
static const uint32_t phy_ctrl_at_reset = 0xC0200000U;
static const uint32_t phy_pwd_at_reset = 0x001E1C00U;
static const uint32_t phy_tx_at_reset = 0x0A000402U;

typedef struct
{
    bool phy_pll_fails;
    bool usb_clock_fails;
    bool tinyusb_start_fails;
    char calls[call_log_bytes];
    size_t calls_length;
} port_model_t;

static port_model_t* model(void)
{
    static port_model_t instance;
    return &instance;
}

static USBPHY_Type usb_phy;
static USB_Type usb_otg1;

USBPHY_Type* const USBPHY1 = &usb_phy;
USB_Type* const USB_OTG1 = &usb_otg1;

// The port's own, which it gives external linkage and no header declares.
extern volatile uint32_t g_m110_usb_port_status;

// Appends one call to the log, after a space unless it is the first.
static void record(const char* call)
{
    port_model_t* const port = model();
    const size_t room = sizeof(port->calls) - port->calls_length;
    const int written = snprintf(&port->calls[port->calls_length], room, "%s%s", port->calls_length == 0U ? "" : " ", call);

    if (written > 0 && (size_t)written < room)
    {
        port->calls_length += (size_t)written;
    }
}

void wfg_test_fail_usb_phy_pll(bool fail)
{
    model()->phy_pll_fails = fail;
}

void wfg_test_fail_usb_clock(bool fail)
{
    model()->usb_clock_fails = fail;
}

void wfg_test_fail_tinyusb_start(bool fail)
{
    model()->tinyusb_start_fails = fail;
}

void wfg_test_reset_usb_port(void)
{
    usb_phy.CTRL = phy_ctrl_at_reset;
    usb_phy.PWD = phy_pwd_at_reset;
    usb_phy.TX = phy_tx_at_reset;
    g_m110_usb_port_status = 0U;
    model()->calls[0] = '\0';
    model()->calls_length = 0U;
}

const char* wfg_test_usb_port_calls(void)
{
    return model()->calls;
}

uint32_t wfg_test_usb_port_status(void)
{
    return g_m110_usb_port_status;
}

uint32_t wfg_test_usb_phy_ctrl(void)
{
    return usb_phy.CTRL;
}

uint32_t wfg_test_usb_phy_pwd(void)
{
    return usb_phy.PWD;
}

uint32_t wfg_test_usb_phy_tx(void)
{
    return usb_phy.TX;
}

bool CLOCK_EnableUsbhs0PhyPllClock(clock_usb_phy_src_t src, uint32_t freq)
{
    char call[call_log_bytes];
    (void)snprintf(call, sizeof(call), "phy_pll(%d,%" PRIu32 ")", (int)src, freq);
    record(call);
    return !model()->phy_pll_fails;
}

bool CLOCK_EnableUsbhs0Clock(clock_usb_src_t src, uint32_t freq)
{
    char call[call_log_bytes];
    (void)snprintf(call, sizeof(call), "usb_clock(%d,%" PRIu32 ")", (int)src, freq);
    record(call);
    return !model()->usb_clock_fails;
}

void NVIC_SetPriority(IRQn_Type irq, uint32_t priority)
{
    char call[call_log_bytes];
    (void)snprintf(call, sizeof(call), "nvic(%d,%" PRIu32 ")", (int)irq, priority);
    record(call);
}

bool tusb_rhport_init(uint8_t rhport, const tusb_rhport_init_t* rh_init)
{
    char call[call_log_bytes];
    (void)snprintf(call, sizeof(call), "tusb_init(%u,%d,%d;%08" PRIX32 ")", (unsigned)rhport, (int)rh_init->role, (int)rh_init->speed,
                   g_m110_usb_port_status);
    record(call);
    return !model()->tinyusb_start_fails;
}

void wfg_usb_ep0_order_reset(void)
{
    record("ep0_reset");
}

// The rest is linked, never called by the test.
void tud_task_ext(uint32_t timeout_ms, bool in_isr)
{
    (void)timeout_ms;
    (void)in_isr;
}

bool tud_deinit(uint8_t rhport)
{
    (void)rhport;
    return true;
}

void tusb_int_handler(uint8_t rhport, bool in_isr)
{
    (void)rhport;
    (void)in_isr;
}

uint32_t tud_cdc_n_available(uint8_t itf)
{
    (void)itf;
    return 0U;
}

uint32_t tud_cdc_n_read(uint8_t itf, void* buffer, uint32_t bufsize)
{
    (void)itf;
    (void)buffer;
    (void)bufsize;
    return 0U;
}

uint32_t tud_cdc_n_write(uint8_t itf, void const* buffer, uint32_t bufsize)
{
    (void)itf;
    (void)buffer;
    (void)bufsize;
    return 0U;
}

uint32_t tud_cdc_n_write_flush(uint8_t itf)
{
    (void)itf;
    return 0U;
}

bool tud_cdc_n_connected(uint8_t itf)
{
    (void)itf;
    return false;
}

void wfg_usb_ep0_order_begin(void) {}

void wfg_usb_ep0_order_end(void) {}

bool wfg_usb_ep0_order_another_pass(bool ep0_completion_pending)
{
    (void)ep0_completion_pending;
    return false;
}

void wfg_usb_ep0_route_event(dcd_event_t const* event, bool in_isr, void (*forward)(dcd_event_t const* event, bool in_isr),
                             bool (*ep0_completion_pending)(void))
{
    (void)ep0_completion_pending;
    forward(event, in_isr);
}

void wfg_usb_ep0_release_setups(void (*forward)(dcd_event_t const* event, bool in_isr))
{
    (void)forward;
}

void wfg_usb_audio_watchdog_expired(bool transfer_primed)
{
    (void)transfer_primed;
}

// TinyUSB's own event handler, which the firmware reaches through GNU ld's
// --wrap under this assembler name.
void wfg_test_real_dcd_event_handler(dcd_event_t const* event, bool in_isr) __asm__("__real_dcd_event_handler");

void wfg_test_real_dcd_event_handler(dcd_event_t const* event, bool in_isr)
{
    (void)event;
    (void)in_isr;
}

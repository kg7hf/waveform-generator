// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "usb/usb_tinyusb_port.h"

#include "FreeRTOS.h"
#include "clock_config.h"
#include "fsl_clock.h"
#include "fsl_device_registers.h"
#include "tusb.h"

#if defined(CPU_MIMXRT1064DVL6A)
#define WFG_USB_CONTROLLER USB1
#else
#define WFG_USB_CONTROLLER USB_OTG1
#endif

#if defined(WFG_PLAYER_IMAGE)
#include "device/dcd.h"
#include "usb/usb_audio_callbacks.h"
#include "usb/usb_composite.h"
#include "usb/usb_ep0_order.h"
#include "usb/usb_ep0_order_glue.h"
#endif

enum
{
    usb_root_port = 0U,
    usb_status_hardware_ready = 0x55534201U,
    usb_status_controller_configured = 0x55534202U,
    usb_status_stack_initialized = 0x55534203U,
    usb_status_task_servicing = 0x55534204U,
    usb_status_controller_failed = 0x555342E2U,
    usb_status_stack_failed = 0x555342E3U,
    // FRINDEX counts microframes: the frame number sits above three microframe bits.
    usb_microframe_bits = 3U,
};

#if defined(WFG_PLAYER_IMAGE)
enum
{
    // GPTLD holds a duration in microseconds less one. The watchdog samples
    // 1.5 service intervals of 1000 us after a completion handled before its
    // deadline, and one interval after anything else: a completion handled at
    // or past its deadline, or a sample that found the packet still waiting.
    usb_audio_watchdog_load_us = 1500U - 1U,
    usb_audio_watchdog_interval_load_us = 1000U - 1U,
    usb_endpoint_number_mask = 0x0FU,
    // Its bit in ENDPTSTAT's transmit half (ETBR) is set while a transfer is
    // primed on the endpoint.
    usb_audio_in_endpoint_number = wfg_uac_endpoint_in & usb_endpoint_number_mask,
};
#endif

// Debugger-visible bring-up progress. Values are stable evidence codes for the
// board ports and do not participate in protocol behavior. Only this file uses
// it, so no header declares it. It keeps external linkage, so the image and its
// symbols are unchanged and the debugger finds it under this name as before.
volatile uint32_t g_m110_usb_port_status;

static __attribute__((warn_unused_result)) enum wfg_status initialize_usb_hardware(void)
{
    if (!CLOCK_EnableUsbhs0PhyPllClock(kCLOCK_Usbphy480M, BOARD_XTAL0_CLK_HZ))
    {
        return kWFG_StatusUsbClockFailed;
    }

    if (!CLOCK_EnableUsbhs0Clock(kCLOCK_Usb480M, BOARD_XTAL0_CLK_HZ))
    {
        return kWFG_StatusUsbClockFailed;
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
    return kWFG_StatusOk;
}

enum wfg_status m110_usb_hardware_initialize(void)
{
    const enum wfg_status hardware = initialize_usb_hardware();

    if (hardware != kWFG_StatusOk)
    {
        g_m110_usb_port_status = usb_status_controller_failed;
        return hardware;
    }

    g_m110_usb_port_status = usb_status_hardware_ready;
    return kWFG_StatusOk;
}

enum wfg_status m110_tinyusb_stack_initialize(void)
{
    g_m110_usb_port_status = usb_status_controller_configured;
#if defined(WFG_PLAYER_IMAGE)
    wfg_usb_ep0_order_reset();
#endif

    const tusb_rhport_init_t root_configuration = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_HIGH,
    };
    if (!tusb_init(usb_root_port, &root_configuration))
    {
        g_m110_usb_port_status = usb_status_stack_failed;
        return kWFG_StatusUsbStackFailed;
    }

    g_m110_usb_port_status = usb_status_stack_initialized;
    return kWFG_StatusOk;
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

uint16_t m110_usb_frame_number(void)
{
    const uint32_t frame_index = WFG_USB_CONTROLLER->FRINDEX & USB_FRINDEX_FRINDEX_MASK;
    return (uint16_t)(frame_index >> usb_microframe_bits);
}

#if defined(WFG_PLAYER_IMAGE)
// Stopped until the first completion restarts it, which loads its deadline.
void m110_usb_audio_watchdog_start(void)
{
    WFG_USB_CONTROLLER->GPTIMER0CTRL = 0U;
    WFG_USB_CONTROLLER->USBSTS = USB_USBSTS_TI0_MASK;
    WFG_USB_CONTROLLER->USBINTR |= USB_USBINTR_TIE0_MASK;
}

// One-shot (GPTMODE 0): GPTRST reloads the count from GPTLD, and it runs.
static void run_audio_watchdog(uint32_t load_us)
{
    WFG_USB_CONTROLLER->GPTIMER0LD = USB_GPTIMER0LD_GPTLD(load_us);
    WFG_USB_CONTROLLER->GPTIMER0CTRL = USB_GPTIMER0CTRL_GPTRUN_MASK | USB_GPTIMER0CTRL_GPTRST_MASK;
}

// A one-shot count that has reached zero stays there: this completion was
// handled at or past the deadline, so its ISR ran late and the next sample
// comes one interval after it, which keeps that sample ahead of the poll after
// next. An expiry this completion overtook is void; acknowledging it after the
// reload cannot drop a new one, which is at least 1 ms away.
void m110_usb_audio_watchdog_restart(void)
{
    const bool overdue = (WFG_USB_CONTROLLER->GPTIMER0CTRL & USB_GPTIMER0CTRL_GPTCNT_MASK) == 0U;
    run_audio_watchdog(overdue ? usb_audio_watchdog_interval_load_us : usb_audio_watchdog_load_us);
    WFG_USB_CONTROLLER->USBSTS = USB_USBSTS_TI0_MASK;
}

void m110_usb_audio_watchdog_stop(void)
{
    WFG_USB_CONTROLLER->GPTIMER0CTRL = 0U;
    WFG_USB_CONTROLLER->USBSTS = USB_USBSTS_TI0_MASK;
}

// Runs before TinyUSB handles anything, so a completion pending with the
// expiry is still unhandled and the packet after it is not primed yet. TI0 is
// set only while the watchdog runs, and is acknowledged here. While the packet
// waits (the host has not polled), the watchdog samples again one interval
// later, for as long as the gap lasts. Nothing primed comes with the completion
// that ends the gap, pending in this same interrupt, whose restart re-arms it;
// if none is pending (TinyUSB did not prime), the watchdog stays stopped.
static void report_audio_watchdog(void)
{
    if ((WFG_USB_CONTROLLER->USBSTS & USB_USBSTS_TI0_MASK) == 0U)
    {
        return;
    }

    WFG_USB_CONTROLLER->USBSTS = USB_USBSTS_TI0_MASK;
    const bool primed = (WFG_USB_CONTROLLER->ENDPTSTAT & USB_ENDPTSTAT_ETBR(1U << usb_audio_in_endpoint_number)) != 0U;

    if (primed)
    {
        run_audio_watchdog(usb_audio_watchdog_interval_load_us);
    }

    wfg_usb_audio_watchdog_expired(primed);
}
#endif

#if defined(WFG_PLAYER_IMAGE)
// GNU ld's --wrap=dcd_event_handler (rt1170/CMakeLists.txt) sends every call
// the ChipIdea driver makes to TinyUSB's dcd_event_handler to the symbol
// __wrap_dcd_event_handler first, and __real_dcd_event_handler is TinyUSB's
// own. Those names are the linker's and reserved in C, so the two functions
// here carry them only as their assembler names.
void wfg_real_dcd_event_handler(dcd_event_t const* event, bool in_isr) __asm__("__real_dcd_event_handler");
void wfg_wrap_dcd_event_handler(dcd_event_t const* event, bool in_isr) __asm__("__wrap_dcd_event_handler");

// An EP0 transfer (either direction) completed after the driver read
// ENDPTCOMPLETE: its bit is set again and not yet acknowledged.
static bool ep0_completion_pending(void)
{
    return (WFG_USB_CONTROLLER->ENDPTCOMPLETE & (USB_ENDPTCOMPLETE_ERCE(1U) | USB_ENDPTCOMPLETE_ETCE(1U))) != 0U;
}

void wfg_wrap_dcd_event_handler(dcd_event_t const* event, bool in_isr)
{
    wfg_usb_ep0_route_event(event, in_isr, &wfg_real_dcd_event_handler, &ep0_completion_pending);
}

// TinyUSB's handler, in passes: while a SETUP it reported is held and an EP0
// completion it has not seen is pending, it runs again, so that completion is
// reported first; then the held SETUPs go on (usb_ep0_order.hpp, #32).
static void run_tinyusb_passes(void)
{
    wfg_usb_ep0_order_begin();
    tusb_int_handler(usb_root_port, true);

    while (wfg_usb_ep0_order_another_pass(ep0_completion_pending()))
    {
        tusb_int_handler(usb_root_port, true);
    }

    wfg_usb_ep0_release_setups(&wfg_real_dcd_event_handler);
    wfg_usb_ep0_order_end();
}
#endif

// TinyUSB's handler acknowledges every USBSTS bit that is both set and enabled
// in USBINTR. TIE0 is hidden from it while it runs, so an expiry that comes
// meanwhile stays latched, raises the interrupt again once TIE0 is restored,
// and is reported by report_audio_watchdog() rather than dropped. TinyUSB's
// own USBINTR changes are kept: only TIE0 is cleared and restored.
void USB_OTG1_IRQHandler(void)
{
#if defined(WFG_PLAYER_IMAGE)
    report_audio_watchdog();
    const uint32_t watchdog_enabled = WFG_USB_CONTROLLER->USBINTR & USB_USBINTR_TIE0_MASK;
    WFG_USB_CONTROLLER->USBINTR &= ~USB_USBINTR_TIE0_MASK;
    run_tinyusb_passes();
    WFG_USB_CONTROLLER->USBINTR |= watchdog_enabled;
#else
    tusb_int_handler(usb_root_port, true);
#endif
}

// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The USB interrupt's routing of TinyUSB's DCD events through the EP0 order
// Guide: docs/modules/rt1170-player-and-audio.md#ep0-event-order

#include "usb/usb_ep0_order_glue.h"
#include "usb/usb_ep0_order.h"

#include <string.h>

_Static_assert(sizeof(tusb_control_request_t) == sizeof((struct wfg_usb_setup){0}.packet), "a SETUP packet fills the held packet exactly");

static bool is_ep0_completion(dcd_event_t const* event)
{
    return event->event_id == DCD_EVENT_XFER_COMPLETE && tu_edpt_number(event->xfer_complete.ep_addr) == 0U;
}

void wfg_usb_ep0_release_setups(void (*forward)(dcd_event_t const* event, bool in_isr))
{
    struct wfg_usb_setup setup;

    while (wfg_usb_ep0_order_release(&setup))
    {
        dcd_event_t event = {.rhport = setup.rhport, .event_id = DCD_EVENT_SETUP_RECEIVED};
        memcpy(&event.setup_received, &setup.packet, sizeof(setup.packet));
        forward(&event, true);
    }
}

// Another event goes on after the held SETUPs, which are released first. One
// that comes while the controller still shows an EP0 completion no pass has
// reported lets them precede it: the order counts that release.
static void release_before_event(void (*forward)(dcd_event_t const* event, bool in_isr), bool (*ep0_completion_pending)(void))
{
    if (!wfg_usb_ep0_order_holding())
    {
        return;
    }

    wfg_usb_ep0_order_releasing(ep0_completion_pending());
    wfg_usb_ep0_release_setups(forward);
}

// A SETUP is held while the interrupt runs; an EP0 completion goes on ahead
// of the held SETUPs, and so does a SOF, whose place among them TinyUSB never
// reads; any other event goes after them, so a bus reset or an unplug still
// follows the SETUPs the driver reported before it.
void wfg_usb_ep0_route_event(dcd_event_t const* event, bool in_isr, void (*forward)(dcd_event_t const* event, bool in_isr),
                             bool (*ep0_completion_pending)(void))
{
    if (!in_isr)
    {
        forward(event, in_isr);
        return;
    }

    if (event->event_id == DCD_EVENT_SETUP_RECEIVED)
    {
        struct wfg_usb_setup setup = {.rhport = event->rhport};
        memcpy(&setup.packet, &event->setup_received, sizeof(setup.packet));

        if (wfg_usb_ep0_order_hold(&setup) != kWFG_StatusOk)
        {
            // Outside the passes, or past a full hold, which the order
            // counted: after the held ones.
            wfg_usb_ep0_release_setups(forward);
            forward(event, in_isr);
        }

        return;
    }

    if (is_ep0_completion(event))
    {
        wfg_usb_ep0_order_completion();
        forward(event, in_isr);
        return;
    }

    if (event->event_id != DCD_EVENT_SOF)
    {
        release_before_event(forward, ep0_completion_pending);
    }

    forward(event, in_isr);
}

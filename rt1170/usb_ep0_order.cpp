// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The USB interrupt's Ep0Order and its C entry points (usb/usb_ep0_order.h)
// Guide: docs/modules/rt1170-player-and-audio.md#ep0-event-order

#include "usb_ep0_order.hpp"

#include "c_status_map.hpp"
#include "os/critical_section.hpp"
#include "usb/usb_ep0_order.h"

namespace waveform_generator::usb_ep0_order
{

Ep0Order& ep0_order() noexcept
{
    __attribute__((section(".ocram.usb_ep0_order"))) static constinit Ep0Order order{};
    return order;
}

Counters ep0_order_counters() noexcept
{
    const TaskCriticalSection lock;
    return ep0_order().counters();
}

namespace
{

[[nodiscard]] constexpr Setup from_c(const wfg_usb_setup& setup) noexcept
{
    return {.rhport = setup.rhport, .packet = setup.packet};
}

constexpr void to_c(const Setup& setup, wfg_usb_setup& out) noexcept
{
    out.rhport = setup.rhport;
    out.packet = setup.packet;
}

} // namespace

} // namespace waveform_generator::usb_ep0_order

// The C entry points: the USB ISR's glue, plus one reset before TinyUSB starts.
namespace ep0 = waveform_generator::usb_ep0_order;

void wfg_usb_ep0_order_reset()
{
    ep0::ep0_order() = ep0::Ep0Order{};
}

void wfg_usb_ep0_order_begin()
{
    ep0::ep0_order().begin();
}

void wfg_usb_ep0_order_end()
{
    ep0::ep0_order().end();
}

// The value of the Status hold() returns: kWFG_StatusOk for its success
// (c_status_of() takes every success as kWFG_StatusOk), else its refusal's
// row in c_status_map.hpp (a static_assert there). A refusal without a row
// is still a SETUP not held, which the glue passes on: the fallback says so.
wfg_status wfg_usb_ep0_order_hold(const wfg_usb_setup* setup)
{
    if (setup == nullptr)
    {
        return kWFG_StatusInvalidArgument;
    }

    return waveform_generator::c_status_of(ep0::ep0_order().hold(ep0::from_c(*setup))).value_or(kWFG_StatusEp0OrderIdle);
}

void wfg_usb_ep0_order_completion()
{
    ep0::ep0_order().completion();
}

bool wfg_usb_ep0_order_holding()
{
    return ep0::ep0_order().held() != 0U;
}

void wfg_usb_ep0_order_releasing(bool ep0_completion_pending)
{
    ep0::ep0_order().releasing(ep0_completion_pending ? ep0::Ep0Pending::completion : ep0::Ep0Pending::none);
}

// False only when no SETUP is held: a null setup is the caller's defect,
// reported as a critical error, which does not return.
bool wfg_usb_ep0_order_release(wfg_usb_setup* setup)
{
    if (setup == nullptr)
    {
        m110::critical_error(ep0::release_without_setup);
    }

    const auto oldest = ep0::ep0_order().release();

    if (oldest.has_value())
    {
        ep0::to_c(oldest.value(), *setup);
    }

    return oldest.has_value();
}

bool wfg_usb_ep0_order_another_pass(bool ep0_completion_pending)
{
    const auto pending = ep0_completion_pending ? ep0::Ep0Pending::completion : ep0::Ep0Pending::none;
    return ep0::ep0_order().another_pass(pending) == ep0::NextPass::again;
}

// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The order of TinyUSB's EP0 events within one USB interrupt
// Guide: docs/modules/rt1170-player-and-audio.md#ep0-event-order
// Ownership: the USB ISR's glue (usb/usb_tinyusb_port.c, usb/usb_ep0_order_glue.c)
// drives the one Ep0Order through usb/usb_ep0_order.h; ep0_order_counters()
// reads its counters for USB? under a critical section.
// Contract: a SETUP that TinyUSB's ChipIdea driver reports during an
// interrupt is held until the interrupt ends; an EP0 transfer completion
// reported meanwhile is passed on ahead of it, and while one is still pending
// in the controller after a pass the glue runs another pass (at most
// max_extra_passes). A SOF passes on at once too: the driver reports it after
// the pass's SETUP, and releasing the SETUP then would put it back ahead of
// the completion the next pass reports. Every other event releases the held
// SETUPs first, so its order is unchanged; one that comes while an EP0
// completion is still pending counts as unordered.
//
// Why (#32). The driver's interrupt reads ENDPTCOMPLETE, reports what it
// found (the audio IN completion, with its packet work, among it), and only
// then reads ENDPTSETUPSTAT. When a control transfer's last stage completes
// on the bus inside that window and the host's next SETUP follows at once,
// the SETUP is reported in this pass and the completion in the next
// interrupt, after it. TinyUSB matches an EP0 completion against the request
// it is processing, so it takes the previous transfer's status ZLP for the
// new request's data stage: an IN-data request such as GET_LINE_CODING or a
// UAC2 GET CUR then sees its data stage end twice, and the second time queues
// its status stage on an endpoint already busy with the first (usbd.c 1587,
// then 941: the pair of assertions window 3 recorded). The previous
// transfer's own last stage is lost too, so a SET_CONTROL_LINE_STATE never
// reaches the CDC driver. A completion pending when TinyUSB reports a SETUP
// cannot belong to that SETUP, which nothing has answered yet: it completed
// before it on the bus, and reporting it first restores the bus order.

#pragma once

#include "common/status.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace waveform_generator::usb_ep0_order
{

// Passes after the first in one interrupt. One is enough: no EP0 transfer
// for a held SETUP can be primed before the task has seen it, so a second
// pass leaves nothing pending of the transfers before it. The bound only
// keeps a controller that never stops completing from holding the interrupt.
inline constexpr std::uint32_t max_extra_passes = 2U;
// SETUPs one pass can report: TinyUSB's ChipIdea driver reads ENDPTSETUPSTAT
// once per pass, and EP0 is the only control endpoint.
inline constexpr std::size_t setups_per_pass = 1U;
// SETUPs one interrupt can hold.
inline constexpr std::size_t hold_capacity = 3U;

static_assert(hold_capacity >= setups_per_pass * (1U + max_extra_passes), "the hold keeps every SETUP the passes of one interrupt can report");

// Why hold() did not hold a SETUP; the glue then releases the held ones and
// passes it on after them, in order.
inline constexpr m110::Status not_in_interrupt{.code = m110::StatusCode::unavailable, .message = "EP0_ORDER_IDLE"};
inline constexpr m110::Status hold_full{.code = m110::StatusCode::buffer_too_small, .message = "EP0_HOLD_FULL"};
// A release given no SETUP to fill (wfg_usb_ep0_order_release(nullptr)): the
// caller's defect, which m110::critical_error() reports; never "none held".
inline constexpr m110::Status release_without_setup{.code = m110::StatusCode::invalid_argument, .message = "EP0_RELEASE_WITHOUT_SETUP"};

// Whether the controller still shows an EP0 completion after a pass
// (ENDPTCOMPLETE), and what follows the pass.
enum class Ep0Pending : std::uint8_t
{
    none,
    completion,
};

enum class NextPass : std::uint8_t
{
    done,
    again,
};

// A SETUP as the driver reported it: its root port and its packet's 8 bytes
// as the glue copied them (wfg_usb_setup), held and never read.
struct Setup
{
    std::uint8_t rhport{};
    std::uint64_t packet{};
};

// Since boot. usb_ep0_reordered and usb_ep0_unordered in USB?.
struct Counters
{
    std::uint32_t reordered{}; // EP0 completions passed on ahead of a held SETUP: the race, put in bus order
    std::uint32_t unordered{}; // SETUPs the order could not keep behind a completion: released with one still pending (at the pass bound, or for another event) or not held for a full hold
};

class Ep0Order
{
public:
    // The interrupt starts, before TinyUSB's first pass.
    constexpr void begin() noexcept
    {
        active_ = true;
        passes_ = 0U;
    }

    // A SETUP of a pass, held until the interrupt ends. Else not_in_interrupt,
    // or hold_full, which counts as unordered (an EP0 completion the passes
    // have not reported may then follow it); either way the glue releases the
    // held SETUPs and passes this one on, in order.
    [[nodiscard]] constexpr m110::Status hold(const Setup& setup) noexcept
    {
        if (!active_)
        {
            return not_in_interrupt;
        }

        if (count_ == held_.size())
        {
            ++counters_.unordered;
            return hold_full;
        }

        held_.at((first_ + count_) % held_.size()) = setup;
        ++count_;
        return m110::Status::success();
    }

    // An EP0 completion of a pass, which the glue passes on now: ahead of the
    // held SETUPs, if any.
    constexpr void completion() noexcept
    {
        if (active_ && count_ != 0U)
        {
            ++counters_.reordered;
        }
    }

    // Another event of a pass goes on after the held SETUPs, which the glue
    // releases first. With an EP0 completion `pending` that no pass has
    // reported (a bus event the driver reports at the start of an extra
    // pass), they now precede it: one unordered release. The glue asks only
    // while a SETUP is held.
    constexpr void releasing(Ep0Pending pending) noexcept
    {
        if (active_ && count_ != 0U && pending == Ep0Pending::completion)
        {
            ++counters_.unordered;
        }
    }

    // The oldest held SETUP, which the glue passes on now; none once all are.
    [[nodiscard]] constexpr std::optional<Setup> release() noexcept
    {
        if (count_ == 0U)
        {
            return std::nullopt;
        }

        const Setup oldest = held_.at(first_);
        first_ = (first_ + 1U) % held_.size();
        --count_;
        return oldest;
    }

    // After a pass: another, when a SETUP is held and the controller has an
    // EP0 completion `pending` that this pass did not see, at most
    // max_extra_passes times; at the bound the SETUPs go with it still
    // pending, which counts as unordered.
    [[nodiscard]] constexpr NextPass another_pass(Ep0Pending pending) noexcept
    {
        if (!active_ || count_ == 0U || pending == Ep0Pending::none)
        {
            return NextPass::done;
        }

        if (passes_ == max_extra_passes)
        {
            ++counters_.unordered;
            return NextPass::done;
        }

        ++passes_;
        return NextPass::again;
    }

    // The interrupt ends, once the glue has released every held SETUP.
    constexpr void end() noexcept
    {
        active_ = false;
        passes_ = 0U;
    }

    [[nodiscard]] constexpr Counters counters() const noexcept { return counters_; }

    [[nodiscard]] constexpr std::size_t held() const noexcept { return count_; }

private:
    std::array<Setup, hold_capacity> held_{};
    std::size_t first_{};
    std::size_t count_{};
    std::uint32_t passes_{};
    bool active_{};
    Counters counters_{};
};

// The USB interrupt's Ep0Order (OCRAM2, .ocram.usb_ep0_order: startup never
// writes it, so wfg_usb_ep0_order_reset() clears it before TinyUSB starts).
[[nodiscard]] Ep0Order& ep0_order() noexcept;

// Its counters, read in a critical section that masks the USB interrupt.
[[nodiscard]] Counters ep0_order_counters() noexcept;

} // namespace waveform_generator::usb_ep0_order

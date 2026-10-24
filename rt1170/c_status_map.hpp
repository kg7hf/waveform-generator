// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The owned C code's statuses (platform/wfg_status.h) as m110::Status, and back
// Guide: docs/coding-standard.md (Failure gates)
// Ownership: the C++ side of each C boundary. C++ code that calls an owned C
// function reads its status through status_of(); a C entry point that C++
// implements returns the value of its Status through c_status_of().
// Contract: one row per enum wfg_status value, in its order, with the code
// and the static message of the Status that carries it inside the firmware;
// as many rows as kWFG_StatusCount counts, so a value added to wfg_status.h
// without its row fails the build.
// The EP0 order's two rows carry usb_ep0_order.hpp's own Statuses, so the
// value of each refusal hold() can return is that Status's row; every
// success, whatever its message, is kWFG_StatusOk. The
// static_asserts below pin every row; tests/error_map_tests.cpp pins each
// row's code and message. Beside error_map.hpp, which maps what the wire
// carries: no C status reaches the wire.

#pragma once

#include "platform/wfg_status.h"
#include "usb_ep0_order.hpp"

#include "common/status.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace waveform_generator
{

// A C status beside the Status that carries it.
struct CStatus
{
    wfg_status value;
    m110::Status status;
};

inline constexpr auto c_statuses = std::to_array<CStatus>({
    {kWFG_StatusOk, m110::Status::success()},
    {kWFG_StatusInvalidArgument, {.code = m110::StatusCode::invalid_argument, .message = "INVALID_ARGUMENT"}},
    {kWFG_StatusMediaRefused, {.code = m110::StatusCode::unavailable, .message = "MEDIA_REFUSED"}},
    {kWFG_StatusMediaFault, {.code = m110::StatusCode::io_error, .message = "MEDIA_FAULT"}},
    {kWFG_StatusSdHostNotReady, {.code = m110::StatusCode::unavailable, .message = "SD_HOST_NOT_READY"}},
    {kWFG_StatusUsbClockFailed, {.code = m110::StatusCode::io_error, .message = "USB_CLOCK_FAILED"}},
    {kWFG_StatusUsbStackFailed, {.code = m110::StatusCode::io_error, .message = "USB_STACK_FAILED"}},
    {kWFG_StatusEp0OrderIdle, usb_ep0_order::not_in_interrupt},
    {kWFG_StatusEp0HoldFull, usb_ep0_order::hold_full},
});

// The Status of a value with no row: kWFG_StatusCount, which is no status, or
// a value only a cast can make.
inline constexpr m110::Status unknown_c_status{.code = m110::StatusCode::internal_error, .message = "UNKNOWN_C_STATUS"};

// Whether two Statuses are one: the same code and message.
[[nodiscard]] constexpr bool same_status(m110::Status first, m110::Status second) noexcept
{
    if (first.message == nullptr || second.message == nullptr)
    {
        return first.code == second.code && first.message == second.message;
    }

    return first.code == second.code && std::string_view{first.message} == std::string_view{second.message};
}

// The Status that carries `value`; unknown_c_status for a value with no row.
[[nodiscard]] constexpr m110::Status status_of(wfg_status value) noexcept
{
    const auto row = std::ranges::find(c_statuses, value, &CStatus::value);
    return row == c_statuses.end() ? unknown_c_status : row->status;
}

// The value whose Status `status` is: kWFG_StatusOk for every success,
// whatever its message (kWFG_StatusOk is the only one); none for a failure
// no row carries.
[[nodiscard]] constexpr std::optional<wfg_status> c_status_of(m110::Status status) noexcept
{
    if (status.is_ok())
    {
        return kWFG_StatusOk;
    }

    const auto row = std::ranges::find_if(c_statuses, [status](const CStatus& candidate) { return same_status(candidate.status, status); });

    if (row == c_statuses.end())
    {
        return std::nullopt;
    }

    return row->value;
}

// Whether the rows are the values from kWFG_StatusOk to the last before
// kWFG_StatusCount, in order, each once.
[[nodiscard]] constexpr bool c_statuses_in_order() noexcept
{
    std::underlying_type_t<wfg_status> expected{};

    for (const CStatus& row : c_statuses)
    {
        if (std::to_underlying(row.value) != expected)
        {
            return false;
        }

        ++expected;
    }

    return expected == std::to_underlying(kWFG_StatusCount);
}

// Whether kWFG_StatusOk is the only success, no two rows carry one Status,
// and each row's Status maps back to its value.
[[nodiscard]] constexpr bool c_statuses_distinct() noexcept
{
    const std::span<const CStatus, c_statuses.size()> rows{c_statuses};

    for (std::size_t index = 0U; index < rows.size(); ++index)
    {
        const CStatus& row = rows[index];
        const bool ok_only_first = row.status.is_ok() == (row.value == kWFG_StatusOk);
        const bool shared = std::ranges::any_of(rows.subspan(index + 1U), [&row](const CStatus& other) { return same_status(other.status, row.status); });

        if (!ok_only_first || shared || c_status_of(row.status) != row.value)
        {
            return false;
        }
    }

    return true;
}

static_assert(c_statuses_in_order(), "a row for every C status, from kWFG_StatusOk to the last before kWFG_StatusCount, in order");
static_assert(c_statuses_distinct(), "kWFG_StatusOk alone is a success, and each row's Status is its own and maps back to it");
static_assert(c_status_of(usb_ep0_order::not_in_interrupt) == kWFG_StatusEp0OrderIdle && c_status_of(usb_ep0_order::hold_full) == kWFG_StatusEp0HoldFull &&
                  c_status_of(m110::Status::success()) == kWFG_StatusOk,
              "each Status Ep0Order::hold() returns has its C status");

} // namespace waveform_generator

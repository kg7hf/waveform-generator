// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

#include <cstdint>
#include <expected>
#include <utility>

namespace m110
{

// The kind of failure; Status::message says the rest. As in modem-core
// (modem/common/status.hpp), a function that returns one cannot have its
// result dropped unread.
enum class [[nodiscard("check the StatusCode; a dropped error is a silent failure")]] StatusCode : std::uint8_t
{
    ok = 0,
    invalid_argument,
    invalid_configuration,
    buffer_too_small,
    unavailable,
    io_error,
    timeout,
    busy,
    internal_error
};

// The outcome of a fallible call: ok, or a code and a static message (a
// string literal: it is never copied or freed). [[nodiscard]] like StatusCode:
// a caller that does not need the outcome says so with static_cast<void>.
struct [[nodiscard("check the Status; a dropped error is a silent failure")]] Status
{
    StatusCode code{StatusCode::ok};
    const char* message{"ok"};

    [[nodiscard]] constexpr bool is_ok() const noexcept { return code == StatusCode::ok; }

    [[nodiscard]] static constexpr Status success() noexcept { return {}; }
};

// Where critical_error() was called: its return address moved back into the
// call instruction, since a call to a [[noreturn]] function can end its
// caller and the return address then lies in the next function. The call is
// in the function that called critical_error(), or that Result::value() was
// inlined into; addr2line -i names it. 0 where the compiler gives no return
// address.
struct CriticalSite
{
    std::uintptr_t call{};
};

// The site of the call that returns to `return_address`. A Thumb return
// address has the state bit (bit 0) set, and a call instruction ends at the
// return address and is 2 bytes at least (BL is 4, BLX through a register 2;
// a host's direct call 5 or more): the halfword below the return address,
// the state bit cleared, lies in the call itself. None below 2.
[[nodiscard]] constexpr CriticalSite critical_site_of(std::uintptr_t return_address) noexcept
{
    constexpr std::uintptr_t thumb_state_bit = 1U;
    constexpr std::uintptr_t call_halfword_bytes = 2U;

    if (return_address < call_halfword_bytes)
    {
        return {};
    }

    return {.call = (return_address & ~thumb_state_bit) - call_halfword_bytes};
}

// modem-core's handler takes the Status alone; this one also gets the site,
// so a record says where the failure was as well as what it was.
using CriticalErrorHandler = void (*)(Status status, CriticalSite site) noexcept;

// Install during single-threaded startup, before any Result is consumed.
// A handler may record diagnostics and return; critical_error() then traps,
// so a failed Result is never observed as a value. The host tools' handler
// (common/host_critical_error.hpp) and a host test's end the process instead.
void set_critical_error_handler(CriticalErrorHandler handler) noexcept;
[[noreturn]] void critical_error(Status status) noexcept;

// A Result built from an ok Status would have neither a value nor an error:
// it holds this error instead, so a missing value always has a reason.
inline constexpr Status ok_status_without_value{.code = StatusCode::internal_error, .message = "a Result was built from an ok Status"};

// A fallible T: a value, or the Status explaining its absence. As in
// modem-core, this is std::expected<T, Status> underneath (no default
// constructor needed for T; trivially copy-constructible and destructible when
// T is, though its assignment is not trivial), wrapped for one
// reason the standard type does not give: value() on an error goes through
// critical_error() and its handler, where std::expected::value() under
// -fno-exceptions aborts with no record of the Status.
//   return value;          -> a value
//   return Status{code,..} -> the reason there is none
template <typename T> class [[nodiscard]] Result
{
public:
    using value_type = T;
    using error_type = Status;

    constexpr Result(T value) noexcept : result_{std::move(value)} {}

    constexpr Result(Status error) noexcept : result_{std::unexpected(error.is_ok() ? ok_status_without_value : error)} {}

    [[nodiscard]] constexpr bool has_value() const noexcept { return result_.has_value(); }

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return result_.has_value(); }

    [[nodiscard]] constexpr T& value() & noexcept
    {
        if (!result_.has_value())
        {
            critical_error(result_.error());
        }

        return *result_;
    }

    [[nodiscard]] constexpr const T& value() const& noexcept
    {
        if (!result_.has_value())
        {
            critical_error(result_.error());
        }

        return *result_;
    }

    [[nodiscard]] constexpr T&& value() && noexcept
    {
        if (!result_.has_value())
        {
            critical_error(result_.error());
        }

        return *std::move(result_);
    }

    // The error, or an ok Status with the value; safe either way, unlike
    // std::expected::error(), whose precondition is !has_value().
    [[nodiscard]] constexpr Status error() const noexcept { return result_.has_value() ? Status::success() : result_.error(); }

    // The same as error(), the name this repository's callers use.
    [[nodiscard]] constexpr Status status() const noexcept { return error(); }

private:
    std::expected<T, Status> result_;
};

} // namespace m110

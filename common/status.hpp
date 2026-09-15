// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#pragma once

#include <cstdint>

namespace m110
{

enum class StatusCode : std::uint8_t
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

struct Status
{
    StatusCode code{StatusCode::ok};
    const char* message{"ok"};

    [[nodiscard]] constexpr bool is_ok() const noexcept
    {
        return code == StatusCode::ok;
    }

    [[nodiscard]] static constexpr Status success() noexcept
    {
        return {};
    }
};

using CriticalErrorHandler = void (*)(Status status) noexcept;

// Install during single-threaded startup, before any Result is consumed.
// A handler may record diagnostics, but it must return; critical_error() then
// traps so a failed Result can never be observed as a default-constructed value.
void set_critical_error_handler(CriticalErrorHandler handler) noexcept;
[[noreturn]] void critical_error(Status status) noexcept;

template <typename T> class [[nodiscard]] Result
{
public:
    constexpr Result(T value) noexcept : value_{value}, status_{} {}

    constexpr Result(Status status) noexcept : value_{}, status_{status} {}

    [[nodiscard]] constexpr bool has_value() const noexcept
    {
        return status_.is_ok();
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return has_value();
    }

    [[nodiscard]] constexpr const T& value() const noexcept
    {
        if (!has_value())
        {
            critical_error(status_);
        }

        return value_;
    }

    [[nodiscard]] constexpr T& value() noexcept
    {
        if (!has_value())
        {
            critical_error(status_);
        }

        return value_;
    }

    [[nodiscard]] constexpr Status status() const noexcept
    {
        return status_;
    }

private:
    T value_{};
    Status status_{};
};

} // namespace m110

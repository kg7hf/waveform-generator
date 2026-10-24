// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The host tools' critical-error handler: what a failed Result::value() (or
// another m110::critical_error() call) reports in a host command-line tool
// Guide: docs/coding-standard.md (Failure gates)
// Ownership: each host tool's main() installs it as its first statement,
// before anything can consume a Result; the handler then runs on whichever
// thread failed. The firmware installs its own (rt1170/critical_error.hpp).
// Contract: one line on stderr,
//   <tool>: critical error: <code> (<number>): <message>; call site 0x<site>
// with the Status's code (status_code_name()), its number and its message,
// and the call site critical_error() was given (common/status.hpp: a
// run-time address, which addr2line resolves against the image's load
// address); stdout is flushed first. Then the process ends with the tool's
// failure status through std::_Exit: no destructor or atexit handler runs,
// since the failure may have left their state broken.

#pragma once

#include "common/status.hpp"

#include <string_view>

namespace m110
{

// The StatusCode's name, as common/status.hpp spells the enumerator.
[[nodiscard]] constexpr std::string_view status_code_name(StatusCode code) noexcept
{
    switch (code)
    {
    case StatusCode::ok:
        return "ok";

    case StatusCode::invalid_argument:
        return "invalid_argument";

    case StatusCode::invalid_configuration:
        return "invalid_configuration";

    case StatusCode::buffer_too_small:
        return "buffer_too_small";

    case StatusCode::unavailable:
        return "unavailable";

    case StatusCode::io_error:
        return "io_error";

    case StatusCode::timeout:
        return "timeout";

    case StatusCode::busy:
        return "busy";

    case StatusCode::internal_error:
        return "internal_error";
    }

    return "unknown";
}

static_assert(status_code_name(StatusCode::ok) == "ok" && status_code_name(StatusCode::internal_error) == "internal_error");

// Installs the handler for the tool named `tool`, which then ends with
// `exit_status`, the tool's documented failure status. `tool` names static
// storage (a string literal). Caller: main(), first, while one thread runs.
void install_host_critical_error_handler(std::string_view tool, int exit_status) noexcept;

} // namespace m110

// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "common/host_critical_error.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <memory>
#include <string_view>
#include <system_error>
#include <utility>

namespace m110
{
namespace
{

constexpr int decimal_base = 10;
constexpr int hexadecimal_base = 16;
// Room for a 64-bit value in decimal or in hexadecimal.
constexpr std::size_t digit_capacity = 24U;

// The tool install_host_critical_error_handler() named; a generic one until then.
struct HostTool
{
    std::string_view name{"host tool"};
    int exit_status{EXIT_FAILURE};
};

[[nodiscard]] HostTool& installed_tool() noexcept
{
    static constinit HostTool tool{};
    return tool;
}

// One part of the report; a failed write of it has nowhere to go.
void put(std::string_view text) noexcept
{
    static_cast<void>(std::fwrite(text.data(), 1U, text.size(), stderr));
}

// `value` in `base`, in `digits`.
[[nodiscard]] constexpr std::string_view as_text(std::array<char, digit_capacity>& digits, std::uintmax_t value, int base) noexcept
{
    char* const first = std::to_address(digits.begin());
    const std::to_chars_result written = std::to_chars(first, std::to_address(digits.end()), value, base);

    if (written.ec != std::errc{})
    {
        return "?";
    }

    return {first, static_cast<std::size_t>(std::distance(first, written.ptr))};
}

[[nodiscard]] constexpr bool as_text_is(std::uintmax_t value, int base, std::string_view expected) noexcept
{
    std::array<char, digit_capacity> digits{};
    return as_text(digits, value, base) == expected;
}

// Known answers: the report's number is decimal and its site hexadecimal, in
// lower case, with no prefix of their own; the widest site fits.
constexpr std::uintmax_t sample_number = 1234U;
constexpr std::uintmax_t sample_site = 0xBEEFU;
static_assert(as_text_is(sample_number, decimal_base, "1234") && as_text_is(sample_site, hexadecimal_base, "beef"));
static_assert(as_text_is(std::numeric_limits<std::uintmax_t>::max(), hexadecimal_base, "ffffffffffffffff"));

// The handler (common/host_critical_error.hpp): the report line, then the
// tool's failure status.
void report_and_exit(Status status, CriticalSite site) noexcept
{
    const HostTool& tool = installed_tool();
    std::array<char, digit_capacity> number{};
    std::array<char, digit_capacity> address{};
    static_cast<void>(std::fflush(stdout));
    put(tool.name);
    put(": critical error: ");
    put(status_code_name(status.code));
    put(" (");
    put(as_text(number, std::to_underlying(status.code), decimal_base));
    put("): ");
    put(status.message != nullptr ? std::string_view{status.message} : std::string_view{"(no message)"});
    put("; call site 0x");
    put(as_text(address, site.call, hexadecimal_base));
    put("\n");
    static_cast<void>(std::fflush(stderr));
    std::_Exit(tool.exit_status);
}

} // namespace

void install_host_critical_error_handler(std::string_view tool, int exit_status) noexcept
{
    installed_tool() = HostTool{.name = tool, .exit_status = exit_status};
    set_critical_error_handler(&report_and_exit);
}

} // namespace m110

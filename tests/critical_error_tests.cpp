// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The critical-error path. Result::value() of an error reaches the installed
// handler through m110::critical_error() (common/status.cpp) with its Status
// and a call site, and the firmware's handler (rt1170/critical_error.cpp)
// masks interrupts and records them, written back to memory; installing it
// clears the record. This file stands in for the platform's side
// (platform/board.hpp) and notes each call.
//
// A handler that returns lets critical_error() trap, which a test process
// does not survive. So each --value-of-error run installs a test handler that
// checks what it was given, passes it to the firmware's handler, checks the
// record and ends the process: exit 0 when all of that holds. The "direct"
// run also checks that the site lies in the function that called
// critical_error(). The "installed" run installs the firmware's handler as
// every image does (install_critical_error_handler()): the platform's
// write-back of the record ends the process, and without the handler the
// process traps.

#include "common/status.hpp"
#include "critical_error.hpp"
#include "platform/board.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace
{
namespace wg = waveform_generator;

class Checks
{
public:
    void expect(bool condition, const char* what) noexcept
    {
        if (!condition)
        {
            ++failures_;
            std::fputs("FAIL: ", stdout);
            std::fputs(what, stdout);
            std::fputs("\n", stdout);
        }
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

private:
    int failures_{};
};

// What the firmware's handler asked of the platform.
struct PlatformCalls
{
    bool interrupts_masked{};
    // The bytes last written back, when they were one record.
    std::optional<wg::CriticalErrorRecord> written_back{};
    // The "installed" run: a record written back ends the process.
    bool exit_on_record{};
};

[[nodiscard]] PlatformCalls& platform_calls() noexcept
{
    static PlatformCalls calls;
    return calls;
}

constexpr m110::Status raised{.code = m110::StatusCode::io_error, .message = "the test's critical error"};
// A call site for the checks that give one: any nonzero address does.
constexpr std::uintptr_t given_site = 0x1234U;

// Exit codes of a --value-of-error run.
constexpr int handled = 0;        // the handler got `raised` and a site, and the firmware recorded them
constexpr int value_returned = 1; // value() of the error returned
constexpr int mishandled = 2;     // the handler got something else, or the record differs
constexpr int bad_arguments = 3;

[[nodiscard]] constexpr bool records(const wg::CriticalErrorRecord& record, m110::Status status, std::uintptr_t site) noexcept
{
    return record.signature == wg::critical_error_signature && record.code == status.code && record.message == status.message && record.site == site;
}

[[nodiscard]] constexpr bool empty(const wg::CriticalErrorRecord& record) noexcept
{
    return record.signature == 0U && record.code == m110::StatusCode::ok && record.message == nullptr && record.site == 0U;
}

static_assert(records(wg::critical_error_record_of(raised, m110::CriticalSite{.call = given_site}), raised, given_site), "a record holds the Status's code, its message and the site");
static_assert(empty(wg::CriticalErrorRecord{}) && wg::critical_error_signature != 0U, "an empty record holds no critical error");

// The signature as docs/modules/rt1170-player-and-audio.md gives it to a
// debugger user: word 0 of the record reads 0x54495243, and its bytes in
// memory, little-endian on the core as on this host, spell "CRIT".
constexpr std::uint32_t documented_signature = 0x54495243U;
constexpr std::array<char, sizeof(std::uint32_t)> signature_text{'C', 'R', 'I', 'T'};
static_assert(std::endian::native == std::endian::little, "this host stores a word little-endian, as the Cortex-M7 does");
static_assert(wg::critical_error_signature == documented_signature, "the record's signature is the word the guide documents");
static_assert(std::bit_cast<std::array<char, sizeof(std::uint32_t)>>(wg::critical_error_signature) == signature_text, "the signature's bytes in memory spell CRIT");

// critical_site_of(): the site lies inside the call that returns to the
// address, for each call the comment names, at a call that starts at 0x1000.
constexpr std::uintptr_t call_at = 0x1000U;

constexpr std::uintptr_t thumb_state = 1U;
constexpr std::uintptr_t no_state = 0U;

// Whether the site of a call of `call_bytes` at call_at, whose return
// address carries `state_bit`, lies in that call.
[[nodiscard]] constexpr bool site_inside_call(std::uintptr_t call_bytes, std::uintptr_t state_bit) noexcept
{
    const std::uintptr_t site = m110::critical_site_of((call_at + call_bytes) | state_bit).call;
    return site >= call_at && site < call_at + call_bytes;
}

constexpr std::uintptr_t bl_bytes = 4U;
constexpr std::uintptr_t blx_register_bytes = 2U;
constexpr std::uintptr_t host_direct_call_bytes = 5U;
constexpr std::uintptr_t host_memory_call_bytes = 6U;

static_assert(site_inside_call(bl_bytes, thumb_state) && site_inside_call(blx_register_bytes, thumb_state), "a Thumb BL's or BLX's site, its return address with the state bit set, lies in the call");
static_assert(site_inside_call(host_direct_call_bytes, no_state) && site_inside_call(host_memory_call_bytes, no_state), "a host call's site lies in the call");
// The halfword below the return address, the state bit cleared: a BL's
// second halfword, and the third byte of a 5-byte host call (its return
// address is odd, so clearing bit 0 moves it one byte down first).
constexpr std::uintptr_t halfword_bytes = 2U;
static_assert(m110::critical_site_of((call_at + bl_bytes) | thumb_state).call == call_at + halfword_bytes && m110::critical_site_of(call_at + host_direct_call_bytes).call == call_at + halfword_bytes,
              "the site is the halfword below the return address, the state bit cleared");
static_assert(m110::critical_site_of(0U).call == 0U && m110::critical_site_of(1U).call == 0U && m110::critical_site_of(2U).call == 0U, "an address below a call's first halfword gives no site");

// The direct run's call: critical_error() from a function whose body is
// that call, so its site lies within the function's first bytes.
constexpr std::uintptr_t max_call_offset_bytes = 64U;

[[noreturn, gnu::noinline]] void raise_directly() noexcept
{
    m110::critical_error(raised);
}

// The direct run's function, or none.
[[nodiscard]] std::optional<std::uintptr_t>& site_function() noexcept
{
    static std::optional<std::uintptr_t> start;
    return start;
}

// The site lies in the function that called critical_error(), when the run
// says which; any nonzero site otherwise.
[[nodiscard]] bool site_as_expected(m110::CriticalSite site) noexcept
{
    const std::optional<std::uintptr_t>& start = site_function();
    return site.call != 0U && (!start.has_value() || (site.call >= *start && site.call - *start < max_call_offset_bytes));
}

// The firmware's handler masked interrupts, and the record it holds is the
// one written back to memory.
[[nodiscard]] bool recorded(m110::Status status, std::uintptr_t site) noexcept
{
    const PlatformCalls& calls = platform_calls();
    return calls.interrupts_masked && records(wg::critical_error_record(), status, site) && calls.written_back.has_value() && records(*calls.written_back, status, site);
}

void test_handler(m110::Status status, m110::CriticalSite site) noexcept
{
    const bool as_raised = status.code == raised.code && status.message == raised.message && site_as_expected(site);
    wg::record_critical_error(status, site);
    const bool handled_right = as_raised && recorded(raised, site.call);
    std::fputs(handled_right ? "critical error handled\n" : "critical error mishandled\n", stdout);
    static_cast<void>(std::fflush(stdout));
    std::_Exit(handled_right ? handled : mishandled);
}

[[nodiscard]] m110::Result<int> failed() noexcept
{
    return raised;
}

// value() of a failed Result, called on an lvalue, a const lvalue or an
// rvalue; critical_error() called directly from raise_directly(); or value()
// of a failed Result once the firmware's handler is installed, as every
// image installs it. Returns only if value() does.
[[nodiscard]] int value_of_error(std::string_view form) noexcept
{
    if (form == "installed")
    {
        platform_calls().exit_on_record = true;
        wg::install_critical_error_handler();
        m110::Result<int> result = failed();
        static_cast<void>(result.value());
        std::fputs("value() of an error returned\n", stdout);
        return value_returned;
    }

    m110::set_critical_error_handler(&test_handler);

    if (form == "direct")
    {
        site_function() = std::bit_cast<std::uintptr_t>(&raise_directly);
        raise_directly();
    }

    if (form == "lvalue")
    {
        m110::Result<int> result = failed();
        static_cast<void>(result.value());
    }
    else if (form == "const")
    {
        const m110::Result<int> result = failed();
        static_cast<void>(result.value());
    }
    else if (form == "rvalue")
    {
        static_cast<void>(failed().value());
    }
    else
    {
        return bad_arguments;
    }

    std::fputs("value() of an error returned\n", stdout);
    return value_returned;
}

// The firmware's handler records a critical error; installing it clears the
// record, through memory too, and masks nothing.
void test_record(Checks& checks)
{
    wg::record_critical_error(raised, m110::CriticalSite{.call = given_site});
    checks.expect(recorded(raised, given_site), "the firmware's handler masks interrupts and records the code, the message and the site");
    platform_calls() = PlatformCalls{};
    wg::install_critical_error_handler();
    const PlatformCalls& calls = platform_calls();
    checks.expect(empty(wg::critical_error_record()) && calls.written_back.has_value() && empty(*calls.written_back), "installing the handler clears the record");
    checks.expect(!calls.interrupts_masked, "installing the handler leaves interrupts alone");
    // The record keeps the first critical error of the boot: a later one, from
    // a context PRIMASK does not mask (an NMI, a fault handler), leaves it.
    constexpr m110::Status later{.code = m110::StatusCode::internal_error, .message = "a later critical error"};
    constexpr std::uintptr_t later_site = 0x5678U;
    wg::record_critical_error(raised, m110::CriticalSite{.call = given_site});
    wg::record_critical_error(later, m110::CriticalSite{.call = later_site});
    checks.expect(recorded(raised, given_site), "a later critical error leaves the first one recorded, in memory too");
    wg::install_critical_error_handler();
    checks.expect(empty(wg::critical_error_record()), "installing the handler at the next boot clears it");
}

} // namespace

// The platform's side of the firmware's handler, which rt1170/platform/board.cpp
// implements on the device.
void m110::platform_mask_interrupts() noexcept
{
    platform_calls().interrupts_masked = true;
}

void m110::platform_write_back(std::span<std::byte> bytes) noexcept
{
    std::array<std::byte, sizeof(wg::CriticalErrorRecord)> copy{};

    if (bytes.size() != copy.size())
    {
        platform_calls().written_back.reset();
        return;
    }

    std::ranges::copy(bytes, copy.begin());
    PlatformCalls& calls = platform_calls();
    calls.written_back = std::bit_cast<wg::CriticalErrorRecord>(copy);

    // The "installed" run: the installed handler wrote a record back, with
    // interrupts masked; installing it wrote back an empty one.
    if (calls.exit_on_record && calls.written_back->signature == wg::critical_error_signature)
    {
        const wg::CriticalErrorRecord& record = *calls.written_back;
        const bool handled_right = calls.interrupts_masked && records(record, raised, record.site) && record.site != 0U && records(wg::critical_error_record(), raised, record.site);
        std::fputs(handled_right ? "critical error recorded by the installed handler\n" : "critical error misrecorded\n", stdout);
        static_cast<void>(std::fflush(stdout));
        std::_Exit(handled_right ? handled : mishandled);
    }
}

int main(int argc, char** argv)
{
    const std::span<char* const> args{argv, static_cast<std::size_t>(argc)};
    constexpr std::string_view value_of_error_flag{"--value-of-error"};
    constexpr std::size_t value_of_error_args = 3U;

    if (args.size() == value_of_error_args && std::string_view{args[1]} == value_of_error_flag)
    {
        return value_of_error(args[2]);
    }

    if (args.size() != 1U)
    {
        std::fputs("usage: wfg_critical_error_tests [--value-of-error lvalue|const|rvalue|direct|installed]\n", stderr);
        return bad_arguments;
    }

    Checks checks;
    test_record(checks);

    if (checks.failures() != 0)
    {
        std::fputs(("critical error: " + std::to_string(checks.failures()) + " check(s) failed\n").c_str(), stdout);
        return 1;
    }

    std::puts("critical error: all checks passed");
    return 0;
}

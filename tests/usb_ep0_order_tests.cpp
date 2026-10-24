// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The USB interrupt's EP0 order (rt1170/usb_ep0_order.hpp, #32): the order
// itself, the glue's routing of the driver's events (usb/usb_ep0_order_glue.c),
// and the race on the pinned TinyUSB's device stack and CDC driver
// (usb_ep0_race_stubs): reported as the ChipIdea driver reports it, an
// IN-data request after an IN status stage (and an OUT-data request after an
// OUT one) trips the pair of assertions window 3 recorded and loses the first
// transfer's last stage; through the EP0 order, neither happens, also when
// the pass that reports the SETUP ends with a SOF. With
// --release-without-setup (wfg-usb-ep0-order-release-without-setup) it
// checks that a null setup given to wfg_usb_ep0_order_release() is a
// critical error, never a "none held".

#include "usb/usb_ep0_order.h"
#include "usb_ep0_order.hpp"
#include "usb_ep0_race_stubs/usb_ep0_race.h"

#include "common/status.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>

namespace
{
namespace ep0 = waveform_generator::usb_ep0_order;

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

// The bit rate the race's SET_LINE_CODING carries (usb_ep0_race.c).
constexpr std::uint32_t host_bit_rate = 921600U;

// A SETUP whose packet tells the held ones apart (the order never reads it).
[[nodiscard]] constexpr ep0::Setup setup_with(std::uint64_t packet) noexcept
{
    return {.rhport = 0U, .packet = packet};
}

// Whether `status` is `expected`, by its code.
[[nodiscard]] constexpr bool is(const m110::Status& status, const m110::Status& expected) noexcept
{
    return std::string_view{status.message} == std::string_view{expected.message};
}

// The packet of the SETUP the order releases next, or 0 when none is held.
[[nodiscard]] constexpr std::uint64_t released_packet(ep0::Ep0Order& order) noexcept
{
    const auto setup = order.release();
    return setup.has_value() ? setup->packet : std::uint64_t{0U};
}

// The order's rules, worked through at compile time: SETUPs are held oldest
// first, an EP0 completion counts as reordered only while one is held, a
// pending completion earns at most max_extra_passes more passes, and a SETUP
// is not held outside an interrupt or past a full hold, which counts as
// unordered as the pass bound does.
[[nodiscard]] consteval bool order_holds()
{
    constexpr std::uint64_t first = 1U;
    constexpr std::uint64_t second = 2U;
    constexpr std::uint64_t third = 3U;
    constexpr std::uint64_t fourth = 4U;
    constexpr std::uint64_t fifth = 5U;
    constexpr std::uint64_t refused = 6U;
    constexpr auto pending = ep0::Ep0Pending::completion;
    ep0::Ep0Order order{};
    bool good = is(order.hold(setup_with(first)), ep0::not_in_interrupt) && order.another_pass(pending) == ep0::NextPass::done;
    order.begin();
    order.completion();
    good = good && order.counters().reordered == 0U;
    good = good && order.hold(setup_with(first)).is_ok() && order.hold(setup_with(second)).is_ok() && order.held() == 2U;
    order.completion();
    good = good && order.counters().reordered == 1U;
    good = good && order.another_pass(pending) == ep0::NextPass::again && order.another_pass(pending) == ep0::NextPass::again && order.another_pass(pending) == ep0::NextPass::done &&
           order.counters().unordered == 1U;
    good = good && released_packet(order) == first && released_packet(order) == second && released_packet(order) == 0U;
    good = good && order.another_pass(pending) == ep0::NextPass::done;
    order.end();
    good = good && is(order.hold(setup_with(third)), ep0::not_in_interrupt) && order.counters().unordered == 1U;
    order.begin();
    good = good && order.hold(setup_with(third)).is_ok() && order.hold(setup_with(fourth)).is_ok() && order.hold(setup_with(fifth)).is_ok() && is(order.hold(setup_with(refused)), ep0::hold_full) &&
           order.counters().unordered == 2U;
    good = good && order.held() == ep0::hold_capacity && order.another_pass(ep0::Ep0Pending::none) == ep0::NextPass::done;
    return good && released_packet(order) == third;
}

// A release for another event counts as unordered only while a SETUP is held
// and an EP0 completion is pending, once per release, and only inside an
// interrupt.
[[nodiscard]] consteval bool early_release_counts()
{
    constexpr std::uint64_t held = 1U;
    constexpr auto pending = ep0::Ep0Pending::completion;
    ep0::Ep0Order order{};
    order.releasing(pending);
    order.begin();
    order.releasing(pending);
    bool good = order.counters().unordered == 0U;
    good = good && order.hold(setup_with(held)).is_ok();
    order.releasing(ep0::Ep0Pending::none);
    good = good && order.counters().unordered == 0U;
    order.releasing(pending);
    good = good && order.counters().unordered == 1U && released_packet(order) == held;
    order.releasing(pending);
    order.end();
    return good && order.counters().unordered == 1U && order.counters().reordered == 0U;
}

static_assert(early_release_counts());

static_assert(order_holds());
static_assert(ep0::hold_capacity == ep0::setups_per_pass * (1U + ep0::max_extra_passes), "one SETUP per pass, and no more room than that");

void test_c_entry_points(Checks& checks)
{
    constexpr std::uint64_t packet = 0x0007'0000'0003'2221U;
    wfg_usb_ep0_order_reset();
    const wfg_usb_setup setup{.rhport = 1U, .packet = packet};
    checks.expect(wfg_usb_ep0_order_hold(&setup) == kWFG_StatusEp0OrderIdle, "no SETUP is held outside an interrupt");
    wfg_usb_ep0_order_begin();
    checks.expect(wfg_usb_ep0_order_hold(&setup) == kWFG_StatusOk && wfg_usb_ep0_order_hold(nullptr) == kWFG_StatusInvalidArgument, "the interrupt holds a SETUP; a null one is an invalid argument");
    wfg_usb_ep0_order_completion();
    checks.expect(wfg_usb_ep0_order_another_pass(true) && !wfg_usb_ep0_order_another_pass(false), "a pending completion earns a pass");
    checks.expect(wfg_usb_ep0_order_holding(), "the SETUP is still held");
    wfg_usb_ep0_order_releasing(false);
    wfg_usb_ep0_order_releasing(true);
    wfg_usb_setup out{};
    const bool released = wfg_usb_ep0_order_release(&out);
    checks.expect(released && out.rhport == 1U && out.packet == packet, "the released SETUP is the held one, byte for byte");
    checks.expect(!wfg_usb_ep0_order_release(&out) && !wfg_usb_ep0_order_holding(), "then none is held");
    wfg_usb_ep0_order_end();
    const auto counters = ep0::ep0_order_counters();
    checks.expect(counters.reordered == 1U && counters.unordered == 1U, "the counters USB? reports: the release with a completion pending is unordered");
    wfg_usb_ep0_order_reset();
    checks.expect(ep0::ep0_order_counters().reordered == 0U, "reset clears them");
}

// Through the C entry points, a SETUP past a full hold is not held and counts
// as unordered; the held ones are released oldest first, and the refused one
// was never among them.
void test_c_hold_full(Checks& checks)
{
    constexpr std::uint64_t refused_packet = 0xFFU;
    wfg_usb_ep0_order_reset();
    wfg_usb_ep0_order_begin();
    bool all_held = true;

    for (std::uint64_t packet = 1U; packet <= ep0::hold_capacity; ++packet)
    {
        const wfg_usb_setup setup{.rhport = 0U, .packet = packet};
        all_held = all_held && wfg_usb_ep0_order_hold(&setup) == kWFG_StatusOk;
    }

    const wfg_usb_setup refused{.rhport = 0U, .packet = refused_packet};
    checks.expect(all_held && wfg_usb_ep0_order_hold(&refused) == kWFG_StatusEp0HoldFull && ep0::ep0_order_counters().unordered == 1U,
                  "a SETUP past a full hold is not held (the hold is full), and counts as unordered");
    wfg_usb_setup out{};
    bool oldest_first = true;

    for (std::uint64_t packet = 1U; packet <= ep0::hold_capacity; ++packet)
    {
        oldest_first = oldest_first && wfg_usb_ep0_order_release(&out) && out.packet == packet;
    }

    checks.expect(oldest_first && !wfg_usb_ep0_order_release(&out), "the held SETUPs go oldest first, and the refused one is not among them");
    wfg_usb_ep0_order_end();
    wfg_usb_ep0_order_reset();
}

// One interrupt's events through the glue: an EP0 completion overtakes the
// held SETUP, and so does a SOF; the audio IN completion and the bus reset
// each release the SETUPs before them first, and the reset, which comes
// while an EP0 completion is pending, counts that release as unordered; the
// end of the interrupt releases the rest. An event from task context passes
// straight through, and outside the interrupt nothing is held.
void test_glue_routing(Checks& checks)
{
    constexpr std::size_t text_capacity = 128U;
    wfg_usb_ep0_order_reset();
    std::array<char, text_capacity> text{};
    wfg_race_route_sample(text.data(), text.size());
    checks.expect(std::string_view{text.data()} == "C80 S21 C83 S0B R F c00 C00 S06 S21", "EP0 completions and a SOF overtake the held SETUPs; every other event keeps its place after them");
    const auto counters = ep0::ep0_order_counters();
    checks.expect(counters.reordered == 2U && counters.unordered == 1U, "two completions went ahead of a held SETUP, and the release for the reset with a completion pending is unordered");
}

// A SETUP past a full hold through the glue: the held ones go on first, in
// order, then that one, and the order counts it as unordered.
void test_glue_full_hold(Checks& checks)
{
    constexpr std::size_t text_capacity = 64U;
    wfg_usb_ep0_order_reset();
    std::array<char, text_capacity> text{};
    wfg_race_route_full_hold(text.data(), text.size());
    checks.expect(std::string_view{text.data()} == "S01 S02 S03 S04", "past a full hold the glue releases the held SETUPs, then passes the next one on after them");
    checks.expect(ep0::ep0_order_counters().unordered == 1U, "and the order counts that SETUP as unordered");
    wfg_usb_ep0_order_reset();
}

// The race on the pinned TinyUSB: the first transfer's last stage and the
// next transfer's SETUP, reported as the ChipIdea driver reports them, then
// through the EP0 order.
void test_race_on_tinyusb(Checks& checks)
{
    constexpr std::uint32_t asserts_of_the_pair = 2U;
    constexpr std::uint32_t ep0_transfers = 3U;
    checks.expect(wfg_race_enumerate(), "TinyUSB enumerates the CDC function, DTR on, without an assertion");
    const auto before = ep0::ep0_order_counters().reordered;
    const auto as_pinned = wfg_race_in_data(false);
    checks.expect(as_pinned.asserts == asserts_of_the_pair, "GET_LINE_CODING after DTR off, the status ZLP reported after the SETUP: its data stage ends twice and the second "
                                                            "status stage asserts twice (usbd.c 1587 within 941)");
    checks.expect(as_pinned.line_state_calls == 0U && as_pinned.dtr, "and DTR off never reaches the CDC driver: its ACK stage was taken as data");
    const auto ordered = wfg_race_in_data(true);
    checks.expect(ordered.asserts == 0U, "through the EP0 order the same bus sequence asserts nothing");
    checks.expect(ordered.line_state_calls == 1U && !ordered.dtr, "and DTR off reaches the CDC driver");
    checks.expect(ep0::ep0_order_counters().reordered == before + 1U, "the completion went ahead of the SETUP once");
    checks.expect(ordered.ep0_primes == ep0_transfers && as_pinned.ep0_primes == ep0_transfers,
                  "both queue three EP0 transfers (the first's status, the next's data and its status); as pinned the second status "
                  "is refused by the busy check before it reaches the driver");
    const auto out_as_pinned = wfg_race_out_data(false);
    checks.expect(out_as_pinned.asserts == asserts_of_the_pair, "SET_LINE_CODING after GET_LINE_CODING's status, reported late: the same pair");
    const auto out_ordered = wfg_race_out_data(true);
    checks.expect(out_ordered.asserts == 0U && out_ordered.line_coding_calls == 1U && out_ordered.line_coding_bitrate == host_bit_rate,
                  "through the EP0 order: no assertion, and the host's line coding arrives once");
    // SOF interrupts on: the pass that reports the SETUP ends with a SOF. It
    // passes on without releasing the SETUP, which still waits for the
    // completion the extra pass reports.
    const auto counted = ep0::ep0_order_counters();
    const auto with_sof = wfg_race_in_data_with_sof();
    checks.expect(with_sof.asserts == 0U && with_sof.line_state_calls == 1U && !with_sof.dtr, "with a SOF after the SETUP the race stays ordered: no assertion, and DTR off reaches the CDC driver");
    checks.expect(ep0::ep0_order_counters().reordered == counted.reordered + 1U && ep0::ep0_order_counters().unordered == counted.unordered,
                  "the completion went ahead of the SETUP, and nothing was let go early");
}

// The --release-without-setup run. A null setup is the caller's defect,
// not a poll's answer: with a SETUP held, wfg_usb_ep0_order_release(nullptr)
// reaches m110::critical_error() with EP0_RELEASE_WITHOUT_SETUP and never
// returns false, which a caller would take for "none held". The test handler
// checks the Status and that the SETUP is still held, and ends the process:
// exit 0 when both hold. A return from the release fails the run.
void release_handler(m110::Status status, m110::CriticalSite /*site*/) noexcept
{
    const bool as_expected = is(status, ep0::release_without_setup) && status.code == ep0::release_without_setup.code && wfg_usb_ep0_order_holding();
    std::fputs(as_expected ? "critical error: EP0_RELEASE_WITHOUT_SETUP, the SETUP still held\n" : "critical error: not as expected\n", stdout);
    static_cast<void>(std::fflush(stdout));
    std::_Exit(as_expected ? EXIT_SUCCESS : EXIT_FAILURE);
}

[[nodiscard]] int release_without_setup() noexcept
{
    constexpr std::uint64_t packet = 0x0007'0000'0003'2221U;
    m110::set_critical_error_handler(&release_handler);
    wfg_usb_ep0_order_reset();
    wfg_usb_ep0_order_begin();
    const wfg_usb_setup setup{.rhport = 0U, .packet = packet};

    if (wfg_usb_ep0_order_hold(&setup) != kWFG_StatusOk)
    {
        std::fputs("the SETUP was not held\n", stdout);
        return EXIT_FAILURE;
    }

    const bool released = wfg_usb_ep0_order_release(nullptr);
    std::fputs(released ? "release(nullptr) returned true\n" : "release(nullptr) returned false\n", stdout);
    return EXIT_FAILURE;
}

} // namespace

int main(int argc, char** argv)
{
    const std::span<char* const> args{argv, static_cast<std::size_t>(argc)};
    constexpr std::string_view release_without_setup_flag{"--release-without-setup"};

    if (args.size() == 2U && std::string_view{args[1]} == release_without_setup_flag)
    {
        return release_without_setup();
    }

    if (args.size() != 1U)
    {
        std::fputs("usage: wfg_usb_ep0_order_tests [--release-without-setup]\n", stderr);
        return EXIT_FAILURE;
    }

    Checks checks;
    test_c_entry_points(checks);
    test_c_hold_full(checks);
    test_glue_routing(checks);
    test_glue_full_hold(checks);
    test_race_on_tinyusb(checks);

    if (checks.failures() != 0)
    {
        std::fputs("USB EP0 order tests: FAIL\n", stdout);
        return 1;
    }

    std::fputs("USB EP0 order tests: PASS\n", stdout);
    return 0;
}

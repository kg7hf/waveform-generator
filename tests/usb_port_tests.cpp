// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The TinyUSB port's bring-up in rt1170/usb/usb_tinyusb_port.c on the host,
// built as the player image builds it (the pinned TinyUSB's headers, the
// player's TinyUSB configuration): the USB hardware's start, which
// start_usb_service() runs before the USB task, and TinyUSB's start, which
// the USB task runs first. Each outcome, the calls in their order, the USB
// PHY's registers and the debugger-visible bring-up status
// (g_m110_usb_port_status). The C stand-ins in usb_port_stubs/ replace the
// clocks, the registers and TinyUSB's device stack; the interrupt paths
// (the audio watchdog, the EP0 order's passes) remain wfg-usb-ep0-order's
// and the bench's.
// Guide: docs/modules/rt1170-player-and-audio.md

#include "usb/usb_tinyusb_port.h"
#include "usb_port_host.h"

#include <cstdint>
#include <cstdio>
#include <string_view>

namespace
{
// The port's bring-up status values (usb_tinyusb_port.c), stable evidence
// codes a debugger reads.
constexpr std::uint32_t port_hardware_ready = 0x55534201U;
constexpr std::uint32_t port_stack_initialized = 0x55534203U;
constexpr std::uint32_t port_controller_failed = 0x555342E2U;
constexpr std::uint32_t port_stack_failed = 0x555342E3U;

// The USB PHY's registers at reset (usb_port_host.c, from the RT1176
// reference manual), and what the port's start leaves: UTMI levels 2 and 3
// enabled, every power-down bit cleared, and the transmitter's calibration
// (D_CAL 0x0C, TXCAL45DP and TXCAL45DM 0x06) over the rest of TX as it was.
constexpr std::uint32_t phy_ctrl_at_reset = 0xC0200000U;
constexpr std::uint32_t phy_pwd_at_reset = 0x001E1C00U;
constexpr std::uint32_t phy_tx_at_reset = 0x0A000402U;
constexpr std::uint32_t utmi_levels_2_and_3 = 0x4000U | 0x8000U;
constexpr std::uint32_t tx_calibration_fields = 0xFU | 0xF00U | 0xF0000U;
constexpr std::uint32_t tx_calibration = 0x0CU | (0x06U << 8U) | (0x06U << 16U);
constexpr std::uint32_t phy_ctrl_started = phy_ctrl_at_reset | utmi_levels_2_and_3;
constexpr std::uint32_t phy_tx_started = (phy_tx_at_reset & ~tx_calibration_fields) | tx_calibration;

// Both clocks from the 24 MHz crystal, then the USB interrupt (136) at the
// highest priority that may call FreeRTOS (5); TinyUSB on root port 0 as a
// high-speed device (TinyUSB's TUSB_ROLE_DEVICE 1, TUSB_SPEED_HIGH 2), after
// the EP0 order's reset, with the status at controller_configured (0x55534202).
constexpr std::string_view phy_pll_call = "phy_pll(0,24000000)";
constexpr std::string_view clocks_calls = "phy_pll(0,24000000) usb_clock(0,24000000)";
constexpr std::string_view hardware_calls = "phy_pll(0,24000000) usb_clock(0,24000000) nvic(136,5)";
constexpr std::string_view stack_calls = "ep0_reset tusb_init(0,1,2;55534202)";

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

[[nodiscard]] bool calls_were(std::string_view expected) noexcept
{
    return std::string_view{wfg_test_usb_port_calls()} == expected;
}

[[nodiscard]] bool phy_untouched() noexcept
{
    return wfg_test_usb_phy_ctrl() == phy_ctrl_at_reset && wfg_test_usb_phy_pwd() == phy_pwd_at_reset && wfg_test_usb_phy_tx() == phy_tx_at_reset;
}

// A clock that does not start fails the hardware's start before the PHY is
// touched or the interrupt's priority set: the PHY's PLL before the USB
// clock is asked, then the USB clock.
void test_hardware_failures(Checks& checks)
{
    wfg_test_reset_usb_port();
    wfg_test_fail_usb_phy_pll(true);
    checks.expect(m110_usb_hardware_initialize() == kWFG_StatusUsbClockFailed && wfg_test_usb_port_status() == port_controller_failed && calls_were(phy_pll_call) && phy_untouched(),
                  "a PHY PLL that does not start fails the hardware's start: controller_failed, nothing else asked or written");
    wfg_test_fail_usb_phy_pll(false);
    wfg_test_reset_usb_port();
    wfg_test_fail_usb_clock(true);
    checks.expect(m110_usb_hardware_initialize() == kWFG_StatusUsbClockFailed && wfg_test_usb_port_status() == port_controller_failed && calls_were(clocks_calls) && phy_untouched(),
                  "a USB clock that does not start fails it the same way, after the PHY PLL");
    wfg_test_fail_usb_clock(false);
}

// Both clocks running: the PHY is powered and calibrated, the interrupt's
// priority set, and the status says the hardware is ready.
void test_hardware_ready(Checks& checks)
{
    wfg_test_reset_usb_port();
    checks.expect(m110_usb_hardware_initialize() == kWFG_StatusOk && wfg_test_usb_port_status() == port_hardware_ready && calls_were(hardware_calls),
                  "the hardware's start runs both clocks, then sets the USB interrupt's priority: hardware_ready");
    checks.expect(wfg_test_usb_phy_ctrl() == phy_ctrl_started && wfg_test_usb_phy_pwd() == 0U && wfg_test_usb_phy_tx() == phy_tx_started,
                  "and leaves the PHY with UTMI levels 2 and 3, powered up and calibrated");
}

// TinyUSB's start resets the EP0 order first; a start TinyUSB refuses leaves
// stack_failed, one it takes stack_initialized.
void test_stack(Checks& checks)
{
    wfg_test_reset_usb_port();
    wfg_test_fail_tinyusb_start(true);
    checks.expect(m110_tinyusb_stack_initialize() == kWFG_StatusUsbStackFailed && wfg_test_usb_port_status() == port_stack_failed && calls_were(stack_calls),
                  "TinyUSB refusing its start fails the stack's start, after the EP0 order's reset: stack_failed");
    wfg_test_fail_tinyusb_start(false);
    wfg_test_reset_usb_port();
    checks.expect(m110_tinyusb_stack_initialize() == kWFG_StatusOk && wfg_test_usb_port_status() == port_stack_initialized && calls_were(stack_calls),
                  "TinyUSB taking its start as a high-speed device on port 0: stack_initialized");
}
} // namespace

int main()
{
    Checks checks;
    test_hardware_failures(checks);
    test_hardware_ready(checks);
    test_stack(checks);

    if (checks.failures() != 0)
    {
        std::fputs("USB port tests: FAIL\n", stdout);
        return 1;
    }

    std::fputs("USB port tests: PASS\n", stdout);
    return 0;
}

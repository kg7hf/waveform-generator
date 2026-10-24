// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The EVK microSD socket's card detect in rt1170/platform/waveform_sd.c on the
// host, built with the player image's SD definitions (the card detect by GPIO,
// always present): the detect WFG_SD_Config() sets up, and every answer
// WFG_SD_ReadHostDetectStatus() gives, which waveform_msc.c's initialisation
// and the SD-only image's probe read before card init. The C stand-ins in
// sd_socket_stubs/ replace NXP's clock, GPIO, IOMUXC and SD host drivers.
// Guide: docs/modules/protocols-media-and-artifacts.md

#include "sd_socket_host.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>

namespace
{

// A value no detect status has: a read that refuses leaves it.
constexpr std::uint32_t untouched = 0xA5A5A5A5U;
// SD_DetectCardInsert()'s results (the SDK's kSD_Removed and kSD_Inserted).
constexpr std::uint32_t card_removed = 0U;
constexpr std::uint32_t card_inserted = 1U;

} // namespace

TEST_CASE("SD socket refusals and card detect routing", "[sd_socket]")
{
    // Refusal paths run first, before detect counters and stub state are used.
    std::uint32_t status = untouched;
    INFO("no card: an invalid argument");
    CHECK((wfg_test_read_without_card(&status) == kWFG_StatusInvalidArgument && status == untouched));
    INFO("no status to write: an invalid argument");
    CHECK(wfg_test_read_without_status() == kWFG_StatusInvalidArgument);
    INFO("a host SD_HostInit() has not readied: the host is not ready");
    CHECK((wfg_test_read_host_not_ready(&status) == kWFG_StatusSdHostNotReady && status == untouched));
    INFO("a card with no host: the host is not ready");
    CHECK((wfg_test_read_without_host(&status) == kWFG_StatusSdHostNotReady && status == untouched));
    INFO("no refusal asks the host");
    CHECK(wfg_test_host_card_detect_reads() == 0U);
    // The player's socket detects by GPIO and is always present; a GPIO detect
    // whose callback finds no card, or without its callback, reads removed; the
    // host's DATA3 detect, or a card with no detect configured, asks the SD host.
    status = untouched;
    INFO("the player's socket is always present: inserted, without asking the host");
    CHECK((wfg_test_socket_detects_by_gpio() && wfg_test_read_socket_detect(&status) == kWFG_StatusOk && status == card_inserted && wfg_test_host_card_detect_reads() == 0U));
    status = untouched;
    INFO("a GPIO detect that finds no card: removed");
    CHECK((wfg_test_read_gpio_detect_without_card(&status) == kWFG_StatusOk && status == card_removed));
    status = untouched;
    INFO("a GPIO detect without its callback: removed");
    CHECK((wfg_test_read_gpio_detect_without_callback(&status) == kWFG_StatusOk && status == card_removed));
    INFO("a GPIO detect never asks the host");
    CHECK(wfg_test_host_card_detect_reads() == 0U);
    wfg_test_set_host_card_detect(card_inserted);
    status = untouched;
    INFO("the host's DATA3 detect answers for the card");
    CHECK((wfg_test_read_host_data3_detect(&status) == kWFG_StatusOk && status == card_inserted && wfg_test_host_card_detect_reads() == 1U));
    wfg_test_set_host_card_detect(card_removed);
    status = untouched;
    INFO("including a removed card");
    CHECK((wfg_test_read_host_data3_detect(&status) == kWFG_StatusOk && status == card_removed && wfg_test_host_card_detect_reads() == 2U));
    wfg_test_set_host_card_detect(card_inserted);
    status = untouched;
    INFO("a card with no detect configured asks the host");
    CHECK((wfg_test_read_without_detect(&status) == kWFG_StatusOk && status == card_inserted && wfg_test_host_card_detect_reads() == 3U));
}

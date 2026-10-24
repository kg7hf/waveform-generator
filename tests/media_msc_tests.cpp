// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// SD ownership, the host-eject latch, the host's hold on its volume and the
// chunked transfers in rt1170/platform/waveform_msc.c on the host, through its
// TinyUSB MSC callbacks and the MEDIA HOST / MEDIA LOCAL entry points. The C
// stand-ins in media_msc_stubs/ replace the SD stack, the FatFs disk glue,
// FreeRTOS, the cycle counter and TinyUSB's sense setter; waveform_msc.c sees
// the firmware's TinyUSB configuration (rt1170/usb/tusb_config.h), so the
// chunks it takes are the firmware's. Built for the writable and the read-only
// LUN. Windows' own eject and PREVENT ALLOW MEDIUM REMOVAL behaviour, TinyUSB's
// chunking and the card's speed remain bench checks.
// The media's start runs once per process: with no argument the program
// starts it where the initialisation task cannot be created, then works
// through every way an initialisation ends; "boot" starts it as main() does,
// and "host-first" with MEDIA HOST before that.
// Guide: docs/modules/protocols-media-and-artifacts.md
//
// Usage: wfg_media_msc_<variant>_tests [boot | host-first]

#include "cycle_counter.h"
#include "media_msc_host.h"
#include "platform/waveform_msc.h"
#include "usb/usb_composite.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

namespace
{
constexpr std::uint8_t lun = 0U;
constexpr std::uint8_t other_lun = 1U;
constexpr std::uint8_t power_condition_none = 0U;
// SPC sense keys and additional sense codes, as the SCSI standards number them.
constexpr std::uint8_t sense_not_ready = 0x02U;
constexpr std::uint8_t medium_not_present = 0x3AU;
constexpr std::uint8_t sense_illegal_request = 0x05U;
constexpr std::uint8_t invalid_command_operation_code = 0x20U;
// SCSI operation codes (SBC-3): SYNCHRONIZE CACHE (10), which the LUN
// answers, and MODE SELECT (6), which it does not.
constexpr std::uint8_t synchronize_cache_10 = 0x35U;
constexpr std::uint8_t mode_select_6 = 0x15U;
constexpr unsigned sense_key_shift = 16U;
constexpr unsigned sense_code_shift = 8U;
// tud_msc_read10_cb / tud_msc_write10_cb failure (TUD_MSC_RET_ERROR).
constexpr std::int32_t transfer_failed = -1;
constexpr std::uint32_t card_blocks = 2048U; // media_msc_host.c
constexpr std::uint32_t block_bytes = 512U;
constexpr std::int32_t block_transferred = 512;
// PREVENT ALLOW MEDIUM REMOVAL: the PREVENT field (SBC-3 5.11) and CONTROL bytes.
constexpr std::uint8_t allow_removal = 0x00U;
constexpr std::uint8_t prevent_removal = 0x01U;
constexpr std::uint8_t obsolete_prevent = 0x02U;
constexpr std::uint8_t control_none = 0x00U;
// One chunk as TinyUSB hands a READ(10) or WRITE(10) to the callbacks: its MSC
// endpoint buffer, CFG_TUD_MSC_EP_BUFSIZE in rt1170/usb/tusb_config.h. The
// firmware's is 16 KB, 32 blocks, the most the ChipIdea controller moves with its
// one transfer descriptor from any buffer address; the tests below check that
// waveform_msc.c takes exactly that: a full chunk is one SD command, a block
// more is refused.
constexpr std::uint32_t chunk_bytes = 16384U;
constexpr std::uint32_t chunk_blocks = chunk_bytes / block_bytes;
constexpr std::uint32_t full_chunk_blocks = 32U;
constexpr std::uint32_t microseconds_per_second = 1000000U;
constexpr std::uint32_t microseconds_per_tick = 1000U; // the stand-in tick, as the firmware's
// SD bus clocks a card can be left at from the player's 198 MHz root
// (rt1170/platform/waveform_sd_clock.h, tests/sd_clock_tests.cpp): High Speed,
// and Default Speed for a card that does not support High Speed, whose
// initialisation still succeeds.
constexpr std::uint32_t high_speed_bus_hz = 49500000U;
constexpr std::uint32_t default_speed_bus_hz = 24750000U;

static_assert(std::cmp_equal(block_bytes, block_transferred));

// STATUS media_state and the msc_host_hold bits as the protocol documents
// publish them (docs/usb-sound-card-bringup.md, docs/live-control-protocol.md),
// against platform/waveform_msc.h, a C header.
struct PublishedNumber
{
    std::uint32_t value;
    std::uint32_t number;
};

constexpr auto published_media_numbers = std::to_array<PublishedNumber>({
    {kWFG_MediaUninitialized, 0U},
    {kWFG_MediaInitializing, 1U},
    {kWFG_MediaHost, 2U},
    {kWFG_MediaLocal, 3U},
    {kWFG_MediaFault, 255U},
    {kWFG_MediaHostHoldNone, 0U},
    {kWFG_MediaHostHoldRemovalPrevented, 1U},
    {kWFG_MediaHostHoldWriting, 2U},
});

static_assert(std::ranges::all_of(published_media_numbers, [](const PublishedNumber& row) { return row.value == row.number; }),

              "the published media states and host hold bits");
static_assert(chunk_bytes % block_bytes == 0U, "a chunk is whole blocks, so each chunk of a command starts at offset 0");
static_assert(chunk_blocks == full_chunk_blocks, "a full chunk is one 32-block SD command (CMD18 / CMD25)");
static_assert(card_blocks % chunk_blocks == 0U, "the test card ends on a chunk boundary");

// A sense as media_msc_host.c packs it.
[[nodiscard]] constexpr std::uint32_t sense(std::uint8_t key, std::uint8_t code) noexcept
{
    return (std::uint32_t{key} << sense_key_shift) | (std::uint32_t{code} << sense_code_shift);
}

constexpr std::uint32_t not_present = sense(sense_not_ready, medium_not_present);

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

using Block = std::array<std::uint8_t, block_bytes>;
using Chunk = std::array<std::uint8_t, chunk_bytes>;

// Each host request starts with no sense, so a check sees what it set.
[[nodiscard]] bool test_unit_ready() noexcept
{
    wfg_test_clear_sense();
    return tud_msc_test_unit_ready_cb(lun);
}

[[nodiscard]] std::int32_t read_block(Block& block) noexcept
{
    wfg_test_clear_sense();
    return tud_msc_read10_cb(lun, 0U, 0U, block.data(), block_bytes);
}

[[nodiscard]] std::int32_t write_block(Block& block) noexcept
{
    wfg_test_clear_sense();
    return tud_msc_write10_cb(lun, 0U, 0U, block.data(), block_bytes);
}

// START STOP UNIT with its Start and LoEj bits.
[[nodiscard]] bool start_stop(bool start, bool load_eject) noexcept
{
    return tud_msc_start_stop_cb(lun, power_condition_none, start, load_eject);
}

[[nodiscard]] bool eject() noexcept
{
    return start_stop(false, true);
}

[[nodiscard]] std::uint32_t ejected() noexcept
{
    return WFG_MediaGetSnapshot().msc_ejected;
}

// PREVENT ALLOW MEDIUM REMOVAL with its PREVENT field.
[[nodiscard]] bool removal(std::uint8_t prevent) noexcept
{
    return tud_msc_prevent_allow_medium_removal_cb(lun, prevent, control_none);
}

[[nodiscard]] std::uint32_t host_hold() noexcept
{
    return WFG_MediaGetSnapshot().msc_host_hold;
}

[[nodiscard]] std::uint32_t bus_clock_hz() noexcept
{
    return WFG_MediaGetSnapshot().sd_bus_clock_hz;
}

// What an initialisation records: the media error, and the status of each
// step (SD host init, the socket's detect status, card init, disk init).
// A step it did not reach reads not_reached; one no attempt has prepared, 0.
struct Initialisation
{
    std::uint32_t error;
    std::uint32_t host_init;
    std::uint32_t host_detect;
    std::uint32_t card_init;
    std::uint32_t disk_init;
};

// waveform_msc.c's WFG_MEDIA_UNKNOWN_STATUS; the stand-ins' failed SD status
// (kStatus_Fail) and disk status (FatFs STA_NOINIT); the detect statuses
// kSD_Removed and kSD_Inserted.
constexpr std::uint32_t not_reached = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint32_t sd_failed = 1U;
constexpr std::uint32_t disk_not_initialised = 1U;
constexpr std::uint32_t card_removed = 0U;
constexpr std::uint32_t card_inserted = 1U;
// A block size the media does not take.
constexpr std::uint32_t large_block_bytes = 1024U;

constexpr Initialisation succeeded{.error = kWFG_MediaErrorNone, .host_init = 0U, .host_detect = card_inserted, .card_init = 0U, .disk_init = 0U};
constexpr Initialisation prepared{.error = kWFG_MediaErrorNone, .host_init = not_reached, .host_detect = not_reached, .card_init = not_reached, .disk_init = not_reached};
constexpr Initialisation task_not_started{.error = kWFG_MediaErrorTaskStart, .host_init = 0U, .host_detect = 0U, .card_init = 0U, .disk_init = 0U};

[[nodiscard]] bool recorded(const Initialisation& expected) noexcept
{
    const auto media = WFG_MediaGetSnapshot();
    return media.error == expected.error && media.host_init_status == expected.host_init && media.host_detect_status == expected.host_detect && media.card_init_status == expected.card_init &&
           media.disk_init_status == expected.disk_init;
}

// A latched media fault: no card ready, nothing for the host, no bus clock.
[[nodiscard]] bool faulted_with(const Initialisation& expected) noexcept
{
    const auto media = WFG_MediaGetSnapshot();
    return media.state == kWFG_MediaFault && media.card_ready == 0U && media.msc_ready == 0U && media.sd_bus_clock_hz == 0U && recorded(expected);
}

// The initialisation attempts, the task's notifications and the SD host inits so far.
struct Starts
{
    std::uint32_t attempts;
    std::uint32_t notifications;
    std::uint32_t host_inits;
};

[[nodiscard]] Starts starts() noexcept
{
    return {.attempts = WFG_MediaGetSnapshot().init_attempts, .notifications = wfg_test_task_notifications(), .host_inits = wfg_test_sd_host_inits()};
}

// Nothing started since `before`: no attempt, no notification, no SD host init.
[[nodiscard]] bool started_nothing(const Starts& before) noexcept
{
    const auto now = starts();
    return now.attempts == before.attempts && now.notifications == before.notifications && now.host_inits == before.host_inits;
}

// One attempt and one notification of the task since `before`, and no SD host init yet.
[[nodiscard]] bool started_one(const Starts& before) noexcept
{
    const auto now = starts();
    return now.attempts == before.attempts + 1U && now.notifications == before.notifications + 1U && now.host_inits == before.host_inits && WFG_MediaGetSnapshot().state == kWFG_MediaInitializing &&
           recorded(prepared);
}

// The process starts with the media uninitialised and the initialisation
// task not created. While the task cannot be created, MEDIA HOST fails and
// START fails, each latching a task_start fault with no attempt; the host's
// eject latch stays. Once the task exists, START still refuses a faulted
// media (only MEDIA HOST retries one), and the task's initialisation and the
// handoff to the host refuse it too, touching no SD host.
void test_start_without_task(Checks& checks)
{
    const auto before = starts();
    checks.expect(WFG_MediaGetSnapshot().state == kWFG_MediaUninitialized && before.attempts == 0U && bus_clock_hz() == 0U, "the media starts uninitialised, with no SD bus clock");
    wfg_test_fail_task_create(true);
    checks.expect(eject() && ejected() == 1U, "an eject before the media starts is latched");
    checks.expect(WFG_MediaRequestHost() == kWFG_MediaHostRequestFailed && faulted_with(task_not_started) && ejected() == 1U && started_nothing(before),
                  "MEDIA HOST fails when the initialisation task cannot be created: a task_start fault, and the eject stays latched");
    checks.expect(WFG_MediaStartInitialization() == kWFG_StatusMediaFault && faulted_with(task_not_started) && started_nothing(before),
                  "the start fails the same way while the task cannot be created: a media fault");
    wfg_test_fail_task_create(false);
    checks.expect(WFG_MediaStartInitialization() == kWFG_StatusMediaRefused && faulted_with(task_not_started) && started_nothing(before),
                  "once the task exists the start still refuses a faulted media");
    checks.expect(WFG_MediaInitialize() == kWFG_StatusMediaRefused && faulted_with(task_not_started) && started_nothing(before), "and the task's initialisation refuses it, touching no SD host");
    checks.expect(WFG_MediaSetHost() == kWFG_StatusMediaRefused && faulted_with(task_not_started) && started_nothing(before), "and MEDIA HOST's handoff refuses it, changing nothing");
}

// SD_HostDeinit() and SD_Deinit() calls.
struct Deinits
{
    std::uint32_t host;
    std::uint32_t card;
};

[[nodiscard]] Deinits deinits() noexcept
{
    return {.host = wfg_test_sd_host_deinits(), .card = wfg_test_sd_card_deinits()};
}

// One attempt that fails at a step: MEDIA HOST restarts the faulted media
// and clears the eject latch, the task's WFG_MediaInitialize() fails with a
// media fault, and the media is faulted with that step's error and the
// statuses of the steps it reached; the failure deinitialises what the
// attempt had started (`undone`).
template <class Fault> void check_failed_attempt(Checks& checks, Fault inject, const Initialisation& expected, const Deinits& undone, const char* what)
{
    checks.expect(eject() && WFG_MediaRequestHost() == kWFG_MediaHostRequestInitializing && ejected() == 0U, "MEDIA HOST restarts a faulted media and clears the eject latch");
    const auto before = deinits();
    inject(true);
    const wfg_status initialised = WFG_MediaInitialize();
    inject(false);
    const auto after = deinits();
    checks.expect(initialised == kWFG_StatusMediaFault && faulted_with(expected) && after.host == before.host + undone.host && after.card == before.card + undone.card, what);
}

// Every step of an initialisation that can fail, in order, each from a fault.
void test_initialization_failures(Checks& checks)
{
    constexpr Deinits nothing{.host = 0U, .card = 0U};
    constexpr Deinits host{.host = 1U, .card = 0U};
    constexpr Deinits card{.host = 0U, .card = 1U};
    check_failed_attempt(checks, wfg_test_fail_sd_host_init, {.error = kWFG_MediaErrorHostInit, .host_init = sd_failed, .host_detect = not_reached, .card_init = not_reached, .disk_init = not_reached},
                         nothing, "a failed SD host init latches a host_init fault");
    check_failed_attempt(checks, wfg_test_fail_sd_detect_read, {.error = kWFG_MediaErrorHostInit, .host_init = 0U, .host_detect = not_reached, .card_init = not_reached, .disk_init = not_reached},
                         host, "an unreadable detect status latches a host_init fault and deinitialises the SD host");
    check_failed_attempt(checks, wfg_test_remove_card, {.error = kWFG_MediaErrorNoCard, .host_init = 0U, .host_detect = card_removed, .card_init = not_reached, .disk_init = not_reached}, host,
                         "an empty socket latches a no_card fault and deinitialises the SD host");
    check_failed_attempt(checks, wfg_test_fail_sd_card_init, {.error = kWFG_MediaErrorCardInit, .host_init = 0U, .host_detect = card_inserted, .card_init = sd_failed, .disk_init = not_reached}, card,
                         "a failed card init latches a card_init fault and deinitialises the card");
    const Initialisation bad_geometry{.error = kWFG_MediaErrorGeometry, .host_init = 0U, .host_detect = card_inserted, .card_init = 0U, .disk_init = not_reached};
    check_failed_attempt(
        checks, [](bool empty) { wfg_test_set_card_geometry(empty ? 0U : card_blocks, block_bytes); }, bad_geometry, card,

        "a card of no blocks latches a geometry fault");
    check_failed_attempt(
        checks, [](bool large) { wfg_test_set_card_geometry(card_blocks, large ? large_block_bytes : block_bytes); }, bad_geometry, card,

        "a card of 1024-byte blocks latches a geometry fault");
    check_failed_attempt(checks, wfg_test_fail_disk_init, {.error = kWFG_MediaErrorDiskInit, .host_init = 0U, .host_detect = card_inserted, .card_init = 0U, .disk_init = disk_not_initialised}, card,
                         "a failed disk init latches a disk_init fault and deinitialises the card");
}

// An eject while faulted, then MEDIA HOST: the request that restarts
// initialization also clears the latch. The snapshot reports no SD bus clock
// until a card is ready, whatever the SD driver holds meanwhile, and then the
// clock the card's initialisation left it at.
void test_host_request_after_fault(Checks& checks)
{
    checks.expect(WFG_MediaGetSnapshot().state == kWFG_MediaFault && bus_clock_hz() == 0U, "no SD bus clock while the media is faulted");
    wfg_test_set_sd_driver_bus_clock(high_speed_bus_hz);
    checks.expect(bus_clock_hz() == 0U, "no SD bus clock while the media is faulted, whatever the SD driver holds");
    checks.expect(WFG_MediaRequestLocal() == kWFG_MediaLocalFailed && WFG_MediaGetSnapshot().state == kWFG_MediaFault, "MEDIA LOCAL fails while the media is faulted and changes nothing");
    checks.expect(eject() && ejected() == 1U, "an eject that arrives while the media is faulted is latched");
    const auto before = starts();
    checks.expect(WFG_MediaRequestHost() == kWFG_MediaHostRequestInitializing && ejected() == 0U && started_one(before), "MEDIA HOST that restarts initialization clears the eject latch");
    checks.expect(bus_clock_hz() == 0U, "no SD bus clock while the card initialises");
    wfg_test_set_sd_init_bus_clock(default_speed_bus_hz);
    checks.expect(WFG_MediaInitialize() == kWFG_StatusOk && WFG_MediaGetSnapshot().state == kWFG_MediaHost && recorded(succeeded), "the initialization task hands the card to the host");
    checks.expect(bus_clock_hz() == default_speed_bus_hz, "a ready card reports the SD bus clock its initialisation left: 24.75 MHz for a card that stayed in Default Speed");
    checks.expect(test_unit_ready(), "the host sees the medium after MEDIA HOST");
}

// A ready card: the start and the task's initialisation accept it and change
// nothing while the host owns it; while the board owns it the start accepts
// it and the initialisation refuses it, both changing nothing.
void test_initialization_while_owned(Checks& checks)
{
    const auto before = starts();
    checks.expect(WFG_MediaStartInitialization() == kWFG_StatusOk && WFG_MediaInitialize() == kWFG_StatusOk && WFG_MediaGetSnapshot().state == kWFG_MediaHost && recorded(succeeded) &&
                      started_nothing(before),
                  "while the host owns the card, the start and the initialisation accept it and start nothing");
    checks.expect(WFG_MediaRequestLocal() == kWFG_MediaLocalGranted && WFG_MediaStartInitialization() == kWFG_StatusOk && WFG_MediaIsLocal() && started_nothing(before),
                  "while the board owns the card the start accepts it and starts nothing");
    checks.expect(WFG_MediaInitialize() == kWFG_StatusMediaRefused && WFG_MediaIsLocal() && recorded(succeeded) && started_nothing(before), "and the initialisation refuses it, changing nothing");
    checks.expect(WFG_MediaRequestHost() == kWFG_MediaHostRequestReady && test_unit_ready() && started_nothing(before), "MEDIA HOST hands the card back");
}

void test_host_owns_medium(Checks& checks)
{
    Block block{};
    const auto before = WFG_MediaGetSnapshot();
    checks.expect(read_block(block) == block_transferred && wfg_test_sd_reads() == 1U, "the host reads the card");
    checks.expect(start_stop(false, false) && start_stop(true, true) && start_stop(true, false) && test_unit_ready(), "START STOP UNIT without LoEj=1 Start=0 leaves the medium present");
    checks.expect(!tud_msc_start_stop_cb(other_lun, power_condition_none, false, true) && test_unit_ready(), "an eject for another LUN is refused and changes nothing");
    const auto after = WFG_MediaGetSnapshot();
    checks.expect(after.eject_requests == before.eject_requests && after.msc_ejected == 0U, "only an eject counts as one");
}

// MEDIA LOCAL refused while the host holds its volume leaves everything as it was.
void check_refusal(Checks& checks, wfg_media_local_request_result expected, const char* what)
{
    const auto before = WFG_MediaGetSnapshot();
    const auto result = WFG_MediaRequestLocal();
    const auto after = WFG_MediaGetSnapshot();
    checks.expect(result == expected && after.state == kWFG_MediaHost && after.msc_ready == 1U && after.ownership_handoffs == before.ownership_handoffs && !WFG_MediaIsLocal() && test_unit_ready(),
                  what);
}

// The host's PREVENT MEDIUM REMOVAL keeps MEDIA LOCAL waiting until ALLOW.
void test_removal_lock(Checks& checks)
{
    checks.expect(host_hold() == kWFG_MediaHostHoldNone, "the host holds nothing before it asks to");
    checks.expect(removal(prevent_removal) && host_hold() == kWFG_MediaHostHoldRemovalPrevented, "PREVENT MEDIUM REMOVAL is the host's lock");
    check_refusal(checks, kWFG_MediaLocalHostLocked, "MEDIA LOCAL is HOST_LOCKED while the host prevents removal, and changes nothing");
    checks.expect(removal(allow_removal) && host_hold() == kWFG_MediaHostHoldNone, "ALLOW MEDIUM REMOVAL releases the lock");
    checks.expect(removal(obsolete_prevent) && host_hold() == kWFG_MediaHostHoldRemovalPrevented && removal(allow_removal), "an obsolete PREVENT value counts as prevent");
    checks.expect(!tud_msc_prevent_allow_medium_removal_cb(other_lun, prevent_removal, control_none) && host_hold() == kWFG_MediaHostHoldNone,
                  "a PREVENT for another LUN is refused and changes nothing");
}

#if defined(WFG_MSC_READ_ONLY)
// The read-only LUN refuses every WRITE(10) as DATA PROTECT, so no write ever
// holds the volume. Each refused chunk counts as a call and an error, and
// none reaches the card.
void test_write_hold(Checks& checks)
{
    Block block{};
    const auto before = WFG_MediaGetSnapshot();
    const auto writes = wfg_test_sd_writes();
    checks.expect(write_block(block) == transfer_failed && host_hold() == kWFG_MediaHostHoldNone, "a refused write holds nothing");
    const auto after = WFG_MediaGetSnapshot();
    checks.expect(after.write_calls == before.write_calls + 1U && after.write_errors == before.write_errors + 1U && after.write_blocks == before.write_blocks &&
                      after.invalid_requests == before.invalid_requests && wfg_test_sd_writes() == writes,
                  "the read-only LUN counts a refused write chunk as one call and one error, no blocks and no invalid request");
}

#else
// A WRITE(10) that has put a block on the card holds the volume until its status.
void test_write_hold(Checks& checks)
{
    // A WRITE(10) block that tud_msc_write10_cb refuses: not at a block boundary.
    constexpr std::uint32_t misaligned_offset = 1U;
    Block block{};
    checks.expect(write_block(block) == block_transferred && host_hold() == kWFG_MediaHostHoldWriting, "a written block holds the volume");
    check_refusal(checks, kWFG_MediaLocalHostWriting, "MEDIA LOCAL is HOST_WRITING between two blocks of a WRITE(10), and changes nothing");
    checks.expect(write_block(block) == block_transferred && host_hold() == kWFG_MediaHostHoldWriting, "the rest of the command still reaches the card");
    tud_msc_write10_complete_cb(lun);
    checks.expect(host_hold() == kWFG_MediaHostHoldNone, "the command's status releases the volume");
    checks.expect(write_block(block) == block_transferred && tud_msc_write10_cb(lun, 0U, misaligned_offset, block.data(), block_bytes) == transfer_failed && host_hold() == kWFG_MediaHostHoldNone,
                  "a failed block ends the command's hold");
    tud_msc_write10_complete_cb(lun);
}

#endif

// A new USB configuration or an unplug ends the host's session and its hold.
void test_host_session_end(Checks& checks)
{
    checks.expect(removal(prevent_removal), "the host locks the medium");
#if !defined(WFG_MSC_READ_ONLY)
    Block block{};
    checks.expect(write_block(block) == block_transferred && host_hold() == (kWFG_MediaHostHoldRemovalPrevented | kWFG_MediaHostHoldWriting),
                  "the lock and a write in progress hold the volume together");
    check_refusal(checks, kWFG_MediaLocalHostWriting, "a write in progress is reported before the lock");
#endif
    WFG_MediaEndHostSession();
    checks.expect(host_hold() == kWFG_MediaHostHoldNone, "the end of the host's session releases its hold");
}

// The control requests a host sends to the composite device, by their wire
// numbers: USB 2.0 Table 9-2 (bmRequestType), Bulk-Only Transport 1.0 3.1
// and 3.2 (Bulk-Only Mass Storage Reset, Get Max LUN), and the player's
// interface numbers (usb/usb_composite.h: CDC control 0, MSC 2).
constexpr std::uint8_t class_interface_out = 0x21U;
constexpr std::uint8_t class_interface_in = 0xA1U;
constexpr std::uint8_t standard_interface_out = 0x01U;
constexpr std::uint8_t bot_reset = 0xFFU;
constexpr std::uint8_t get_max_lun = 0xFEU;
// A class request that Bulk-Only Transport does not define: only bRequest 0xFF is the reset.
constexpr std::uint8_t other_class_request = 0x00U;
constexpr std::uint8_t cdc_control_interface = 0U;
constexpr std::uint8_t msc_interface = 2U;

static_assert(wfg_usb_request_type_class_interface_out == class_interface_out && wfg_msc_request_bot_reset == bot_reset && wfg_usb_interface_msc == msc_interface &&
                  wfg_usb_interface_cdc_control == cdc_control_interface,
              "the firmware's numbers are the wire's");

// The host gives up on a WRITE(10) between its chunks and recovers with a
// Bulk-Only Mass Storage Reset, then CLEAR_FEATURE(HALT) on the bulk pipes,
// without a new configuration (#44). TinyUSB answers the reset without an MSC
// callback; once its status stage is done, the cancelled command no longer
// holds the volume. The removal lock stays, and no other request releases
// anything.
void test_bot_reset(Checks& checks)
{
    checks.expect(removal(prevent_removal), "the host locks the medium");
#if defined(WFG_MSC_READ_ONLY)
    constexpr std::uint32_t held = kWFG_MediaHostHoldRemovalPrevented;
#else
    constexpr std::uint32_t held = kWFG_MediaHostHoldRemovalPrevented | kWFG_MediaHostHoldWriting;
    Block block{};
    checks.expect(write_block(block) == block_transferred && host_hold() == held, "the first chunk of a WRITE(10) the host gives up on is on the card");
    check_refusal(checks, kWFG_MediaLocalHostWriting, "MEDIA LOCAL is HOST_WRITING after the host gave up on the write");
#endif
    WFG_MediaControlTransferDone(class_interface_in, get_max_lun, msc_interface);
    WFG_MediaControlTransferDone(class_interface_out, bot_reset, cdc_control_interface);
    WFG_MediaControlTransferDone(standard_interface_out, bot_reset, msc_interface);
    WFG_MediaControlTransferDone(class_interface_out, other_class_request, msc_interface);
    checks.expect(host_hold() == held, "GET MAX LUN, a reset of another interface, a standard request with the reset's number and another class request to the MSC interface release nothing");
    WFG_MediaControlTransferDone(class_interface_out, bot_reset, msc_interface);
    checks.expect(host_hold() == kWFG_MediaHostHoldRemovalPrevented, "a Bulk-Only Mass Storage Reset of the MSC interface ends the write's hold, not the removal lock");
    check_refusal(checks, kWFG_MediaLocalHostLocked, "MEDIA LOCAL is then HOST_LOCKED until the host allows removal");
    checks.expect(removal(allow_removal) && host_hold() == kWFG_MediaHostHoldNone, "ALLOW MEDIUM REMOVAL releases the rest");
}

// Once the host has let go, MEDIA LOCAL hands the card over. While the board owns
// it, a host lock is recorded but does not take the card back.
void test_media_local_after_release(Checks& checks)
{
    const auto before = WFG_MediaGetSnapshot();
    checks.expect(WFG_MediaRequestLocal() == kWFG_MediaLocalGranted && WFG_MediaIsLocal() && WFG_MediaGetSnapshot().ownership_handoffs == before.ownership_handoffs + 1U,
                  "MEDIA LOCAL hands the released card to the board");
    checks.expect(bus_clock_hz() == default_speed_bus_hz, "the card's SD bus clock is still reported while the board owns it");
    checks.expect(removal(prevent_removal) && host_hold() == kWFG_MediaHostHoldRemovalPrevented && WFG_MediaRequestLocal() == kWFG_MediaLocalGranted && WFG_MediaIsLocal(),
                  "MEDIA LOCAL again while the board owns the card is granted and changes nothing");
    checks.expect(removal(allow_removal) && WFG_MediaRequestHost() == kWFG_MediaHostRequestReady && test_unit_ready(), "MEDIA HOST hands the card back to the host");
}

void check_write_while_ejected(Checks& checks)
{
    Block block{};
    const auto writes = wfg_test_sd_writes();
#if defined(WFG_MSC_READ_ONLY)
    constexpr std::uint8_t sense_data_protect = 0x07U;
    constexpr std::uint8_t write_protected = 0x27U;
    checks.expect(!tud_msc_is_writable_cb(lun) && write_block(block) == transfer_failed && wfg_test_last_sense() == sense(sense_data_protect, write_protected) && wfg_test_sd_writes() == writes,
                  "the read-only LUN keeps refusing writes as DATA PROTECT");
#else
    const auto before = WFG_MediaGetSnapshot();
    checks.expect(tud_msc_is_writable_cb(lun), "an ejected medium is absent, not write-protected");
    checks.expect(write_block(block) == transfer_failed && wfg_test_last_sense() == not_present && wfg_test_sd_writes() == writes,
                  "WRITE(10) fails with NOT READY, MEDIUM NOT PRESENT and leaves the card alone");
    const auto after = WFG_MediaGetSnapshot();
    checks.expect(after.write_calls == before.write_calls && after.write_errors == before.write_errors && after.invalid_requests == before.invalid_requests &&
                      after.msc_host_hold == kWFG_MediaHostHoldNone,
                  "a WRITE(10) refused because the medium is ejected counts as no call, no error and no invalid request, and holds nothing");
#endif
}

void test_eject_latch(Checks& checks)
{
    const auto before = WFG_MediaGetSnapshot();
    checks.expect(eject(), "the host's eject succeeds");
    const auto after = WFG_MediaGetSnapshot();
    checks.expect(after.msc_ejected == 1U && after.eject_requests == before.eject_requests + 1U, "an eject latches msc_ejected and is still counted");
    checks.expect(after.state == kWFG_MediaHost && after.msc_ready == 1U && after.ownership_handoffs == before.ownership_handoffs, "an eject moves no ownership");
    checks.expect(!test_unit_ready() && wfg_test_last_sense() == not_present && WFG_MediaGetSnapshot().not_ready_responses == after.not_ready_responses + 1U,
                  "TEST UNIT READY reports NOT READY, MEDIUM NOT PRESENT while ejected");
    Block block{};
    const auto reads = wfg_test_sd_reads();
    checks.expect(read_block(block) == transfer_failed && wfg_test_last_sense() == not_present && wfg_test_sd_reads() == reads,
                  "READ(10) fails with NOT READY, MEDIUM NOT PRESENT and leaves the card alone");
    const auto refused = WFG_MediaGetSnapshot();
    checks.expect(refused.read_calls == after.read_calls && refused.read_errors == after.read_errors && refused.invalid_requests == after.invalid_requests,
                  "a READ(10) refused because the medium is ejected counts as no call, no error and no invalid request");
    std::uint32_t blocks = card_blocks;
    std::uint16_t size = 1U;
    tud_msc_capacity_cb(lun, &blocks, &size);
    checks.expect(blocks == 0U && size == 0U, "READ CAPACITY reports no medium");
    check_write_while_ejected(checks);
    checks.expect(start_stop(true, true) && start_stop(true, false) && !test_unit_ready() && ejected() == 1U, "a host LOAD or START does not end the latch; MEDIA HOST does");
}

void test_media_local_after_eject(Checks& checks)
{
    checks.expect(WFG_MediaRequestLocal() == kWFG_MediaLocalGranted && WFG_MediaIsLocal(), "MEDIA LOCAL after an eject hands the card to the board");
    checks.expect(ejected() == 1U && !test_unit_ready(), "the medium stays absent for the host while the board owns it");
}

void test_media_host_clears_latch(Checks& checks)
{
    const auto before = WFG_MediaGetSnapshot();
    checks.expect(WFG_MediaRequestHost() == kWFG_MediaHostRequestReady, "MEDIA HOST hands the card back to the host");
    const auto after = WFG_MediaGetSnapshot();
    checks.expect(after.msc_ejected == 0U && after.state == kWFG_MediaHost && after.ownership_handoffs == before.ownership_handoffs + 1U, "MEDIA HOST clears the eject latch");
    Block block{};
    checks.expect(test_unit_ready() && read_block(block) == block_transferred, "the host sees and reads the medium again");
#if !defined(WFG_MSC_READ_ONLY)
    const auto writes = wfg_test_sd_writes();
    checks.expect(write_block(block) == block_transferred && wfg_test_sd_writes() == writes + 1U, "the host writes the medium again");
#endif
    checks.expect(eject() && !test_unit_ready(), "the next eject latches again");
    checks.expect(WFG_MediaRequestHost() == kWFG_MediaHostRequestReady && test_unit_ready() && WFG_MediaGetSnapshot().ownership_handoffs == after.ownership_handoffs,
                  "MEDIA HOST while the host still owns the card clears the latch without a handoff");
}

// A chunk whose blocks all differ, so a chunk moved whole but from the wrong
// place shows.
[[nodiscard]] Chunk pattern(std::uint32_t seed) noexcept
{
    constexpr std::uint32_t multiplier = 1664525U;
    constexpr std::uint32_t increment = 1013904223U;
    constexpr unsigned top_byte_shift = 24U;
    Chunk chunk{};
    std::uint32_t state = seed;

    for (auto& byte : chunk)
    {
        state = state * multiplier + increment;
        byte = static_cast<std::uint8_t>(state >> top_byte_shift);
    }

    return chunk;
}

// One chunk of a READ(10) or WRITE(10) as TinyUSB hands it over: at a block
// boundary (offset 0), bytes long.
[[nodiscard]] bool read_chunk(std::uint32_t lba, std::span<std::uint8_t> bytes) noexcept
{
    wfg_test_clear_sense();
    return std::cmp_equal(tud_msc_read10_cb(lun, lba, 0U, bytes.data(), static_cast<std::uint32_t>(bytes.size())), bytes.size());
}

[[nodiscard]] bool write_chunk(std::uint32_t lba, std::span<std::uint8_t> bytes) noexcept
{
    wfg_test_clear_sense();
    return std::cmp_equal(tud_msc_write10_cb(lun, lba, 0U, bytes.data(), static_cast<std::uint32_t>(bytes.size())), bytes.size());
}

// The latest SD command started at lba and moved blocks.
[[nodiscard]] bool last_sd_command(std::uint32_t lba, std::uint32_t blocks) noexcept
{
    return wfg_test_last_sd_lba() == lba && wfg_test_last_sd_blocks() == blocks;
}

// Puts a chunk on the card at lba: a WRITE(10) chunk on the writable LUN,
// straight onto the card on the read-only one.
[[nodiscard]] bool stage_chunk(std::uint32_t lba, Chunk& chunk) noexcept
{
#if defined(WFG_MSC_READ_ONLY)
    return wfg_test_put_card_bytes(lba, chunk.data(), chunk_bytes);
#else
    const bool written = write_chunk(lba, chunk) && last_sd_command(lba, chunk_blocks) && host_hold() == kWFG_MediaHostHoldWriting;
    tud_msc_write10_complete_cb(lun);
    return written && host_hold() == kWFG_MediaHostHoldNone;
#endif
}

// A full chunk is one SD command of all its blocks at its LBA, through the
// bounce buffer both ways; the call counters count the chunk once and the
// block counters every block.
void test_full_chunk(Checks& checks)
{
    constexpr std::uint32_t lba = 3U * chunk_blocks + 5U;
    constexpr std::uint32_t seed = 0x5A17U;
    const auto before = WFG_MediaGetSnapshot();
    auto staged = pattern(seed);
    Chunk on_card{};
    checks.expect(stage_chunk(lba, staged) && wfg_test_get_card_bytes(lba, on_card.data(), chunk_bytes) && on_card == staged,
                  "a full WRITE(10) chunk is one SD write of all its blocks at its LBA, and holds the volume until its status");
    Chunk read{};
    checks.expect(read_chunk(lba, read) && last_sd_command(lba, chunk_blocks) && read == staged, "a full READ(10) chunk is one SD read of all its blocks at its LBA and returns the card's bytes");
    const auto after = WFG_MediaGetSnapshot();
    checks.expect(after.read_calls == before.read_calls + 1U && after.read_blocks == before.read_blocks + chunk_blocks && after.read_errors == before.read_errors,
                  "read_calls counts the chunk and read_blocks its blocks");
#if !defined(WFG_MSC_READ_ONLY)
    checks.expect(after.write_calls == before.write_calls + 1U && after.write_blocks == before.write_blocks + chunk_blocks && after.write_errors == before.write_errors,
                  "write_calls counts the chunk and write_blocks its blocks");
#endif
    checks.expect(after.msc_max_blocks_per_call == chunk_blocks, "msc_max_blocks_per_call reports a full chunk's blocks");
}

// A command's last chunk can be shorter: one SD command of its own blocks. The
// card's last chunk is read; one that would leave the card is refused before
// the SD card is asked.
void test_short_and_last_chunks(Checks& checks)
{
    constexpr std::uint32_t short_blocks = 8U;
    constexpr std::size_t short_bytes = std::size_t{short_blocks} * block_bytes;
    constexpr std::uint32_t lba = 7U;
    constexpr std::uint32_t last_lba = card_blocks - chunk_blocks;
    Chunk chunk{};
    const auto short_chunk = std::span{chunk}.first(short_bytes);
    checks.expect(read_chunk(lba, short_chunk) && last_sd_command(lba, short_blocks), "a short last chunk is one SD read of its own blocks");
    checks.expect(read_chunk(last_lba, chunk) && last_sd_command(last_lba, chunk_blocks), "the card's last full chunk is read");
    const auto before = WFG_MediaGetSnapshot();
    const auto reads = wfg_test_sd_reads();
    checks.expect(!read_chunk(last_lba + 1U, chunk) && wfg_test_last_sense() == not_present && wfg_test_sd_reads() == reads,
                  "a chunk that would leave the card fails with NOT READY, MEDIUM NOT PRESENT and leaves the card alone");
    const auto after = WFG_MediaGetSnapshot();
    checks.expect(after.invalid_requests == before.invalid_requests + 1U && after.read_errors == before.read_errors + 1U && after.read_calls == before.read_calls &&
                      WFG_MediaGetSnapshot().msc_max_blocks_per_call == chunk_blocks,
                  "it counts one invalid request and one read error, and no call");
}

// A chunk that starts at the card's end or past it is refused before the SD
// card is asked, a single block too: past the end, the room left on the card
// (its blocks less the LBA) would wrap to a huge count and admit any chunk,
// which TinyUSB's MSC driver, checking no LBA, would pass to the card.
void test_chunks_past_the_card(Checks& checks)
{
    constexpr std::uint32_t past_end = 5U;
    constexpr std::array<std::uint32_t, 2> lbas{card_blocks, card_blocks + past_end};
    std::array<std::uint8_t, block_bytes> block{};
    Chunk chunk{};

    for (const std::uint32_t lba : lbas)
    {
        const auto before = WFG_MediaGetSnapshot();
        const auto reads = wfg_test_sd_reads();
        checks.expect(!read_chunk(lba, block) && wfg_test_last_sense() == not_present && !read_chunk(lba, chunk) && wfg_test_last_sense() == not_present && wfg_test_sd_reads() == reads,
                      "a block or a chunk read at or past the card's end fails with NOT READY, MEDIUM NOT PRESENT and leaves the card alone");
#if defined(WFG_MSC_READ_ONLY)
        constexpr std::uint32_t refused_writes = 0U;
#else
        constexpr std::uint32_t refused_writes = 2U;
        const auto writes = wfg_test_sd_writes();
        checks.expect(!write_chunk(lba, block) && wfg_test_last_sense() == not_present && !write_chunk(lba, chunk) && wfg_test_last_sense() == not_present && wfg_test_sd_writes() == writes &&
                          host_hold() == kWFG_MediaHostHoldNone,
                      "a block or a chunk written at or past the card's end fails with NOT READY, MEDIUM NOT PRESENT, writes nothing and holds nothing");
#endif
        const auto after = WFG_MediaGetSnapshot();
        checks.expect(after.invalid_requests == before.invalid_requests + 2U + refused_writes && after.read_errors == before.read_errors + 2U &&
                          after.write_errors == before.write_errors + refused_writes && after.read_calls == before.read_calls && after.write_calls == before.write_calls,
                      "each refusal at or past the card's end counts an invalid request and an error, and no call");
    }
}

// What TinyUSB never hands over with this buffer is refused before the card:
// more than one chunk, part of a block, or a chunk off a block boundary.
void test_malformed_chunks(Checks& checks)
{
    std::array<std::uint8_t, chunk_bytes + block_bytes> bytes{};
    const auto before = WFG_MediaGetSnapshot();
    const auto reads = wfg_test_sd_reads();
    const bool reads_refused =
        !read_chunk(0U, bytes) && !read_chunk(0U, std::span{bytes}.first(chunk_bytes - 1U)) && tud_msc_read10_cb(lun, 0U, block_bytes, bytes.data(), chunk_bytes) == transfer_failed;
    checks.expect(reads_refused && wfg_test_sd_reads() == reads, "a read longer than a chunk, of part of a block or at an offset is refused");
#if defined(WFG_MSC_READ_ONLY)
    constexpr std::uint32_t refused_writes = 0U;
#else
    constexpr std::uint32_t refused_writes = 1U;
    const auto writes = wfg_test_sd_writes();
    checks.expect(!write_chunk(0U, bytes) && wfg_test_sd_writes() == writes && host_hold() == kWFG_MediaHostHoldNone, "a write longer than a chunk is refused and holds nothing");
#endif
    const auto after = WFG_MediaGetSnapshot();
    checks.expect(after.invalid_requests == before.invalid_requests + 3U + refused_writes && after.read_errors == before.read_errors + 3U &&
                      after.write_errors == before.write_errors + refused_writes && after.read_calls == before.read_calls && after.write_calls == before.write_calls,
                  "each refusal counts an invalid request and an error, and no call");
}

// A chunk the card fails is one call and one error, and no blocks; a failed
// write chunk ends the hold its command's earlier chunk set on the volume.
void test_failed_chunks(Checks& checks)
{
    auto chunk = pattern(1U);
#if !defined(WFG_MSC_READ_ONLY)
    checks.expect(write_chunk(0U, chunk) && host_hold() == kWFG_MediaHostHoldWriting, "the command's first chunk is on the card and holds the volume");
#endif
    const auto before = WFG_MediaGetSnapshot();
    wfg_test_fail_sd_transfers(true);
    const bool read_failed = !read_chunk(0U, chunk);
#if !defined(WFG_MSC_READ_ONLY)
    const bool write_failed = !write_chunk(chunk_blocks, chunk) && host_hold() == kWFG_MediaHostHoldNone;
    tud_msc_write10_complete_cb(lun);
#endif
    wfg_test_fail_sd_transfers(false);
    const auto after = WFG_MediaGetSnapshot();
    checks.expect(read_failed && after.read_calls == before.read_calls + 1U && after.read_errors == before.read_errors + 1U && after.read_blocks == before.read_blocks,
                  "a failed read chunk counts one call and one error, and no blocks");
#if !defined(WFG_MSC_READ_ONLY)
    checks.expect(write_failed && after.write_calls == before.write_calls + 1U && after.write_errors == before.write_errors + 1U && after.write_blocks == before.write_blocks,
                  "a failed write chunk counts one call and one error, no blocks, and releases the volume");
#endif
}

// A READ(10) or WRITE(10) chunk for another LUN, or with no buffer, is
// refused before the card as an invalid request and an error, and moves no
// block.
void test_invalid_lun_and_buffer(Checks& checks)
{
    Block block{};
    const auto before = WFG_MediaGetSnapshot();
    const auto reads = wfg_test_sd_reads();
    const auto writes = wfg_test_sd_writes();
    wfg_test_clear_sense();
    const bool reads_refused = tud_msc_read10_cb(other_lun, 0U, 0U, block.data(), block_bytes) == transfer_failed && tud_msc_read10_cb(lun, 0U, 0U, nullptr, block_bytes) == transfer_failed;
    const bool writes_refused = tud_msc_write10_cb(other_lun, 0U, 0U, block.data(), block_bytes) == transfer_failed && tud_msc_write10_cb(lun, 0U, 0U, nullptr, block_bytes) == transfer_failed;
    const auto after = WFG_MediaGetSnapshot();
    checks.expect(reads_refused && writes_refused && wfg_test_sd_reads() == reads && wfg_test_sd_writes() == writes && host_hold() == kWFG_MediaHostHoldNone,
                  "a chunk for another LUN or with no buffer is refused before the card");
#if defined(WFG_MSC_READ_ONLY)
    // The read-only LUN refuses a write before it looks at the request: a call and an error each.
    constexpr std::uint32_t invalid_writes = 0U;
    constexpr std::uint32_t write_calls = 2U;
#else
    constexpr std::uint32_t invalid_writes = 2U;
    constexpr std::uint32_t write_calls = 0U;
#endif
    checks.expect(after.invalid_requests == before.invalid_requests + 2U + invalid_writes && after.read_errors == before.read_errors + 2U && after.write_errors == before.write_errors + 2U &&
                      after.read_calls == before.read_calls && after.write_calls == before.write_calls + write_calls && after.read_blocks == before.read_blocks,
                  "each counts one error, and one invalid request where the LUN looks at it, and no call or block");
}

// The SCSI commands TinyUSB leaves to the firmware: SYNCHRONIZE CACHE (10)
// passes while the host owns the medium, counted in sync_cache_calls; any
// other command, or one for another LUN, is ILLEGAL REQUEST, INVALID COMMAND
// OPERATION CODE and counts an invalid request.
void test_scsi_commands(Checks& checks)
{
    const auto before = WFG_MediaGetSnapshot();
    wfg_test_clear_sense();
    checks.expect(wfg_test_scsi_command(lun, synchronize_cache_10) == 0 && wfg_test_last_sense() == 0U, "SYNCHRONIZE CACHE (10) passes on the host's medium");
    const auto synced = WFG_MediaGetSnapshot();
    checks.expect(synced.sync_cache_calls == before.sync_cache_calls + 1U && synced.invalid_requests == before.invalid_requests, "and counts in sync_cache_calls");
    const auto illegal = sense(sense_illegal_request, invalid_command_operation_code);
    wfg_test_clear_sense();
    const bool other_lun_refused = wfg_test_scsi_command(other_lun, synchronize_cache_10) == transfer_failed && wfg_test_last_sense() == illegal;
    wfg_test_clear_sense();
    const bool unknown_refused = wfg_test_scsi_command(lun, mode_select_6) == transfer_failed && wfg_test_last_sense() == illegal;
    wfg_test_clear_sense();
    const bool null_refused = wfg_test_scsi_without_command(lun) == transfer_failed && wfg_test_last_sense() == illegal;
    const auto after = WFG_MediaGetSnapshot();
    checks.expect(other_lun_refused && unknown_refused && null_refused && after.invalid_requests == synced.invalid_requests + 3U && after.sync_cache_calls == synced.sync_cache_calls,
                  "another LUN's, an unsupported and a missing command are ILLEGAL REQUEST, one invalid request each");
}

// Reads one full chunk whose SD transfer takes the given cycles and ticks, and
// returns msc_max_read_us after it.
[[nodiscard]] std::uint32_t max_read_us_after(std::uint32_t cycles, std::uint32_t ticks) noexcept
{
    Chunk chunk{};
    wfg_test_set_sd_transfer_time(cycles, ticks);
    const bool read = read_chunk(0U, chunk);
    wfg_test_set_sd_transfer_time(0U, 0U);
    return read ? WFG_MediaGetSnapshot().msc_max_read_us : 0U;
}

// The longest SD transfer of one chunk, reads and writes apart, in
// microseconds: by the cycle counter while it cannot have wrapped (fewer ticks
// than its period, 4312 at 996 MHz), else by the tick, up to UINT32_MAX.
void test_transfer_maxima(Checks& checks)
{
    constexpr std::uint32_t board_cycles_per_us = 996U;
    constexpr std::uint32_t board_period_ticks = 4312U; // 2^32 cycles at 996 MHz, in whole milliseconds
    constexpr std::uint32_t chunk_read_us = 1500U;
    constexpr std::uint32_t quick_read_us = 100U;
    constexpr std::uint32_t period_ticks = std::numeric_limits<std::uint32_t>::max() / board_cycles_per_us / microseconds_per_tick;
    constexpr std::uint32_t half_tick_us = microseconds_per_tick / 2U;
    constexpr std::uint32_t wrapped_cycles = 12345U;
    constexpr std::uint32_t saturating_ticks = (std::numeric_limits<std::uint32_t>::max() / microseconds_per_tick) + 1U;
    static_assert(period_ticks == board_period_ticks, "the cycle counter wraps after 4312 whole ticks at 996 MHz");
    const std::uint32_t cycles_per_us = m110_platform_cycle_counter_frequency_hz() / microseconds_per_second;
    checks.expect(cycles_per_us == board_cycles_per_us, "the stand-in clock runs at the board's 996 MHz");
    checks.expect(max_read_us_after(chunk_read_us * cycles_per_us, 1U) == chunk_read_us, "msc_max_read_us is the chunk's SD read time, by the cycle counter");
    checks.expect(max_read_us_after(quick_read_us * cycles_per_us, 0U) == chunk_read_us, "a quicker read leaves the maximum");
    const std::uint32_t below_period_us = ((period_ticks - 1U) * microseconds_per_tick) + half_tick_us;
    checks.expect(max_read_us_after(below_period_us * cycles_per_us, period_ticks - 1U) == below_period_us, "a read of fewer ticks than the counter's period is timed by the cycle counter");
    checks.expect(max_read_us_after(wrapped_cycles, period_ticks) == period_ticks * microseconds_per_tick, "a read as long as the counter's period, which may have wrapped it, is timed by the tick");
    checks.expect(max_read_us_after(wrapped_cycles, saturating_ticks) == std::numeric_limits<std::uint32_t>::max(), "a read too long for microseconds in 32 bits reads as UINT32_MAX");
    auto chunk = pattern(2U);
#if defined(WFG_MSC_READ_ONLY)
    const auto writes = wfg_test_sd_writes();
    checks.expect(!write_chunk(0U, chunk) && wfg_test_sd_writes() == writes && WFG_MediaGetSnapshot().msc_max_write_us == 0U,
                  "the read-only LUN refuses a full chunk before the card, so msc_max_write_us stays 0");
#else
    constexpr std::uint32_t chunk_write_us = 2500U;
    wfg_test_set_sd_transfer_time(chunk_write_us * cycles_per_us, 2U);
    const bool written = write_chunk(0U, chunk);
    wfg_test_set_sd_transfer_time(0U, 0U);
    tud_msc_write10_complete_cb(lun);
    checks.expect(written && WFG_MediaGetSnapshot().msc_max_write_us == chunk_write_us && WFG_MediaGetSnapshot().msc_max_read_us == std::numeric_limits<std::uint32_t>::max(),
                  "msc_max_write_us is the chunk's SD write time, apart from the reads");
#endif
}

// The start as main() makes it: from an uninitialised media the task is told
// to run one attempt; a second start, and MEDIA HOST while the card
// initialises, start nothing more, and the request still clears the eject
// latch; the task's initialisation hands the card to the host.
void test_boot(Checks& checks)
{
    const auto before = starts();
    checks.expect(WFG_MediaGetSnapshot().state == kWFG_MediaUninitialized && WFG_MediaStartInitialization() == kWFG_StatusOk && started_one(before),
                  "the start tells the task to run one initialisation");
    const auto started = starts();
    checks.expect(WFG_MediaStartInitialization() == kWFG_StatusOk && started_nothing(started), "a second start while the card initialises starts nothing");
    checks.expect(eject() && WFG_MediaRequestHost() == kWFG_MediaHostRequestInitializing && ejected() == 0U && started_nothing(started),
                  "MEDIA HOST while the card initialises starts nothing more and clears the eject latch");
    checks.expect(WFG_MediaInitialize() == kWFG_StatusOk && WFG_MediaGetSnapshot().state == kWFG_MediaHost && recorded(succeeded) && test_unit_ready(),
                  "the task's initialisation hands the card to the host");
}

// MEDIA HOST before the start: it starts the initialisation itself.
void test_host_first(Checks& checks)
{
    const auto before = starts();
    checks.expect(WFG_MediaGetSnapshot().state == kWFG_MediaUninitialized && WFG_MediaRequestHost() == kWFG_MediaHostRequestInitializing && started_one(before),
                  "MEDIA HOST on an uninitialised media tells the task to run one initialisation");
    checks.expect(WFG_MediaInitialize() == kWFG_StatusOk && WFG_MediaGetSnapshot().state == kWFG_MediaHost && recorded(succeeded), "the task's initialisation hands the card to the host");
    const auto ready = starts();
    checks.expect(WFG_MediaStartInitialization() == kWFG_StatusOk && started_nothing(ready), "the start then finds the card ready");
}

// Every way an initialisation ends, then the card's ownership, the host's
// hold and the transfers.
void test_media(Checks& checks)
{
    test_start_without_task(checks);
    test_initialization_failures(checks);
    test_host_request_after_fault(checks);
    test_initialization_while_owned(checks);
    test_host_owns_medium(checks);
    test_removal_lock(checks);
    test_write_hold(checks);
    test_host_session_end(checks);
    test_bot_reset(checks);
    test_media_local_after_release(checks);
    test_eject_latch(checks);
    test_media_local_after_eject(checks);
    test_media_host_clears_latch(checks);
    test_full_chunk(checks);
    test_short_and_last_chunks(checks);
    test_chunks_past_the_card(checks);
    test_malformed_chunks(checks);
    test_failed_chunks(checks);
    test_invalid_lun_and_buffer(checks);
    test_scsi_commands(checks);
    test_transfer_maxima(checks);
}
} // namespace

int main(int argc, char** argv)
{
    const std::span<char* const> args{argv, static_cast<std::size_t>(argc)};
    const std::string_view mode = args.size() > 1U ? std::string_view{args[1]} : std::string_view{};
    Checks checks;

    if (mode == "boot")
    {
        test_boot(checks);
    }
    else if (mode == "host-first")
    {
        test_host_first(checks);
    }
    else if (mode.empty())
    {
        test_media(checks);
    }
    else
    {
        std::fputs("usage: wfg_media_msc_<variant>_tests [boot | host-first]\n", stderr);
        return 2;
    }

    if (checks.failures() != 0)
    {
        std::fputs("MSC media tests: FAIL\n", stdout);
        return 1;
    }

    std::fputs("MSC media tests: PASS\n", stdout);
    return 0;
}

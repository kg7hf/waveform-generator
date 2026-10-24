// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The application boundary's map between a Status and what the wire and the
// operator see: the STATUS player_error and engine_error names, engine_error's
// numbers, the refusal codes a command's reply carries (a live control's from
// the live controller, a TX request's from the TX artifact owner), and the one
// live control fault ENGINE? names where the live controller gives no Status
// Guide: docs/live-control-protocol.md (Player errors, the command table)
// Contract: one row per published row, in its order (player.hpp's
// published_player_errors, which accounts for every wire number, reserved ones
// included, and published_engine_errors below), with the name the protocol
// publishes and the StatusCode of the Status that carries the error inside
// the firmware (status_of()); lookups find a row by its enumerator, so the
// same text holds on a branch that reserves numbers. The static_asserts below
// pin every row, so a renamed, renumbered or missing one fails the build;
// tests/error_map_tests.cpp pins each row's and each refusal's StatusCode,
// which the wire does not carry.
// player.cpp names errors and refuses commands through this file. The owned
// C code's statuses, which no wire carries, map to a Status in
// c_status_map.hpp beside it.

#pragma once

#include "card_listing.hpp"
#include "player.hpp"
#include "usb_audio.hpp"

#include "common/status.hpp"
#include "common/wav.hpp"
#include "signal_lab/live_control.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace waveform_generator
{

// A wire error as a Status: its enumerator, the name the protocol publishes
// (a static string, the Status's message) and the Status's code.
template <class Enum> struct ErrorName
{
    Enum value;
    const char* name;
    m110::StatusCode code;
};

inline constexpr auto player_error_names = std::to_array<ErrorName<PlayerError>>({
    {PlayerError::none, "none", m110::StatusCode::ok},
    {PlayerError::host_init, "host_init", m110::StatusCode::io_error},
    {PlayerError::card_detect, "card_detect", m110::StatusCode::unavailable},
    {PlayerError::mount, "mount", m110::StatusCode::unavailable},
    {PlayerError::open, "open", m110::StatusCode::io_error},
    {PlayerError::header_io, "header_io", m110::StatusCode::io_error},
    {PlayerError::invalid_riff, "invalid_riff", m110::StatusCode::invalid_argument},
    {PlayerError::missing_format, "missing_format", m110::StatusCode::invalid_argument},
    {PlayerError::missing_data, "missing_data", m110::StatusCode::invalid_argument},
    {PlayerError::unsupported_format, "unsupported_format", m110::StatusCode::invalid_argument},
    {PlayerError::seek, "seek", m110::StatusCode::io_error},
    {PlayerError::sd_read, "sd_read", m110::StatusCode::io_error},
    {PlayerError::codec_config, "codec_config", m110::StatusCode::io_error},
    {PlayerError::codec_level, "codec_level", m110::StatusCode::io_error},
    {PlayerError::codec_start, "codec_start", m110::StatusCode::io_error},
    {PlayerError::underrun, "underrun", m110::StatusCode::timeout},
    {PlayerError::impairment, "impairment", m110::StatusCode::internal_error},
    {PlayerError::native_source, "native_source", m110::StatusCode::internal_error},
    {PlayerError::live_control, "live_control", m110::StatusCode::internal_error},
    {PlayerError::usb_stream_closed, "usb_stream_closed", m110::StatusCode::unavailable},
    {PlayerError::usb_stall, "usb_stall", m110::StatusCode::timeout},
    {PlayerError::usb_no_stream, "usb_no_stream", m110::StatusCode::unavailable},
    {PlayerError::cut_log, "cut_log", m110::StatusCode::buffer_too_small},
    {PlayerError::missed_interval, "missed_interval", m110::StatusCode::timeout},
    {PlayerError::usb_assert, "usb_assert", m110::StatusCode::internal_error},
    {PlayerError::deadline_miss, "deadline_miss", m110::StatusCode::timeout},
    {PlayerError::usb_unconfirmed, "usb_unconfirmed", m110::StatusCode::io_error},
});

// STATUS engine_state's and engine_error's numbers, as
// docs/live-control-protocol.md's ENGINE? section publishes them
// (tests/error_map_tests.cpp compares the two).
inline constexpr auto published_engine_states = std::to_array<WireNumber<EngineState>>({
    {EngineState::none, 0U},
    {EngineState::active, 1U},
    {EngineState::refused, 2U},
    {EngineState::failed, 3U},
});

inline constexpr auto published_engine_errors = std::to_array<WireNumber<EngineError>>({
    {EngineError::none, 0U},
    {EngineError::scenario_open, 1U},
    {EngineError::scenario_read, 2U},
    {EngineError::scenario_parse, 3U},
    {EngineError::missing_reference_rms, 4U},
    {EngineError::configure_failed, 5U},
    {EngineError::scenario_too_large, 6U},
    {EngineError::clipping_rejected, 7U},
    {EngineError::processing_failed, 8U},
});

// The engine_error names: PLAYLIST_SCN_INVALID's detail is one.
inline constexpr auto engine_error_names = std::to_array<ErrorName<EngineError>>({
    {EngineError::none, "none", m110::StatusCode::ok},
    {EngineError::scenario_open, "scenario_open", m110::StatusCode::io_error},
    {EngineError::scenario_read, "scenario_read", m110::StatusCode::io_error},
    {EngineError::scenario_parse, "scenario_parse", m110::StatusCode::invalid_argument},
    {EngineError::missing_reference_rms, "missing_reference_rms", m110::StatusCode::invalid_configuration},
    {EngineError::configure_failed, "configure_failed", m110::StatusCode::invalid_configuration},
    {EngineError::scenario_too_large, "scenario_too_large", m110::StatusCode::buffer_too_small},
    {EngineError::clipping_rejected, "clipping_rejected", m110::StatusCode::invalid_configuration},
    {EngineError::processing_failed, "processing_failed", m110::StatusCode::internal_error},
});

// The Status of a value that is no enumerator, which only a cast can make.
inline constexpr m110::Status unknown_error{.code = m110::StatusCode::internal_error, .message = "unknown_error"};

// Whether `names` has a row for each row of `published`, in the same order:
// a number the published table reserves has no name row either.
template <class Enum, std::size_t Names, std::size_t Published>
[[nodiscard]] constexpr bool rows_match(const std::array<ErrorName<Enum>, Names>& names, const std::array<WireNumber<Enum>, Published>& published) noexcept
{
    return std::ranges::equal(names, published, {}, &ErrorName<Enum>::value, &WireNumber<Enum>::value);
}

// Whether no two rows of `table` share a name.
template <class Enum, std::size_t Size> [[nodiscard]] constexpr bool names_unique(const std::array<ErrorName<Enum>, Size>& table) noexcept
{
    const std::span<const ErrorName<Enum>, Size> rows{table};

    for (std::size_t index = 0U; index < Size; ++index)
    {
        const std::string_view name{rows[index].name};

        if (std::ranges::any_of(rows.subspan(index + 1U), [name](const ErrorName<Enum>& row) { return name == row.name; }))
        {
            return false;
        }
    }

    return true;
}

// Whether the first row, none, is the only success.
template <class Enum, std::size_t Size> [[nodiscard]] constexpr bool only_none_is_ok(const std::array<ErrorName<Enum>, Size>& table) noexcept
{
    const std::span<const ErrorName<Enum>, Size> rows{table};
    return rows.front().code == m110::StatusCode::ok && std::string_view{rows.front().name} == "none" &&
           std::ranges::none_of(rows.subspan(1U), [](const ErrorName<Enum>& row) { return row.code == m110::StatusCode::ok; });
}

// The Status that carries `error`: its code, and its name as the message;
// unknown_error for a value with no row (no enumerator, or a reserved number).
template <class Enum, std::size_t Size> [[nodiscard]] constexpr m110::Status status_in(const std::array<ErrorName<Enum>, Size>& table, Enum error) noexcept
{
    const auto row = std::ranges::find(table, error, &ErrorName<Enum>::value);

    if (row == table.end())
    {
        return unknown_error;
    }

    return {.code = row->code, .message = row->name};
}

// The enumerator whose Status `status` is (the same code and name), if any.
template <class Enum, std::size_t Size> [[nodiscard]] constexpr std::optional<Enum> error_in(const std::array<ErrorName<Enum>, Size>& table, m110::Status status) noexcept
{
    if (status.message == nullptr)
    {
        return std::nullopt;
    }

    const std::string_view message{status.message};
    const auto found = std::ranges::find_if(table, [message, status](const ErrorName<Enum>& row) { return row.code == status.code && message == row.name; });

    if (found == table.end())
    {
        return std::nullopt;
    }

    return found->value;
}

// Whether each row's Status maps back to its enumerator.
template <class Enum, std::size_t Size> [[nodiscard]] constexpr bool round_trips(const std::array<ErrorName<Enum>, Size>& table) noexcept
{
    return std::ranges::all_of(table, [&table](const ErrorName<Enum>& row) { return error_in(table, status_in(table, row.value)) == row.value; });
}

static_assert(rows_match(player_error_names, published_player_errors), "a name for every published player error, in its order");
// Neither table reserves a number: a row for each, 0 to the last, in order.
inline constexpr std::array<std::uint32_t, 0U> no_reserved_numbers{};
static_assert(wire_numbers_hold(published_engine_states) && numbers_accounted_for(published_engine_states, no_reserved_numbers, EngineState::failed), "the published engine states, a row for each");
static_assert(wire_numbers_hold(published_engine_errors) && numbers_accounted_for(published_engine_errors, no_reserved_numbers, EngineError::processing_failed),
              "the published engine errors, a row for each");
static_assert(rows_match(engine_error_names, published_engine_errors), "a name for every engine error, in order");
static_assert(names_unique(player_error_names) && names_unique(engine_error_names), "each name names one error");
static_assert(only_none_is_ok(player_error_names) && only_none_is_ok(engine_error_names), "none is the only error that is no failure");
static_assert(round_trips(player_error_names) && round_trips(engine_error_names), "each error's Status maps back to it");

// The Status that carries a player error or an engine error, its name the
// message: none's is a success.
[[nodiscard]] constexpr m110::Status status_of(PlayerError error) noexcept
{
    return status_in(player_error_names, error);
}

[[nodiscard]] constexpr m110::Status status_of(EngineError error) noexcept
{
    return status_in(engine_error_names, error);
}

// The name the protocol publishes for an error.
[[nodiscard]] constexpr const char* error_name(PlayerError error) noexcept
{
    return status_of(error).message;
}

[[nodiscard]] constexpr const char* error_name(EngineError error) noexcept
{
    return status_of(error).message;
}

// The player error or engine error a Status carries, if it carries one.
[[nodiscard]] constexpr std::optional<PlayerError> player_error_of(m110::Status status) noexcept
{
    return error_in(player_error_names, status);
}

[[nodiscard]] constexpr std::optional<EngineError> engine_error_of(m110::Status status) noexcept
{
    return error_in(engine_error_names, status);
}

// The player error PLAY latches for a WAV it refuses; PLAYLIST_WAV_INVALID's
// detail is its name.
struct WavPlayerError
{
    WavValidationError wav;
    PlayerError player;
};

inline constexpr auto wav_player_errors = std::to_array<WavPlayerError>({
    {WavValidationError::none, PlayerError::none},
    {WavValidationError::io, PlayerError::header_io},
    {WavValidationError::invalid_riff, PlayerError::invalid_riff},
    {WavValidationError::missing_format, PlayerError::missing_format},
    {WavValidationError::missing_data, PlayerError::missing_data},
    {WavValidationError::unsupported_format, PlayerError::unsupported_format},
});

// Whether wav_player_errors has a row for every WAV error, in order.
[[nodiscard]] constexpr bool wav_errors_in_order() noexcept
{
    std::uint32_t number = 0U;

    for (const WavPlayerError& row : wav_player_errors)
    {
        if (std::to_underlying(row.wav) != number)
        {
            return false;
        }

        ++number;
    }

    return wav_player_errors.size() == std::size_t{std::to_underlying(WavValidationError::unsupported_format)} + 1U;
}

static_assert(wav_errors_in_order(), "a player error for every WAV error, in order");

// header_io for a value that is no enumerator, as for a header that could
// not be read.
[[nodiscard]] constexpr PlayerError player_error_of(WavValidationError error) noexcept
{
    const auto found = std::ranges::find(wav_player_errors, error, &WavPlayerError::wav);
    return found == wav_player_errors.end() ? PlayerError::header_io : found->player;
}

// The refusals a command's reply carries (ControlReply::error: WFG-LIVE/1's
// error code, the legacy ERR line's word), as Statuses whose message is the
// code. CUT's and PLAYLIST's own refusals are cut_schedule.hpp's and
// playlist.hpp's; NO_STREAM is usb_audio::stream_not_open. BAD_REQUEST is the
// parser's refusal of a line (live_protocol::parse(), whose reply the USB
// service sends), which the player gives too.
namespace refusal
{

inline constexpr m110::Status busy{.code = m110::StatusCode::busy, .message = "BUSY"};
inline constexpr m110::Status bad_state{.code = m110::StatusCode::unavailable, .message = "BAD_STATE"};
inline constexpr m110::Status load_required{.code = m110::StatusCode::unavailable, .message = "LOAD_REQUIRED"};
inline constexpr m110::Status run_id_reused{.code = m110::StatusCode::invalid_argument, .message = "RUN_ID_REUSED"};
inline constexpr m110::Status bad_request = live_protocol::bad_request;
inline constexpr m110::Status host_owned{.code = m110::StatusCode::unavailable, .message = "HOST_OWNED"};
inline constexpr m110::Status files_mount{.code = m110::StatusCode::io_error, .message = "FILES_MOUNT"};
inline constexpr m110::Status files_unmount{.code = m110::StatusCode::io_error, .message = "FILES_UNMOUNT"};
inline constexpr m110::Status files_page{.code = m110::StatusCode::internal_error, .message = "FILES_PAGE"};

} // namespace refusal

// A refusal code as docs/live-control-protocol.md publishes it, beside the
// Status that carries it.
struct PublishedRefusal
{
    m110::Status status;
    std::string_view code;
};

inline constexpr auto published_refusals = std::to_array<PublishedRefusal>({
    {refusal::busy, "BUSY"},
    {refusal::bad_state, "BAD_STATE"},
    {refusal::load_required, "LOAD_REQUIRED"},
    {refusal::run_id_reused, "RUN_ID_REUSED"},
    {refusal::bad_request, "BAD_REQUEST"},
    {usb_audio::stream_not_open, "NO_STREAM"},
    {refusal::host_owned, "HOST_OWNED"},
    {refusal::files_mount, "FILES_MOUNT"},
    {refusal::files_unmount, "FILES_UNMOUNT"},
    {card_listing::open_failed, "FILES_OPEN"},
    {card_listing::read_failed, "FILES_READ"},
    {card_listing::close_failed, "FILES_CLOSE"},
    {card_listing::invalid_name, "FILES_NAME"},
    {card_listing::invalid_offset, "FILES_RANGE"},
    {refusal::files_page, "FILES_PAGE"},
});

static_assert(std::ranges::all_of(published_refusals, [](const PublishedRefusal& row) { return !row.status.is_ok() && row.code == row.status.message; }),

              "each refusal carries its published code");
static_assert(std::string_view{ControlReply{}.error} == refusal::bad_state.message, "a reply that no command answered is BAD_STATE");

// A refused live control's code (apply_live_command()): the live
// controller's refusal, which docs/live-control-protocol.md's Live commands
// section publishes in this order. The player's BUSY and BAD_STATE come first.
inline constexpr auto published_control_refusals = std::to_array<PublishedRefusal>({
    {signal_lab::control_refusal::not_configured, "not_configured"},
    {signal_lab::control_refusal::invalid_event, "invalid_event"},
    {signal_lab::control_refusal::late_event, "late_event"},
    {signal_lab::control_refusal::pending_full, "pending_full"},
    {signal_lab::control_refusal::capture_full, "capture_full"},
});

// Whether `table` publishes each of `refusals` with its message, in their
// order, and nothing else.
template <std::size_t Rows, std::size_t Refusals>
[[nodiscard]] constexpr bool publishes_in_order(const std::array<PublishedRefusal, Rows>& table, const std::array<m110::Status, Refusals>& refusals) noexcept
{
    return std::ranges::equal(table, refusals, [](const PublishedRefusal& row, m110::Status refusal) { return !refusal.is_ok() && row.status.code == refusal.code && row.code == refusal.message; });
}

// Whether no two rows of `table` publish one code.
template <std::size_t Rows> [[nodiscard]] constexpr bool codes_unique(const std::array<PublishedRefusal, Rows>& table) noexcept
{
    const std::span<const PublishedRefusal, Rows> rows{table};

    for (std::size_t index = 0U; index < Rows; ++index)
    {
        const std::string_view code = rows[index].code;

        if (std::ranges::any_of(rows.subspan(index + 1U), [code](const PublishedRefusal& row) { return row.code == code; }))
        {
            return false;
        }
    }

    return true;
}

static_assert(publishes_in_order(published_control_refusals, signal_lab::control_refusal::all), "a published code for every refusal of the live controller, its message, in order");
static_assert(codes_unique(published_control_refusals) && codes_unique(published_refusals), "each code names one refusal");
static_assert(std::ranges::none_of(published_control_refusals,
                                   [](const PublishedRefusal& row) { return std::ranges::any_of(published_refusals, [&row](const PublishedRefusal& player) { return player.code == row.code; }); }),

              "a live control refusal is none of the player's");

// The TX artifact owner's refusals (tx_artifact.cpp): the code a TX reply's
// error and STATUS's TX error carry is the message. A card error gives the
// code of the refusal it stands in for, under its own StatusCode:
// media_failed reads MEDIA_NOT_LOCAL, output_unchecked OUTPUT_EXISTS. BUSY
// and BAD_REQUEST are refusal::busy and refusal::bad_request; the owner's
// other codes are the WAV writer's or an encoder's own message.
namespace tx_refusal
{

inline constexpr m110::Status bad_mode{.code = m110::StatusCode::invalid_argument, .message = "BAD_MODE"};
inline constexpr m110::Status upload_in_progress{.code = m110::StatusCode::unavailable, .message = "UPLOAD_IN_PROGRESS"};
inline constexpr m110::Status bad_filename{.code = m110::StatusCode::invalid_argument, .message = "BAD_FILENAME"};
inline constexpr m110::Status select_new_file{.code = m110::StatusCode::unavailable, .message = "SELECT_NEW_FILE"};
inline constexpr m110::Status payload_too_large{.code = m110::StatusCode::buffer_too_small, .message = "PAYLOAD_TOO_LARGE"};
inline constexpr m110::Status media_not_local{.code = m110::StatusCode::unavailable, .message = "MEDIA_NOT_LOCAL"};
inline constexpr m110::Status media_failed{.code = m110::StatusCode::io_error, .message = "MEDIA_NOT_LOCAL"};
inline constexpr m110::Status payload_exists_or_io_error{.code = m110::StatusCode::io_error, .message = "PAYLOAD_EXISTS_OR_IO_ERROR"};
inline constexpr m110::Status payload_write_error{.code = m110::StatusCode::io_error, .message = "PAYLOAD_WRITE_ERROR"};
inline constexpr m110::Status no_uploaded_payload{.code = m110::StatusCode::unavailable, .message = "NO_UPLOADED_PAYLOAD"};
inline constexpr m110::Status unknown_encoder{.code = m110::StatusCode::invalid_argument, .message = "UNKNOWN_ENCODER"};
inline constexpr m110::Status output_exists{.code = m110::StatusCode::invalid_argument, .message = "OUTPUT_EXISTS"};
inline constexpr m110::Status output_unchecked{.code = m110::StatusCode::io_error, .message = "OUTPUT_EXISTS"};
inline constexpr m110::Status bad_payload_file{.code = m110::StatusCode::invalid_argument, .message = "BAD_PAYLOAD_FILE"};
inline constexpr m110::Status encoder_workspace_error{.code = m110::StatusCode::internal_error, .message = "ENCODER_WORKSPACE_ERROR"};
inline constexpr m110::Status artifact_finalize_error{.code = m110::StatusCode::io_error, .message = "ARTIFACT_FINALIZE_ERROR"};

} // namespace tx_refusal

// The TX codes beside the Statuses that carry them.
inline constexpr auto published_tx_refusals = std::to_array<PublishedRefusal>({
    {tx_refusal::bad_mode, "BAD_MODE"},
    {tx_refusal::upload_in_progress, "UPLOAD_IN_PROGRESS"},
    {tx_refusal::bad_filename, "BAD_FILENAME"},
    {tx_refusal::select_new_file, "SELECT_NEW_FILE"},
    {tx_refusal::payload_too_large, "PAYLOAD_TOO_LARGE"},
    {tx_refusal::media_not_local, "MEDIA_NOT_LOCAL"},
    {tx_refusal::media_failed, "MEDIA_NOT_LOCAL"},
    {tx_refusal::payload_exists_or_io_error, "PAYLOAD_EXISTS_OR_IO_ERROR"},
    {tx_refusal::payload_write_error, "PAYLOAD_WRITE_ERROR"},
    {tx_refusal::no_uploaded_payload, "NO_UPLOADED_PAYLOAD"},
    {tx_refusal::unknown_encoder, "UNKNOWN_ENCODER"},
    {tx_refusal::output_exists, "OUTPUT_EXISTS"},
    {tx_refusal::output_unchecked, "OUTPUT_EXISTS"},
    {tx_refusal::bad_payload_file, "BAD_PAYLOAD_FILE"},
    {tx_refusal::encoder_workspace_error, "ENCODER_WORKSPACE_ERROR"},
    {tx_refusal::artifact_finalize_error, "ARTIFACT_FINALIZE_ERROR"},
});

static_assert(std::ranges::all_of(published_tx_refusals, [](const PublishedRefusal& row) { return !row.status.is_ok() && row.code == row.status.message; }),

              "each TX refusal carries its code");

// Why the live controls stop a play (player_error live_control) where the
// live controller gives no Status of its own: a control queued before PLAY
// with no reference RMS to scale it by. ENGINE? reports it
// (player_live_reason()).
namespace live_fault
{

inline constexpr m110::Status no_reference{.code = m110::StatusCode::invalid_configuration,
                                           .message = "live_control: a control queued before PLAY needs a reference RMS in (0, 1] "
                                                      "(REFERENCE, or a card that is not silent)"};

} // namespace live_fault

// A refused command's reply: not ok, and its error is `why`'s message, the
// code the wire carries.
constexpr void refuse(ControlReply& reply, m110::Status why) noexcept
{
    reply.ok = false;
    reply.error = why.message;
}

} // namespace waveform_generator

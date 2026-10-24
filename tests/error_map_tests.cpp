// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// rt1170/error_map.hpp against the protocol document it publishes for: every
// player error's name and number in docs/live-control-protocol.md's Player
// errors table, the engine_error and WAV names a PLAYLIST refusal's detail
// documents, each refusal code, the Live commands section's table of the live
// controller's refusals, and the ENGINE? section's engine_state and
// engine_error numbers. Then the lookups the player uses: a
// Status for each error and back, a value that is no enumerator, a Status
// that carries no error, and refuse().
//
// Then the owned C code's statuses (c_status_map.hpp), which no wire carries:
// each value's Status and back.
//
// Usage: wfg_error_map_tests <docs/live-control-protocol.md>

#include "c_status_map.hpp"
#include "error_map.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace wg = waveform_generator;

// numbers_accounted_for() on signal-lab-phase-2's table, which has no
// PLAYLIST and no looped items: the published rows without 21 and 23 to 25,
// which that branch reserves. The same text gives that table on either
// branch, whether its own table has those rows or not.
template <std::size_t Reserved> [[nodiscard]] constexpr bool is_reserved(const std::array<std::uint32_t, Reserved>& reserved, const wg::WireNumber<wg::PlayerError>& row) noexcept
{
    return std::ranges::find(reserved, row.number) != reserved.end();
}

template <std::size_t Reserved> [[nodiscard]] constexpr std::size_t rows_kept(const std::array<std::uint32_t, Reserved>& reserved) noexcept
{
    std::size_t kept = 0U;

    for (const auto& row : wg::published_player_errors)
    {
        kept += is_reserved(reserved, row) ? 0U : 1U;
    }

    return kept;
}

template <auto Reserved> [[nodiscard]] constexpr auto rows_without() noexcept
{
    std::array<wg::WireNumber<wg::PlayerError>, rows_kept(Reserved)> rows{};
    std::ranges::copy_if(wg::published_player_errors, rows.begin(), [](const wg::WireNumber<wg::PlayerError>& row) { return !is_reserved(Reserved, row); });

    return rows;
}

constexpr auto last_error = wg::PlayerError::usb_unconfirmed;
// Phase 2 publishes 0 to 20, 22 and 26.
constexpr std::size_t phase_2_row_count = 23U;
constexpr auto phase_2_reserved = std::to_array<std::uint32_t>({21U, 23U, 24U, 25U});

constexpr auto phase_2_rows = rows_without<phase_2_reserved>();
// Reserved numbers that do not fit phase 2's rows.
constexpr auto one_gap_unclaimed = std::to_array<std::uint32_t>({21U, 23U, 24U});

constexpr auto cut_log_reserved_too = std::to_array<std::uint32_t>({21U, 22U, 23U, 24U, 25U});

constexpr auto one_past_the_last = std::to_array<std::uint32_t>({21U, 23U, 24U, 25U, 27U});

constexpr auto out_of_order = std::to_array<std::uint32_t>({23U, 21U, 24U, 25U});

// Phase 2's rows without the last error's too.
constexpr auto without_the_last = std::to_array<std::uint32_t>({21U, 23U, 24U, 25U, 26U});

// The same rows with their first two exchanged.
[[nodiscard]] constexpr auto phase_2_rows_out_of_order() noexcept
{
    auto rows = phase_2_rows;
    std::swap(rows[0], rows[1]);
    return rows;
}

static_assert(phase_2_rows.size() == phase_2_row_count && wg::numbers_accounted_for(phase_2_rows, phase_2_reserved, last_error), "phase 2's 23 rows and its four reserved numbers account for 0 to 26");
static_assert(!wg::numbers_accounted_for(phase_2_rows, one_gap_unclaimed, last_error), "a number neither published nor reserved fails");
static_assert(!wg::numbers_accounted_for(phase_2_rows, cut_log_reserved_too, last_error), "a number both published and reserved fails");
static_assert(!wg::numbers_accounted_for(phase_2_rows, one_past_the_last, last_error), "a reserved number past the last error's fails");
static_assert(!wg::numbers_accounted_for(phase_2_rows, out_of_order, last_error), "reserved numbers out of order fail");
static_assert(!wg::numbers_accounted_for(phase_2_rows_out_of_order(), phase_2_reserved, last_error), "rows out of order fail");
static_assert(!wg::numbers_accounted_for(phase_2_rows, phase_2_reserved, wg::PlayerError::cut_log), "a last row that is not the named last error fails");
static_assert(!wg::numbers_accounted_for(rows_without<without_the_last>(), phase_2_reserved, last_error), "a table that stops before the last error fails");
static_assert(!wg::numbers_accounted_for(std::array<wg::WireNumber<wg::PlayerError>, 0U>{}, std::array<std::uint32_t, 0U>{}, last_error), "an empty table fails");

// error_map.hpp on phase 2's tables: the name rows of the published rows, the
// reserved numbers left out, as that branch's player_error_names has them.
template <std::size_t Reserved> [[nodiscard]] constexpr bool name_reserved(const std::array<std::uint32_t, Reserved>& reserved, const wg::ErrorName<wg::PlayerError>& row) noexcept
{
    return std::ranges::find(reserved, std::to_underlying(row.value)) != reserved.end();
}

template <auto Reserved> [[nodiscard]] constexpr auto names_without() noexcept
{
    std::array<wg::ErrorName<wg::PlayerError>, rows_kept(Reserved)> names{};
    std::ranges::copy_if(wg::player_error_names, names.begin(), [](const wg::ErrorName<wg::PlayerError>& row) { return !name_reserved(Reserved, row); });

    return names;
}

constexpr auto phase_2_names = names_without<phase_2_reserved>();

[[nodiscard]] constexpr auto phase_2_names_out_of_order() noexcept
{
    auto names = phase_2_names;
    std::swap(names[1], names[2]);
    return names;
}

[[nodiscard]] constexpr bool is(m110::Status status, m110::StatusCode code, std::string_view name) noexcept
{
    return status.code == code && status.message != nullptr && std::string_view{status.message} == name;
}

static_assert(wg::rows_match(phase_2_names, phase_2_rows), "phase 2's 23 name rows match its published rows, 21 and 23 to 25 left out");
static_assert(is(wg::status_in(phase_2_names, wg::PlayerError::cut_log), m110::StatusCode::buffer_too_small, "cut_log") &&
                  is(wg::status_in(phase_2_names, wg::PlayerError::usb_unconfirmed), m110::StatusCode::io_error, "usb_unconfirmed") &&
                  is(wg::status_in(phase_2_names, wg::PlayerError::usb_stall), m110::StatusCode::timeout, "usb_stall"),
              "a lookup in phase 2's table finds each error's own row past the gaps");
// A reserved number as the value it would be: phase 2 has no enumerator for
// it, so the same text names none here either.
[[nodiscard]] constexpr bool every_reserved_number_unknown() noexcept
{
    return std::ranges::all_of(phase_2_reserved,
                               [](std::uint32_t number) { return is(wg::status_in(phase_2_names, static_cast<wg::PlayerError>(number)), wg::unknown_error.code, wg::unknown_error.message); });
}

static_assert(every_reserved_number_unknown(), "a reserved number has no row: unknown_error");
static_assert(wg::error_in(phase_2_names, m110::Status{.code = m110::StatusCode::io_error, .message = "usb_unconfirmed"}) == wg::PlayerError::usb_unconfirmed && wg::round_trips(phase_2_names),
              "phase 2's Statuses map back to their errors");
static_assert(!wg::rows_match(phase_2_names_out_of_order(), phase_2_rows) && !wg::rows_match(names_without<without_the_last>(), phase_2_rows) &&
                  !wg::rows_match(phase_2_names, rows_without<without_the_last>()),
              "name rows out of order, a name row short or a published row short fail");

// The StatusCode of every error row and refusal (QA's step-1 check on #50).
// The wire carries the name or the number, never the code, so the protocol
// document does not list it; the code is what the firmware's own callers and
// the critical-error record see, and these tables are where a changed one
// shows. By name: every row a branch's table has must be here with its code,
// so the same text holds on phase 2's table, which has no rows 21 and 23 to 25.
struct PinnedCode
{
    std::string_view name;
    m110::StatusCode code;
};

constexpr auto player_error_codes = std::to_array<PinnedCode>({
    {"none", m110::StatusCode::ok},
    {"host_init", m110::StatusCode::io_error},
    {"card_detect", m110::StatusCode::unavailable},
    {"mount", m110::StatusCode::unavailable},
    {"open", m110::StatusCode::io_error},
    {"header_io", m110::StatusCode::io_error},
    {"invalid_riff", m110::StatusCode::invalid_argument},
    {"missing_format", m110::StatusCode::invalid_argument},
    {"missing_data", m110::StatusCode::invalid_argument},
    {"unsupported_format", m110::StatusCode::invalid_argument},
    {"seek", m110::StatusCode::io_error},
    {"sd_read", m110::StatusCode::io_error},
    {"codec_config", m110::StatusCode::io_error},
    {"codec_level", m110::StatusCode::io_error},
    {"codec_start", m110::StatusCode::io_error},
    {"underrun", m110::StatusCode::timeout},
    {"impairment", m110::StatusCode::internal_error},
    {"native_source", m110::StatusCode::internal_error},
    {"live_control", m110::StatusCode::internal_error},
    {"usb_stream_closed", m110::StatusCode::unavailable},
    {"usb_stall", m110::StatusCode::timeout},
    {"usb_no_stream", m110::StatusCode::unavailable},
    {"cut_log", m110::StatusCode::buffer_too_small},
    {"missed_interval", m110::StatusCode::timeout},
    {"usb_assert", m110::StatusCode::internal_error},
    {"deadline_miss", m110::StatusCode::timeout},
    {"usb_unconfirmed", m110::StatusCode::io_error},
});

constexpr auto engine_error_codes = std::to_array<PinnedCode>({
    {"none", m110::StatusCode::ok},
    {"scenario_open", m110::StatusCode::io_error},
    {"scenario_read", m110::StatusCode::io_error},
    {"scenario_parse", m110::StatusCode::invalid_argument},
    {"missing_reference_rms", m110::StatusCode::invalid_configuration},
    {"configure_failed", m110::StatusCode::invalid_configuration},
    {"scenario_too_large", m110::StatusCode::buffer_too_small},
    {"clipping_rejected", m110::StatusCode::invalid_configuration},
    {"processing_failed", m110::StatusCode::internal_error},
});

constexpr auto refusal_codes = std::to_array<PinnedCode>({
    {"BUSY", m110::StatusCode::busy},
    {"BAD_STATE", m110::StatusCode::unavailable},
    {"LOAD_REQUIRED", m110::StatusCode::unavailable},
    {"RUN_ID_REUSED", m110::StatusCode::invalid_argument},
    {"BAD_REQUEST", m110::StatusCode::invalid_argument},
    {"NO_STREAM", m110::StatusCode::unavailable},
    {"HOST_OWNED", m110::StatusCode::unavailable},
    {"FILES_MOUNT", m110::StatusCode::io_error},
    {"FILES_UNMOUNT", m110::StatusCode::io_error},
    {"FILES_OPEN", m110::StatusCode::io_error},
    {"FILES_READ", m110::StatusCode::io_error},
    {"FILES_CLOSE", m110::StatusCode::io_error},
    {"FILES_NAME", m110::StatusCode::invalid_argument},
    {"FILES_RANGE", m110::StatusCode::invalid_argument},
    {"FILES_PAGE", m110::StatusCode::internal_error},
});

// Whether `pinned` names `name` with `code`.
template <std::size_t Size> [[nodiscard]] constexpr bool pinned_as(const std::array<PinnedCode, Size>& pinned, std::string_view name, m110::StatusCode code) noexcept
{
    const auto found = std::ranges::find(pinned, name, &PinnedCode::name);
    return found != pinned.end() && found->code == code;
}

template <class Enum, std::size_t Size, std::size_t Pinned>
[[nodiscard]] constexpr bool codes_pinned(const std::array<wg::ErrorName<Enum>, Size>& table, const std::array<PinnedCode, Pinned>& pinned) noexcept
{
    return std::ranges::all_of(table, [&pinned](const wg::ErrorName<Enum>& row) { return pinned_as(pinned, row.name, row.code); });
}

static_assert(codes_pinned(wg::player_error_names, player_error_codes) && codes_pinned(phase_2_names, player_error_codes), "every player error carries its pinned StatusCode");
static_assert(codes_pinned(wg::engine_error_names, engine_error_codes) && engine_error_codes.size() == wg::engine_error_names.size(),
              "every engine error carries its pinned StatusCode, and each pinned one is a row");
static_assert(std::ranges::all_of(wg::published_refusals, [](const wg::PublishedRefusal& row) { return pinned_as(refusal_codes, row.code, row.status.code); }) &&

                  refusal_codes.size() == wg::published_refusals.size(),
              "every refusal carries its pinned StatusCode, and each pinned one is a refusal");

constexpr auto control_refusal_codes = std::to_array<PinnedCode>({
    {"not_configured", m110::StatusCode::unavailable},
    {"invalid_event", m110::StatusCode::invalid_argument},
    {"late_event", m110::StatusCode::timeout},
    {"pending_full", m110::StatusCode::busy},
    {"capture_full", m110::StatusCode::buffer_too_small},
});

static_assert(std::ranges::all_of(wg::published_control_refusals, [](const wg::PublishedRefusal& row) { return pinned_as(control_refusal_codes, row.code, row.status.code); }) &&

                  control_refusal_codes.size() == wg::published_control_refusals.size(),
              "every live control refusal carries its pinned StatusCode, and each pinned one is a refusal");
// A changed code fails: usb_stall as io_error, BUSY as timeout.
static_assert(!pinned_as(player_error_codes, "usb_stall", m110::StatusCode::io_error) && !pinned_as(refusal_codes, "BUSY", m110::StatusCode::timeout) &&
                  !pinned_as(player_error_codes, "no_such_error", m110::StatusCode::ok),
              "a code other than the pinned one, or a name with no pin, fails");

// The TX artifact owner's refusals, row by row in published_tx_refusals'
// order: two pairs share a wire code and differ only by StatusCode, so a pin
// by name could not tell them apart. MEDIA_NOT_LOCAL is a card that is not
// local (unavailable) or one that failed (io_error); OUTPUT_EXISTS an output
// that exists (invalid_argument) or one that could not be checked (io_error).
constexpr auto tx_refusal_codes = std::to_array<PinnedCode>({
    {"BAD_MODE", m110::StatusCode::invalid_argument},
    {"UPLOAD_IN_PROGRESS", m110::StatusCode::unavailable},
    {"BAD_FILENAME", m110::StatusCode::invalid_argument},
    {"SELECT_NEW_FILE", m110::StatusCode::unavailable},
    {"PAYLOAD_TOO_LARGE", m110::StatusCode::buffer_too_small},
    {"MEDIA_NOT_LOCAL", m110::StatusCode::unavailable},
    {"MEDIA_NOT_LOCAL", m110::StatusCode::io_error},
    {"PAYLOAD_EXISTS_OR_IO_ERROR", m110::StatusCode::io_error},
    {"PAYLOAD_WRITE_ERROR", m110::StatusCode::io_error},
    {"NO_UPLOADED_PAYLOAD", m110::StatusCode::unavailable},
    {"UNKNOWN_ENCODER", m110::StatusCode::invalid_argument},
    {"OUTPUT_EXISTS", m110::StatusCode::invalid_argument},
    {"OUTPUT_EXISTS", m110::StatusCode::io_error},
    {"BAD_PAYLOAD_FILE", m110::StatusCode::invalid_argument},
    {"ENCODER_WORKSPACE_ERROR", m110::StatusCode::internal_error},
    {"ARTIFACT_FINALIZE_ERROR", m110::StatusCode::io_error},
});

// Whether `table` has `pinned`'s rows, in its order: each row's wire code and
// StatusCode.
template <std::size_t Rows, std::size_t Pinned> [[nodiscard]] constexpr bool rows_pinned(const std::array<wg::PublishedRefusal, Rows>& table, const std::array<PinnedCode, Pinned>& pinned) noexcept
{
    return Rows == Pinned && std::ranges::equal(table, pinned, [](const wg::PublishedRefusal& row, const PinnedCode& pin) { return row.code == pin.name && row.status.code == pin.code; });
}

static_assert(rows_pinned(wg::published_tx_refusals, tx_refusal_codes), "every TX refusal carries its pinned wire code and StatusCode, in order");

// The same pins with one pair's StatusCodes swapped, and with a row short.
constexpr auto tx_pair_swapped = []
{
    auto rows = wg::published_tx_refusals;
    constexpr std::size_t media_not_local_row = 5U;
    std::swap(rows.at(media_not_local_row), rows.at(media_not_local_row + 1U));
    return rows;
}();
constexpr auto tx_row_short = []
{
    std::array<wg::PublishedRefusal, wg::published_tx_refusals.size() - 1U> rows{};
    std::ranges::copy_n(wg::published_tx_refusals.begin(), rows.size(), rows.begin());
    return rows;
}();
static_assert(!rows_pinned(tx_pair_swapped, tx_refusal_codes) && !rows_pinned(tx_row_short, tx_refusal_codes), "a pair's StatusCodes swapped, or a row missing, fails");

// Why the live controls stopped a play when the controller gave no Status:
// an invalid configuration (no reference RMS), which ENGINE? reports.
static_assert(wg::live_fault::no_reference.code == m110::StatusCode::invalid_configuration, "live_fault::no_reference carries its pinned StatusCode");

void check(bool condition, const std::string& description)
{
    INFO(description);
    CHECK(condition);
}

struct DocumentedError
{
    std::string name;
    std::uint32_t number;

    [[nodiscard]] bool operator==(const DocumentedError&) const = default;
};

[[nodiscard]] std::optional<std::string> read_text(const std::string_view path)
{
    std::ifstream file{std::string{path}};

    if (!file)
    {
        return std::nullopt;
    }

    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

// The `name`s of a table cell or sentence, in order.
[[nodiscard]] std::vector<std::string> quoted_names(std::string_view text)
{
    std::vector<std::string> names;

    for (std::size_t open = text.find('`'); open != std::string_view::npos; open = text.find('`', open))
    {
        const std::size_t close = text.find('`', open + 1U);

        if (close == std::string_view::npos)
        {
            break;
        }

        names.emplace_back(text.substr(open + 1U, close - open - 1U));
        open = close + 1U;
    }

    return names;
}

// A row's code cell: one number ("3"), a list ("1, 2") or a range ("5 to 9").
[[nodiscard]] std::vector<std::uint32_t> numbers_of(std::string_view cell)
{
    std::vector<std::uint32_t> numbers;
    constexpr std::string_view list_separator{", "};
    constexpr std::string_view range_separator{" to "};
    constexpr std::uint32_t decimal_radix = 10U;
    const auto number = [](std::string_view digits)
    {
        std::uint32_t value = 0U;

        for (const char digit : digits)
        {
            value = value * decimal_radix + static_cast<std::uint32_t>(digit - '0');
        }

        return value;
    };

    while (!cell.empty())
    {
        const std::size_t end = cell.find(list_separator);
        const std::string_view part = cell.substr(0U, end);
        const std::size_t range = part.find(range_separator);

        if (range == std::string_view::npos)
        {
            numbers.push_back(number(part));
        }
        else
        {
            for (std::uint32_t value = number(part.substr(0U, range)); value <= number(part.substr(range + range_separator.size())); ++value)
            {
                numbers.push_back(value);
            }
        }

        cell = end == std::string_view::npos ? std::string_view{} : cell.substr(end + list_separator.size());
    }

    return numbers;
}

// The Player errors table: each row's code cell and its names, paired in
// order. A row whose cells disagree in count names no error.
[[nodiscard]] std::vector<DocumentedError> documented_player_errors(std::string_view document)
{
    constexpr std::string_view heading{"### Player errors\n"};
    const std::size_t start = document.find(heading);
    const std::size_t end = document.find("\n## ", start);

    if (start == std::string_view::npos || end == std::string_view::npos)
    {
        return {};
    }

    std::vector<DocumentedError> errors;
    std::string_view section = document.substr(start, end - start);

    while (!section.empty())
    {
        const std::size_t line_end = section.find('\n');
        const std::string_view line = section.substr(0U, line_end);
        section = line_end == std::string_view::npos ? std::string_view{} : section.substr(line_end + 1U);
        constexpr std::string_view cell_separator{" | "};
        const std::size_t code_end = line.find(cell_separator, 2U);

        if (!line.starts_with("| ") || line.size() < 3U || line[2] < '0' || line[2] > '9' || code_end == std::string_view::npos)
        {
            continue;
        }

        const std::string_view rest = line.substr(code_end + cell_separator.size());
        const auto numbers = numbers_of(line.substr(2U, code_end - 2U));
        const auto names = quoted_names(rest.substr(0U, rest.find(cell_separator)));

        if (numbers.size() != names.size())
        {
            return {};
        }

        for (std::size_t index = 0U; index < names.size(); ++index)
        {
            errors.push_back({.name = names[index], .number = numbers[index]});
        }
    }

    return errors;
}

// The names a refusal table row lists after `marker`, up to the row's end.
[[nodiscard]] std::vector<std::string> names_after(std::string_view document, std::string_view row_start, std::string_view marker)
{
    const std::size_t row = document.find(row_start);

    if (row == std::string_view::npos)
    {
        return {};
    }

    const std::string_view line = document.substr(row, document.find('\n', row) - row);
    const std::size_t at = line.find(marker);
    return at == std::string_view::npos ? std::vector<std::string>{} : quoted_names(line.substr(at + marker.size()));
}

[[nodiscard]] constexpr bool is_digit(char character) noexcept
{
    return character >= '0' && character <= '9';
}

// The "N `name`" pairs of `text`, in order: a number, a space and a quoted
// name ("3 `scenario_parse`"); a number followed by anything else is none.
[[nodiscard]] std::vector<DocumentedError> numbered_names(std::string_view text)
{
    std::vector<DocumentedError> found;
    constexpr std::string_view opening{" `"};

    for (std::size_t at = 0U; at < text.size(); ++at)
    {
        if (!is_digit(text[at]) || (at > 0U && is_digit(text[at - 1U])))
        {
            continue;
        }

        std::size_t end = at;

        while (end < text.size() && is_digit(text[end]))
        {
            ++end;
        }

        const std::size_t close = text.find('`', end + opening.size());

        if (text.substr(end, opening.size()) != opening || close == std::string_view::npos)
        {
            continue;
        }

        found.push_back({.name = std::string{text.substr(end + opening.size(), close - end - opening.size())}, .number = numbers_of(text.substr(at, end - at)).front()});
        at = close;
    }

    return found;
}

// The ENGINE? section's engine_state and engine_error numbers against the
// firmware's published tables: each documented as "N `name`", the errors by
// the error map's names, the states by the protocol's words for them.
void test_documented_engine_numbers(std::string_view document)
{
    constexpr std::string_view row_start{"| `engine_state`, `engine_error` |"};
    constexpr std::string_view states_marker{"`engine_state`:"};
    constexpr std::string_view errors_marker{"`engine_error`:"};
    const std::size_t row = document.find(row_start);
    const std::string_view line = row == std::string_view::npos ? std::string_view{} : document.substr(row, document.find('\n', row) - row);
    const std::size_t states_at = line.find(states_marker);
    const std::size_t errors_at = line.find(errors_marker);

    if (states_at == std::string_view::npos || errors_at == std::string_view::npos || errors_at < states_at)
    {
        check(false, "ENGINE? has its engine_state and engine_error row");
        return;
    }

    std::vector<DocumentedError> errors;
    std::ranges::transform(wg::published_engine_errors, std::back_inserter(errors),
                           [](const auto& published) { return DocumentedError{.name = wg::error_name(published.value), .number = published.number}; });

    check(numbered_names(line.substr(errors_at)) == errors, "ENGINE? documents every engine_error by the firmware's number and name");
    constexpr auto state_words = std::to_array<std::pair<wg::EngineState, std::string_view>>(
        {{wg::EngineState::none, "none"}, {wg::EngineState::active, "active"}, {wg::EngineState::refused, "refused"}, {wg::EngineState::failed, "failed"}});
    std::vector<DocumentedError> states;

    for (const auto& published : wg::published_engine_states)
    {
        const auto word = std::ranges::find(state_words, published.value, &std::pair<wg::EngineState, std::string_view>::first);
        states.push_back({.name = word == state_words.end() ? std::string{} : std::string{word->second}, .number = published.number});
    }

    check(numbered_names(line.substr(states_at, errors_at - states_at)) == states, "ENGINE? documents every engine_state by the firmware's number");
}

template <class Enum, std::size_t Size> [[nodiscard]] std::optional<Enum> named(const std::array<wg::ErrorName<Enum>, Size>& table, std::string_view name)
{
    const auto found = std::ranges::find_if(table, [name](const wg::ErrorName<Enum>& row) { return name == row.name; });

    return found == table.end() ? std::nullopt : std::optional<Enum>{found->value};
}

void test_documented_player_errors(std::string_view document)
{
    const auto documented = documented_player_errors(document);
    check(documented.size() == wg::player_error_names.size(), "the Player errors table documents every player error");

    for (const DocumentedError& error : documented)
    {
        const auto value = named(wg::player_error_names, error.name);
        check(value.has_value() && std::to_underlying(*value) == error.number, "player error " + error.name + " is " + std::to_string(error.number) + ", by name and number");
    }
}

void test_documented_details(std::string_view document)
{
    // PLAYLIST_SCN_INVALID's detail: the engine_error names PLAY rejects a
    // scenario with.
    const auto engine = names_after(document, "| `PLAYLIST_SCN_INVALID` |", "the `engine_error` name:");
    constexpr auto rejections = std::to_array<wg::EngineError>({wg::EngineError::scenario_open, wg::EngineError::scenario_read, wg::EngineError::scenario_parse, wg::EngineError::missing_reference_rms,
                                                                wg::EngineError::configure_failed, wg::EngineError::scenario_too_large});
    std::vector<std::string> engine_names;
    std::ranges::transform(rejections, std::back_inserter(engine_names), [](wg::EngineError error) { return std::string{wg::error_name(error)}; });

    check(engine == engine_names, "PLAYLIST_SCN_INVALID documents the scenario rejections by the map's names");
    // PLAYLIST_WAV_INVALID's detail: the player error PLAY would latch.
    const auto wav = names_after(document, "| `PLAYLIST_WAV_INVALID` |", "the [player error](#player-errors) it would latch:");
    std::vector<std::string> wav_names;
    std::ranges::transform(std::span{wg::wav_player_errors}.subspan(1U), std::back_inserter(wav_names),
                           [](const wg::WavPlayerError& row) { return std::string{wg::error_name(wg::player_error_of(row.wav))}; });

    check(wav == wav_names, "PLAYLIST_WAV_INVALID documents every WAV refusal by the map's names");
}

void test_documented_refusals(std::string_view document)
{
    for (const wg::PublishedRefusal& refusal : wg::published_refusals)
    {
        const std::string quoted = "`" + std::string{refusal.code} + "`";
        check(document.find(quoted) != std::string_view::npos, "the protocol document names refusal " + quoted);
    }
}

// The Live commands section's table of the live controller's refusals: the
// code each row names, in order; none when a row names no single code.
[[nodiscard]] std::vector<std::string> documented_control_refusals(std::string_view document)
{
    constexpr std::string_view header{"| Live control refusal | Why |\n"};
    constexpr std::string_view row_start{"| `"};
    constexpr std::string_view cell_separator{" | "};
    const std::size_t start = document.find(header);

    if (start == std::string_view::npos)
    {
        return {};
    }

    std::vector<std::string> codes;
    std::string_view rest = document.substr(start + header.size());

    while (!rest.empty())
    {
        const std::size_t line_end = rest.find('\n');
        const std::string_view line = rest.substr(0U, line_end);
        rest = line_end == std::string_view::npos ? std::string_view{} : rest.substr(line_end + 1U);

        if (line.starts_with("| ---"))
        {
            continue;
        }

        if (!line.starts_with(row_start))
        {
            break;
        }

        const auto names = quoted_names(line.substr(0U, line.find(cell_separator, row_start.size())));

        if (names.size() != 1U)
        {
            return {};
        }

        codes.push_back(names.front());
    }

    return codes;
}

void test_documented_control_refusals(std::string_view document)
{
    std::vector<std::string> published;
    std::ranges::transform(wg::published_control_refusals, std::back_inserter(published), [](const wg::PublishedRefusal& row) { return std::string{row.code}; });

    check(documented_control_refusals(document) == published, "the Live commands section's table names every refusal of the live controller, in the map's order");
}

[[nodiscard]] bool same(m110::Status left, m110::Status right) noexcept
{
    return left.code == right.code && std::string_view{left.message} == std::string_view{right.message};
}

void test_lookups()
{
    for (const auto& row : wg::player_error_names)
    {
        const m110::Status status = wg::status_of(row.value);
        check(status.code == row.code && status.message == row.name && wg::player_error_of(status) == row.value, std::string{"player error "} + row.name + "'s Status maps back to it");
    }

    for (const auto& row : wg::engine_error_names)
    {
        const m110::Status status = wg::status_of(row.value);
        check(status.code == row.code && status.message == row.name && wg::engine_error_of(status) == row.value, std::string{"engine error "} + row.name + "'s Status maps back to it");
    }

    constexpr std::uint32_t no_enumerator = 99U;
    check(same(wg::status_of(static_cast<wg::PlayerError>(no_enumerator)), wg::unknown_error) && same(wg::status_of(static_cast<wg::EngineError>(no_enumerator)), wg::unknown_error),
          "a value that is no enumerator has the unknown_error Status");
    check(wg::player_error_of(static_cast<wg::WavValidationError>(no_enumerator)) == wg::PlayerError::header_io, "a WAV error that is no enumerator is header_io, as an unreadable header");
    check(wg::player_error_of(m110::Status{.code = m110::StatusCode::io_error, .message = "sd_read"}) == wg::PlayerError::sd_read, "a Status with sd_read's code and name carries sd_read");
    check(!wg::player_error_of(m110::Status{.code = m110::StatusCode::internal_error, .message = "sd_read"}).has_value() &&
              !wg::player_error_of(m110::Status{.code = m110::StatusCode::io_error, .message = nullptr}).has_value() &&
              !wg::engine_error_of(m110::Status{.code = m110::StatusCode::io_error, .message = "sd_read"}).has_value(),
          "a Status whose code, name or table differs carries no error");
    check(wg::status_of(wg::PlayerError::none).is_ok() && wg::status_of(wg::EngineError::none).is_ok(), "none is a success");
}

// Each C status's code and message, as the firmware's C++ side reads it,
// and back; kWFG_StatusCount, which is no status (a cast to enum wfg_status
// of a value outside its range, such as 99, is undefined: the enumeration
// has no fixed underlying type), and a Status no row carries.
void test_c_statuses()
{
    struct Expected
    {
        wfg_status value;
        m110::StatusCode code;
        std::string_view message;
    };

    constexpr auto expected = std::to_array<Expected>({
        {kWFG_StatusOk, m110::StatusCode::ok, "ok"},
        {kWFG_StatusInvalidArgument, m110::StatusCode::invalid_argument, "INVALID_ARGUMENT"},
        {kWFG_StatusMediaRefused, m110::StatusCode::unavailable, "MEDIA_REFUSED"},
        {kWFG_StatusMediaFault, m110::StatusCode::io_error, "MEDIA_FAULT"},
        {kWFG_StatusSdHostNotReady, m110::StatusCode::unavailable, "SD_HOST_NOT_READY"},
        {kWFG_StatusUsbClockFailed, m110::StatusCode::io_error, "USB_CLOCK_FAILED"},
        {kWFG_StatusUsbStackFailed, m110::StatusCode::io_error, "USB_STACK_FAILED"},
        {kWFG_StatusEp0OrderIdle, m110::StatusCode::unavailable, "EP0_ORDER_IDLE"},
        {kWFG_StatusEp0HoldFull, m110::StatusCode::buffer_too_small, "EP0_HOLD_FULL"},
    });
    check(expected.size() == wg::c_statuses.size() && std::cmp_equal(expected.size(), std::to_underlying(kWFG_StatusCount)), "a row for each C status, as many as kWFG_StatusCount counts");

    for (const Expected& row : expected)
    {
        const m110::Status status = wg::status_of(row.value);
        check(status.code == row.code && std::string_view{status.message} == row.message && wg::c_status_of(status) == row.value,
              std::string{"C status "} + std::string{row.message} + " is its Status and back");
    }

    check(same(wg::status_of(kWFG_StatusCount), wg::unknown_c_status) && !wg::c_status_of(wg::unknown_c_status).has_value(),
          "kWFG_StatusCount, which is no status, has the UNKNOWN_C_STATUS Status, which no row carries");
    check(!wg::c_status_of(m110::Status{.code = m110::StatusCode::io_error, .message = "MEDIA_REFUSED"}).has_value() &&
              !wg::c_status_of(m110::Status{.code = m110::StatusCode::unavailable, .message = nullptr}).has_value(),
          "a Status whose code or message differs from every row's carries no C status");
    check(wg::c_status_of(m110::Status{.code = m110::StatusCode::ok, .message = "HELD"}) == kWFG_StatusOk &&
              wg::c_status_of(m110::Status{.code = m110::StatusCode::ok, .message = nullptr}) == kWFG_StatusOk,
          "every success is kWFG_StatusOk, whatever its message");
}

void test_refuse()
{
    wg::ControlReply reply{.ok = true};
    wg::refuse(reply, wg::refusal::run_id_reused);
    check(!reply.ok && reply.error == wg::refusal::run_id_reused.message, "a refused reply carries the refusal's code");
    wg::refuse(reply, wg::usb_audio::stream_not_open);
    check(!reply.ok && std::string_view{reply.error} == "NO_STREAM", "a Status from elsewhere is refused with its message");
    wg::refuse(reply, signal_lab::control_refusal::late_event);
    check(!reply.ok && std::string_view{reply.error} == "late_event", "a live control is refused with the live controller's code");
}

} // namespace

#ifndef WFG_PROTOCOL_DOCUMENT
#error WFG_PROTOCOL_DOCUMENT must name the protocol document used by the error-map test.
#endif

TEST_CASE("Player error names and numbers match the protocol document", "[error-map][documentation]")
{
    const auto document = read_text(WFG_PROTOCOL_DOCUMENT);

    if (!document.has_value())
    {
        FAIL("cannot read the protocol document");
        return;
    }

    test_documented_player_errors(*document);
}

TEST_CASE("Playlist refusal details match the error map", "[error-map][documentation]")
{
    const auto document = read_text(WFG_PROTOCOL_DOCUMENT);

    if (!document.has_value())
    {
        FAIL("cannot read the protocol document");
        return;
    }

    test_documented_details(*document);
}

TEST_CASE("Published refusals are named by the protocol document", "[error-map][documentation]")
{
    const auto document = read_text(WFG_PROTOCOL_DOCUMENT);

    if (!document.has_value())
    {
        FAIL("cannot read the protocol document");
        return;
    }

    test_documented_refusals(*document);
}

TEST_CASE("Live control refusals match the protocol document", "[error-map][documentation]")
{
    const auto document = read_text(WFG_PROTOCOL_DOCUMENT);

    if (!document.has_value())
    {
        FAIL("cannot read the protocol document");
        return;
    }

    test_documented_control_refusals(*document);
}

TEST_CASE("Engine state and error numbers match the protocol document", "[error-map][documentation]")
{
    const auto document = read_text(WFG_PROTOCOL_DOCUMENT);

    if (!document.has_value())
    {
        FAIL("cannot read the protocol document");
        return;
    }

    test_documented_engine_numbers(*document);
}

TEST_CASE("Error status lookups preserve their published rows", "[error-map]")
{
    test_lookups();
}

TEST_CASE("C status values map to their C++ statuses", "[error-map]")
{
    test_c_statuses();
}

TEST_CASE("Refusal replies retain the refusal code", "[error-map]")
{
    test_refuse();
}

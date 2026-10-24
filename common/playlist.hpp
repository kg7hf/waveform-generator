// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// PLAYLIST batch mode: the playlist file's items, the result blocks the player
// keeps until the host reads them, and the rules that place each item in time
// Guide: docs/modules/rt1170-player-and-audio.md (PLAYLIST sequencer),
// docs/live-control-protocol.md (PLAYLIST)
// Ownership: plain values. In the firmware the player task owns the item
// table, the progress and the result ring; it writes the progress and the ring
// under a critical section that their reader (the USB task, for PLAYLIST?)
// also takes.
// Contract: constexpr, allocation-free, no I/O. A playlist file is ASCII, one
// item per line: <FILE.WAV> <gap_seconds> <USB|CODEC> and the options
// label=<id>, scn=<NAME.SCN>, seed=<u32>, cut=<entries>, att=<dB> and
// dur=<seconds> in any order, each at most once; '#' starts a comment that runs to the line's end, and blank
// lines are ignored. FileParser reads the file's bytes in any split, parses
// every line and keeps the items in an ItemTable (at most max_items, labels
// unique), and names the first line it refuses. The card checks (the WAV and
// the scenario each item names) are the player's.

#pragma once

#include "common/secondary_source_identity.hpp"

#include "common/attenuation.hpp"
#include "common/cut_schedule.hpp"
#include "common/status.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace waveform_generator::playlist
{

// A line of the playlist file, counted from 1; 0 names the file as a whole.
struct LineNumber
{
    std::uint32_t value{};

    constexpr auto operator<=>(const LineNumber&) const noexcept = default;
};

// At most Capacity characters, NUL-terminated for the C file API: every
// character past the text stays NUL.
template <std::size_t Capacity> class FixedText
{
public:
    // The first Capacity characters of `text`; the grammar bounds every text
    // it keeps, so nothing is cut in practice.
    [[nodiscard]] static constexpr FixedText from(std::string_view text) noexcept
    {
        FixedText fixed{};
        const auto kept = text.substr(0U, Capacity);
        std::ranges::copy(kept, fixed.chars_.begin());
        fixed.size_ = kept.size();
        return fixed;
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept { return std::string_view{chars_.data(), size_}; }

    [[nodiscard]] constexpr const char* c_str() const noexcept { return chars_.data(); }

    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0U; }

    constexpr bool operator==(const FixedText&) const noexcept = default;

private:
    std::array<char, Capacity + 1U> chars_{};
    std::size_t size_{};
};

// A short (8.3) file name in upper case: a stem of 1 to 8 name characters
// (A-Z, 0-9, _), a dot and an extension of 1 to 3.
inline constexpr std::size_t name_stem_max_chars = 8U;
inline constexpr std::size_t name_extension_max_chars = 3U;
inline constexpr std::size_t name_max_chars = name_stem_max_chars + 1U + name_extension_max_chars;
// An item's label: 1 to 24 of A-Z, a-z, 0-9, '_', '-' and '.'.
inline constexpr std::size_t label_max_chars = 24U;
using ShortName = FixedText<name_max_chars>;
using Label = FixedText<label_max_chars>;

inline constexpr std::size_t max_items = 256U;
// Result blocks kept until the host acknowledges them; a full ring pauses the
// playlist before its next item, so no block is ever dropped.
inline constexpr std::size_t result_capacity = 32U;
// Bytes of a line before its LF, a CR included.
inline constexpr std::size_t max_line_chars = 511U;
// The largest playlist file the player reads: 256 items of the longest line,
// with room to spare for comments and blank lines.
inline constexpr std::uint32_t max_file_bytes = 262144U;
static_assert(max_file_bytes >= max_items * (max_line_chars + 1U), "every line of a full playlist fits");
// The result blocks one PLAYLIST? reply carries, the oldest first; the reply's
// worst case (rt1170/status_contract.hpp) fits the response buffer.
inline constexpr std::size_t blocks_per_reply = 2U;

// A gap is 0 to 3600 s, in whole seconds or with up to 3 decimals, kept as
// an exact number of 48 kHz frames: whole milliseconds of 48 frames each.
inline constexpr std::uint32_t frames_per_millisecond = 48U;
inline constexpr std::uint32_t milliseconds_per_second = 1000U;
inline constexpr std::uint32_t max_gap_seconds = 3600U;
inline constexpr std::uint32_t max_gap_milliseconds = max_gap_seconds * milliseconds_per_second;
inline constexpr std::size_t gap_max_whole_digits = 4U;
inline constexpr std::size_t gap_max_decimals = 3U;
inline constexpr std::uint32_t decimal_radix = 10U;

struct GapFrames
{
    std::uint32_t value{};

    constexpr auto operator<=>(const GapFrames&) const noexcept = default;
};

inline constexpr GapFrames max_gap{.value = max_gap_milliseconds * frames_per_millisecond};

// The gap in milliseconds: exact, a gap being whole milliseconds.
[[nodiscard]] constexpr std::uint32_t gap_milliseconds(GapFrames gap) noexcept
{
    return gap.value / frames_per_millisecond;
}

// An item's duration (dur=) is 1 to 86400 s, spelt as a gap is, kept as an
// exact number of 48 kHz frames: the frames of its looped card the item
// feeds the engine, which is the length of its output (the axis its CUT
// offsets count) unless its scenario slips samples.
inline constexpr std::uint32_t min_duration_seconds = 1U;
inline constexpr std::uint32_t max_duration_seconds = 86400U;
inline constexpr std::size_t duration_max_whole_digits = 5U;

struct DurationFrames
{
    std::uint32_t value{};

    constexpr auto operator<=>(const DurationFrames&) const noexcept = default;
};

inline constexpr DurationFrames min_duration{.value = min_duration_seconds * milliseconds_per_second * frames_per_millisecond};
inline constexpr DurationFrames max_duration{.value = max_duration_seconds * milliseconds_per_second * frames_per_millisecond};
static_assert(std::uint64_t{max_duration_seconds} * milliseconds_per_second * frames_per_millisecond == max_duration.value, "the longest duration's frames fit 32 bits: 24 h at 48 kHz");

// An item's output route. The numbers are OutputRoute's (rt1170/player.hpp,
// STATUS output_route).
enum class Route : std::uint8_t
{
    codec = 0U,
    usb = 1U,
};

// The output routes an image offers: the codec always, the USB sound card on
// every player image. An image without it refuses USB items.
struct OfferedRoutes
{
    bool usb{};
};

// The refusals of a playlist file, as WFG-LIVE/1 error codes; the reply names
// the line (docs/live-control-protocol.md, PLAYLIST).
inline constexpr m110::Status bad_syntax{.code = m110::StatusCode::invalid_argument, .message = "PLAYLIST_SYNTAX"};
inline constexpr m110::Status bad_file{.code = m110::StatusCode::invalid_argument, .message = "PLAYLIST_FILE"};
inline constexpr m110::Status bad_gap{.code = m110::StatusCode::invalid_argument, .message = "PLAYLIST_GAP"};
inline constexpr m110::Status bad_route{.code = m110::StatusCode::invalid_argument, .message = "PLAYLIST_ROUTE"};
inline constexpr m110::Status route_unavailable{.code = m110::StatusCode::unavailable, .message = "PLAYLIST_ROUTE_UNAVAILABLE"};
inline constexpr m110::Status bad_label{.code = m110::StatusCode::invalid_argument, .message = "PLAYLIST_LABEL"};
inline constexpr m110::Status duplicate_label{.code = m110::StatusCode::invalid_argument, .message = "PLAYLIST_LABEL_DUPLICATE"};
inline constexpr m110::Status bad_scenario{.code = m110::StatusCode::invalid_argument, .message = "PLAYLIST_SCN"};
inline constexpr m110::Status bad_seed{.code = m110::StatusCode::invalid_argument, .message = "PLAYLIST_SEED"};
inline constexpr m110::Status seed_without_scenario{.code = m110::StatusCode::invalid_argument, .message = "PLAYLIST_SEED_NO_SCN"};
inline constexpr m110::Status bad_cut{.code = m110::StatusCode::invalid_argument, .message = "PLAYLIST_CUT"};
// att= that is not 0 to -40.0 dB in tenths (common/attenuation.hpp).
inline constexpr m110::Status bad_attenuation = attenuation::bad_attenuation;
inline constexpr m110::Status bad_duration{.code = m110::StatusCode::invalid_argument, .message = "PLAYLIST_DUR"};
inline constexpr m110::Status line_too_long{.code = m110::StatusCode::buffer_too_small, .message = "PLAYLIST_LINE_TOO_LONG"};
inline constexpr m110::Status too_many_items{.code = m110::StatusCode::buffer_too_small, .message = "PLAYLIST_TOO_MANY"};
inline constexpr m110::Status no_items{.code = m110::StatusCode::invalid_argument, .message = "PLAYLIST_EMPTY"};
// The player's card checks, before anything plays.
inline constexpr m110::Status playlist_unreadable{.code = m110::StatusCode::io_error, .message = "PLAYLIST_OPEN"};
inline constexpr m110::Status playlist_too_large{.code = m110::StatusCode::buffer_too_small, .message = "PLAYLIST_TOO_LARGE"};
inline constexpr m110::Status wav_missing{.code = m110::StatusCode::unavailable, .message = "PLAYLIST_WAV_MISSING"};
inline constexpr m110::Status wav_invalid{.code = m110::StatusCode::invalid_argument, .message = "PLAYLIST_WAV_INVALID"};
inline constexpr m110::Status scenario_missing{.code = m110::StatusCode::unavailable, .message = "PLAYLIST_SCN_MISSING"};
inline constexpr m110::Status scenario_invalid{.code = m110::StatusCode::invalid_argument, .message = "PLAYLIST_SCN_INVALID"};
// PLAYLIST ACK of a number no block has been given yet.
inline constexpr m110::Status ack_range{.code = m110::StatusCode::invalid_argument, .message = "PLAYLIST_ACK_RANGE"};
// ResultRing::push() on a full ring; the sequencer pauses before that.
inline constexpr m110::Status ring_full{.code = m110::StatusCode::buffer_too_small, .message = "PLAYLIST_RING_FULL"};
// Why a request line that starts with PLAYLIST is none of its commands
// (live_protocol::parse_playlist_command()): a name that is not an upper-case
// 8.3 name, a policy other than CONTINUE, an ACK without a canonical uint32,
// or another spelling. On the wire each is BAD_REQUEST.
inline constexpr m110::Status bad_name{.code = m110::StatusCode::invalid_argument, .message = "PLAYLIST_NAME"};
inline constexpr m110::Status bad_policy{.code = m110::StatusCode::invalid_argument, .message = "PLAYLIST_POLICY"};
inline constexpr m110::Status bad_ack{.code = m110::StatusCode::invalid_argument, .message = "PLAYLIST_ACK"};
inline constexpr m110::Status bad_command{.code = m110::StatusCode::invalid_argument, .message = "PLAYLIST_COMMAND"};

inline constexpr std::string_view wav_extension{"WAV"};
inline constexpr std::string_view scenario_extension{"SCN"};
inline constexpr std::string_view usb_route_name{"USB"};
inline constexpr std::string_view codec_route_name{"CODEC"};
inline constexpr std::string_view label_key{"label="};
inline constexpr std::string_view scenario_key{"scn="};
inline constexpr std::string_view seed_key{"seed="};
inline constexpr std::string_view cut_key{"cut="};
inline constexpr std::string_view attenuation_key{"att="};
inline constexpr std::string_view duration_key{"dur="};
inline constexpr std::string_view separators{" \t"};
inline constexpr char comment_mark = '#';
inline constexpr char extension_mark = '.';
inline constexpr char horizontal_tab = '\t';
inline constexpr char carriage_return = '\r';
inline constexpr std::uint8_t line_feed = '\n';
inline constexpr char first_printable = ' ';
inline constexpr char last_printable = '~';

// One playlist item, as its line gives it.
struct Item
{
    ShortName file{}; // the WAV in 2:/WG
    GapFrames gap{};  // waited after the item
    Route route{Route::codec};
    Label label{};                            // empty: none
    ShortName scenario{};                     // the scenario file in 2:/WG; empty: none
    std::optional<std::uint32_t> seed{};      // the scenario's seed for this item
    cut::ValidSchedule cuts{};                // the item's CUT schedule
    attenuation::Tenths attenuation{};        // att=: below full level, after the engine; 0 dB without it
    std::optional<DurationFrames> duration{}; // dur=: the card loops until the engine has this many frames; none: the card once
    LineNumber line{};
    std::uint32_t scenario_crc32{}; // the player's card check: zlib CRC-32 of the scenario file; 0 without one
};

[[nodiscard]] constexpr bool is_name_char(char character) noexcept
{
    return (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') || character == '_';
}

[[nodiscard]] constexpr bool is_label_char(char character) noexcept
{
    return is_name_char(character) || (character >= 'a' && character <= 'z') || character == '-' || character == extension_mark;
}

// Printable ASCII or a tab: everything else on a line is refused.
[[nodiscard]] constexpr bool is_line_char(char character) noexcept
{
    return (character >= first_printable && character <= last_printable) || character == horizontal_tab;
}

[[nodiscard]] constexpr bool is_name_stem(std::string_view stem) noexcept
{
    return !stem.empty() && stem.size() <= name_stem_max_chars && std::ranges::all_of(stem, is_name_char);
}

// An upper-case 8.3 name with the extension `extension`.
[[nodiscard]] constexpr bool is_short_name(std::string_view text, std::string_view extension) noexcept
{
    const auto dot = text.find(extension_mark);
    return dot != std::string_view::npos && is_name_stem(text.substr(0U, dot)) && text.substr(dot + 1U) == extension;
}

// The playlist's own name: an upper-case 8.3 name whose extension, 1 to 3 name
// characters, may be left out (BATTERY.PL, LADDER).
[[nodiscard]] constexpr bool is_playlist_name(std::string_view text) noexcept
{
    const auto dot = text.find(extension_mark);

    if (dot == std::string_view::npos)
    {
        return is_name_stem(text);
    }

    const auto extension = text.substr(dot + 1U);
    return is_name_stem(text.substr(0U, dot)) && !extension.empty() && extension.size() <= name_extension_max_chars && std::ranges::all_of(extension, is_name_char);
}

// The value of a decimal digit, or none.
[[nodiscard]] constexpr std::optional<std::uint32_t> digit_value(char character) noexcept
{
    constexpr std::string_view digits{"0123456789"};
    std::uint32_t value = 0U;

    for (const char digit : digits)
    {
        if (digit == character)
        {
            return value;
        }

        ++value;
    }

    return std::nullopt;
}

// Up to gap_max_decimals digits after the point, as milliseconds ("5" is 500).
[[nodiscard]] constexpr std::optional<std::uint32_t> fraction_milliseconds(std::string_view fraction) noexcept
{
    if (fraction.empty() || fraction.size() > gap_max_decimals)
    {
        return std::nullopt;
    }

    std::uint32_t milliseconds = 0U;

    for (std::size_t place = 0U; place < gap_max_decimals; ++place)
    {
        const auto digit = place < fraction.size() ? digit_value(fraction[place]) : std::optional<std::uint32_t>{0U};

        if (!digit.has_value())
        {
            return std::nullopt;
        }

        milliseconds = milliseconds * decimal_radix + *digit;
    }

    return milliseconds;
}

// <seconds>[.<decimals>]: canonical whole seconds (no sign, no leading zero)
// of at most `max_whole_digits` digits, then up to 3 decimals, as
// milliseconds; none for another spelling.
[[nodiscard]] constexpr std::optional<std::uint32_t> parse_milliseconds(std::string_view text, std::size_t max_whole_digits) noexcept
{
    const auto point = text.find(extension_mark);
    const auto whole = text.substr(0U, point);
    const auto seconds = cut::parse_uint32(whole);

    if (!seconds.has_value() || whole.size() > max_whole_digits)
    {
        return std::nullopt;
    }

    const auto fraction = point == std::string_view::npos ? std::optional<std::uint32_t>{0U} : fraction_milliseconds(text.substr(point + 1U));

    if (!fraction.has_value())
    {
        return std::nullopt;
    }

    // At most 99999 s and 999 ms here: the sum fits 32 bits.
    return seconds.value() * milliseconds_per_second + *fraction;
}

// The most parse_milliseconds() returns, from the longest whole seconds.
[[nodiscard]] consteval std::uint64_t most_milliseconds(std::size_t max_whole_digits) noexcept
{
    std::uint64_t seconds_bound = 1U;

    for (std::size_t digit = 0U; digit < max_whole_digits; ++digit)
    {
        seconds_bound *= decimal_radix;
    }

    return seconds_bound * milliseconds_per_second - 1U;
}

static_assert(most_milliseconds(duration_max_whole_digits) <= std::numeric_limits<std::uint32_t>::max() && gap_max_whole_digits <= duration_max_whole_digits,
              "the longest spelling's milliseconds fit 32 bits");

// <seconds>[.<decimals>]: canonical whole seconds (no sign, no leading zero),
// then up to 3 decimals; 0 to 3600 s. Else bad_gap.
[[nodiscard]] constexpr m110::Result<GapFrames> parse_gap(std::string_view text) noexcept
{
    const auto milliseconds = parse_milliseconds(text, gap_max_whole_digits);

    if (!milliseconds.has_value() || *milliseconds > max_gap_milliseconds)
    {
        return bad_gap;
    }

    return GapFrames{.value = *milliseconds * frames_per_millisecond};
}

// dur=<seconds>: spelt as a gap, 1 to 86400 s. Else bad_duration.
[[nodiscard]] constexpr m110::Result<DurationFrames> parse_duration(std::string_view text) noexcept
{
    const auto milliseconds = parse_milliseconds(text, duration_max_whole_digits);
    constexpr std::uint32_t least = min_duration_seconds * milliseconds_per_second;
    constexpr std::uint32_t most = max_duration_seconds * milliseconds_per_second;

    if (!milliseconds.has_value() || *milliseconds < least || *milliseconds > most)
    {
        return bad_duration;
    }

    return DurationFrames{.value = *milliseconds * frames_per_millisecond};
}

[[nodiscard]] constexpr m110::Result<Route> parse_route(std::string_view text, const OfferedRoutes& offered) noexcept
{
    if (text == codec_route_name)
    {
        return Route::codec;
    }

    if (text != usb_route_name)
    {
        return bad_route;
    }

    if (!offered.usb)
    {
        return route_unavailable;
    }

    return Route::usb;
}

// A line's tokens: runs of characters between spaces and tabs, up to a token
// that starts with '#', which begins a comment.
class Tokens
{
public:
    explicit constexpr Tokens(std::string_view line) noexcept : rest_{line} {}

    // The next token, or none at the line's end or its comment.
    [[nodiscard]] constexpr std::optional<std::string_view> next() noexcept
    {
        const auto start = rest_.find_first_not_of(separators);

        if (start == std::string_view::npos || rest_[start] == comment_mark)
        {
            rest_ = {};
            return std::nullopt;
        }

        rest_.remove_prefix(start);
        const auto token = rest_.substr(0U, rest_.find_first_of(separators));
        rest_.remove_prefix(token.size());
        return token;
    }

private:
    std::string_view rest_;
};

// Whether an option is given for the first time; marks it given.
[[nodiscard]] constexpr bool first_time(bool& seen) noexcept
{
    const bool first = !seen;
    seen = true;
    return first;
}

// The value of `token` when it is `key`'s option, else none.
[[nodiscard]] constexpr std::optional<std::string_view> option_value(std::string_view token, std::string_view key) noexcept
{
    if (!token.starts_with(key))
    {
        return std::nullopt;
    }

    return token.substr(key.size());
}

[[nodiscard]] constexpr m110::Status parse_label(std::string_view value, Item& item) noexcept
{
    if (value.empty() || value.size() > label_max_chars || !std::ranges::all_of(value, is_label_char))
    {
        return bad_label;
    }

    item.label = Label::from(value);
    return m110::Status::success();
}

[[nodiscard]] constexpr m110::Status parse_scenario(std::string_view value, Item& item) noexcept
{
    if (!is_short_name(value, scenario_extension))
    {
        return bad_scenario;
    }

    item.scenario = ShortName::from(value);
    return m110::Status::success();
}

[[nodiscard]] constexpr m110::Status parse_seed(std::string_view value, Item& item) noexcept
{
    const auto seed = cut::parse_uint32(value);

    if (!seed.has_value())
    {
        return bad_seed;
    }

    item.seed = seed.value();
    return m110::Status::success();
}

// The CUT command's grammar and limits (common/cut_schedule.hpp): a bad
// spelling is bad_cut; a list that breaks a rule is refused with the CUT
// command's own code (CUT_RANGE, CUT_UNSORTED, CUT_OVERLAP, CUT_DROP_RUN).
[[nodiscard]] constexpr m110::Status parse_cuts(std::string_view value, Item& item) noexcept
{
    const auto cuts = cut::parse_entries(value);

    if (!cuts.has_value())
    {
        return bad_cut;
    }

    const auto valid = cut::validate(cuts.value());

    if (!valid.has_value())
    {
        return valid.status();
    }

    item.cuts = valid.value();
    return m110::Status::success();
}

// att=<dB> (common/attenuation.hpp): 0 to -40.0 dB in tenths; a positive
// level, a second decimal or another spelling is bad_attenuation.
[[nodiscard]] constexpr m110::Status parse_attenuation(std::string_view value, Item& item) noexcept
{
    const auto tenths = attenuation::parse(value);

    if (!tenths.has_value())
    {
        return tenths.status();
    }

    item.attenuation = tenths.value();
    return m110::Status::success();
}

// dur=<seconds>: 1 to 86400 s, spelt as a gap; else bad_duration.
[[nodiscard]] constexpr m110::Status parse_item_duration(std::string_view value, Item& item) noexcept
{
    const auto frames = parse_duration(value);

    if (!frames.has_value())
    {
        return frames.status();
    }

    item.duration = frames.value();
    return m110::Status::success();
}

// An option's key and the parser of its value. No key is a prefix of
// another's.
struct OptionRule
{
    std::string_view key;
    m110::Status (*parse)(std::string_view value, Item& item) noexcept;
};

inline constexpr auto option_rules = std::to_array<OptionRule>({
    {.key = label_key, .parse = &parse_label},
    {.key = scenario_key, .parse = &parse_scenario},
    {.key = seed_key, .parse = &parse_seed},
    {.key = cut_key, .parse = &parse_cuts},
    {.key = attenuation_key, .parse = &parse_attenuation},
    {.key = duration_key, .parse = &parse_item_duration},
});

// The options a line has given so far, in option_rules' order: each at most
// once.
using SeenOptions = std::array<bool, option_rules.size()>;

// One key=value option after the route. An unknown key, or a key given
// twice, is bad_syntax.
[[nodiscard]] constexpr m110::Status parse_option(std::string_view token, Item& item, SeenOptions& seen) noexcept
{
    const std::span rules{option_rules};
    const std::span given{seen};

    for (std::size_t index = 0U; index < rules.size(); ++index)
    {
        if (const auto value = option_value(token, rules[index].key); value.has_value())
        {
            return first_time(given[index]) ? rules[index].parse(*value, item) : bad_syntax;
        }
    }

    return bad_syntax;
}

// The file, gap and route that every item starts with.
[[nodiscard]] constexpr m110::Status parse_fields(std::string_view file, std::string_view gap, std::string_view route, const OfferedRoutes& offered, Item& item) noexcept
{
    if (!is_short_name(file, wav_extension))
    {
        return bad_file;
    }

    const auto frames = parse_gap(gap);

    if (!frames.has_value())
    {
        return frames.status();
    }

    const auto output = parse_route(route, offered);

    if (!output.has_value())
    {
        return output.status();
    }

    item.file = ShortName::from(file);
    item.gap = frames.value();
    item.route = output.value();
    return m110::Status::success();
}

// One line of the file, without its LF and a CR before it: an item, or none
// for a blank or comment line. A line with fewer than three fields, or with
// a character other than printable ASCII and tabs, is bad_syntax; seed=
// needs scn= (seed_without_scenario).
[[nodiscard]] constexpr m110::Result<std::optional<Item>> parse_line(std::string_view line, LineNumber number, const OfferedRoutes& offered) noexcept
{
    if (!std::ranges::all_of(line, is_line_char))
    {
        return bad_syntax;
    }

    Tokens tokens{line};
    const auto file = tokens.next();

    if (!file.has_value())
    {
        return std::optional<Item>{};
    }

    const auto gap = tokens.next();
    const auto route = tokens.next();

    if (!gap.has_value() || !route.has_value())
    {
        return bad_syntax;
    }

    Item item{.line = number};
    const auto fields = parse_fields(*file, *gap, *route, offered, item);

    if (!fields.is_ok())
    {
        return fields;
    }

    SeenOptions seen{};

    for (auto token = tokens.next(); token.has_value(); token = tokens.next())
    {
        const auto option = parse_option(*token, item, seen);

        if (!option.is_ok())
        {
            return option;
        }
    }

    if (item.seed.has_value() && item.scenario.empty())
    {
        return seed_without_scenario;
    }

    return std::optional<Item>{item};
}

// The items of one playlist file, in its order.
class ItemTable
{
public:
    constexpr void clear() noexcept { count_ = 0U; }

    // Appends `item`: too_many_items when the table holds max_items,
    // duplicate_label when another item has its label. A refusal changes
    // nothing.
    [[nodiscard]] constexpr m110::Status push(const Item& item) noexcept
    {
        if (count_ == items_.size())
        {
            return too_many_items;
        }

        const auto same_label = [&item](const Item& earlier) { return earlier.label == item.label; };

        if (!item.label.empty() && std::ranges::any_of(items(), same_label))
        {
            return duplicate_label;
        }

        std::span{items_}[count_] = item;
        ++count_;
        return m110::Status::success();
    }

    [[nodiscard]] constexpr std::span<const Item> items() const noexcept { return std::span{items_}.first(count_); }

    // The player's card checks fill in what they learn (scenario_crc32).
    [[nodiscard]] constexpr std::span<Item> items() noexcept { return std::span{items_}.first(count_); }

    [[nodiscard]] constexpr std::uint32_t size() const noexcept { return count_; }

private:
    std::array<Item, max_items> items_{};
    std::uint32_t count_{};
};

// Reads a playlist file's bytes, in any split, line by line into an
// ItemTable. Each LF ends a line; a CR before it is dropped. After a refusal,
// line() names the line refused (0 for an empty playlist); stop feeding then.
class FileParser
{
public:
    [[nodiscard]] constexpr m110::Status feed(std::span<const std::uint8_t> bytes, ItemTable& table, const OfferedRoutes& offered) noexcept
    {
        for (const std::uint8_t byte : bytes)
        {
            if (byte != line_feed)
            {
                append(std::bit_cast<char>(byte));
                continue;
            }

            const auto ended = end_line(table, offered);

            if (!ended.is_ok())
            {
                return ended;
            }
        }

        return m110::Status::success();
    }

    // The file has ended: its last line when no LF ends it, then at least
    // one item (else no_items, for the file as a whole).
    [[nodiscard]] constexpr m110::Status finish(ItemTable& table, const OfferedRoutes& offered) noexcept
    {
        if (size_ != 0U || overflow_)
        {
            const auto ended = end_line(table, offered);

            if (!ended.is_ok())
            {
                return ended;
            }
        }

        if (table.size() == 0U)
        {
            line_ = LineNumber{};
            return no_items;
        }

        return m110::Status::success();
    }

    // The line being read, or the one refused.
    [[nodiscard]] constexpr LineNumber line() const noexcept { return line_; }

private:
    constexpr void append(char character) noexcept
    {
        if (size_ == text_.size())
        {
            overflow_ = true;
            return;
        }

        std::span{text_}[size_] = character;
        ++size_;
    }

    // The line's text without the CR that may end it.
    [[nodiscard]] constexpr std::string_view text() const noexcept
    {
        std::string_view text{text_.data(), size_};

        if (text.ends_with(carriage_return))
        {
            text.remove_suffix(1U);
        }

        return text;
    }

    [[nodiscard]] constexpr m110::Status end_line(ItemTable& table, const OfferedRoutes& offered) noexcept
    {
        const auto status = overflow_ ? line_too_long : keep(parse_line(text(), line_, offered), table);

        if (status.is_ok())
        {
            ++line_.value;
            size_ = 0U;
        }

        return status;
    }

    // An item a line gave goes into the table.
    [[nodiscard]] static constexpr m110::Status keep(const m110::Result<std::optional<Item>>& parsed, ItemTable& table) noexcept
    {
        if (!parsed.has_value())
        {
            return parsed.status();
        }

        const auto& item = parsed.value();

        if (!item.has_value())
        {
            return m110::Status::success();
        }

        return table.push(*item);
    }

    std::array<char, max_line_chars> text_{};
    std::size_t size_{};
    bool overflow_{};
    LineNumber line_{.value = 1U};
};

// How an item ended.
enum class Outcome : std::uint8_t
{
    done = 0U,    // every frame played (codec: drained; USB: every chunk confirmed sent)
    aborted = 1U, // STOP
    error = 2U,   // a fault, or the item could not start: player_error names it
};

[[nodiscard]] constexpr std::string_view outcome_name(Outcome outcome) noexcept
{
    switch (outcome)
    {
    case Outcome::done:
        return "done";

    case Outcome::aborted:
        return "aborted";

    case Outcome::error:
        return "error";
    }

    return "error";
}

[[nodiscard]] constexpr std::string_view route_name(Route route) noexcept
{
    return route == Route::usb ? "usb" : "codec";
}

// Why an item ended: its end, STOP, or the fault that ended it. An item with
// dur= (a looped item) also ends at its first anomaly: a missed USB interval,
// usb_stack_asserts rising, or an engine deadline miss, which a play without
// dur= only counts. The player maps each player error (rt1170/player.hpp
// PlayerError) to its cause; the block's player_error names any fault.
enum class EndCause : std::uint8_t
{
    end = 0U,             // every frame played: the card's, or the looped item's duration
    stop = 1U,            // STOP
    underrun = 2U,        // the output FIFO ran dry (player_error 15)
    missed_interval = 3U, // a looped item's USB interval without a packet while it had frames queued (23)
    stream_closed = 4U,   // the capture stream closed (19)
    stall = 5U,           // the host stopped polling (20)
    sd_read = 6U,         // an SD read or seek failed (11, 10)
    usb_assert = 7U,      // usb_stack_asserts rose during a looped item (24)
    deadline_miss = 8U,   // an engine block came too late in a looped item (25)
    no_stream = 9U,       // a USB item without an open capture stream: it played nothing (21)
    fault = 10U,          // another fault: player_error names it
};

// The causes' names, in the enumeration's order.
inline constexpr auto end_cause_names =
    std::to_array<std::string_view>({"end", "stop", "underrun", "missed_interval", "stream_closed", "stall", "sd_read", "usb_assert", "deadline_miss", "no_stream", "fault"});

[[nodiscard]] constexpr std::string_view end_cause_name(EndCause cause) noexcept
{
    const std::size_t index = std::to_underlying(cause);
    return index < end_cause_names.size() ? std::span{end_cause_names}[index] : end_cause_names.back();
}

static_assert(end_cause_names.size() == std::size_t{std::to_underlying(EndCause::fault)} + 1U && end_cause_name(EndCause::end) == "end" && end_cause_name(EndCause::deadline_miss) == "deadline_miss" &&
                  end_cause_name(EndCause::fault) == "fault",
              "each cause has its name, in the enumeration's order");

// The configured channel of one play, retained independently of the engine.
// All zero means no successfully configured Watterson stage for that play.
struct WattersonIdentity
{
    std::uint32_t version{};
    std::uint32_t digest_hi{};
    std::uint32_t digest_lo{};
    std::uint32_t latency_frames{};
    std::uint32_t paths{};

    constexpr bool operator==(const WattersonIdentity&) const noexcept = default;
};

// An item's play, as STATUS and USB? report the current or last play when it
// ended (the uac_play_* fields, the engine and the codec counters). An item
// that could not start leaves them all 0.
struct PlayRecord
{
    std::uint32_t file_frames_total{};      // STATUS file_frames_total
    std::uint32_t usb_file_frames{};        // uac_play_file_frames
    std::uint32_t sent_frames{};            // uac_play_sent_frames
    std::uint32_t crc32_left{};             // uac_play_crc32_l
    std::uint32_t crc32_right{};            // uac_play_crc32_r
    std::uint32_t first_frame_index{};      // uac_play_first_frame_index
    std::uint32_t last_frame_index{};       // uac_play_last_frame_index
    std::uint32_t first_frame_sof{};        // uac_play_first_frame_sof
    std::uint32_t missed_intervals{};       // uac_play_missed_intervals
    std::uint32_t first_miss_frame_index{}; // uac_play_first_miss_frame_index
    std::uint32_t stream_bits{};            // uac_play_stream_bits
    std::uint32_t stream_channels{};        // uac_play_stream_channels
    std::uint32_t cut_count{};              // uac_play_cut_count
    std::uint32_t cut_drop_frames{};        // uac_play_cut_drop_frames
    std::uint32_t cut_zero_frames{};        // uac_play_cut_zero_frames
    std::uint32_t cut_first_index{};        // uac_play_cut_first_index
    std::uint32_t cut_rejected{};           // uac_play_cut_rejected
    std::uint32_t underruns{};              // STATUS underruns
    std::uint32_t pcm_drained{};            // STATUS pcm_drained
    std::uint32_t engine_state{};           // STATUS engine_state
    std::uint32_t engine_error{};           // STATUS engine_error
    std::uint32_t deadline_misses{};        // USB? engine_deadline_misses
    WattersonIdentity watterson{};
    SecondarySourceIdentity secondary_source{};
    std::uint32_t engine_digest_hi{}; // engine output before live controls and CUT
    std::uint32_t engine_digest_lo{};

    constexpr bool operator==(const PlayRecord&) const noexcept = default;
};

// One item's result, kept until the host acknowledges it.
struct ResultBlock
{
    std::uint32_t seq{};  // since boot, from 1, never reused
    std::uint32_t run{};  // the playlist (PLAYLIST? run) the item belongs to
    std::uint32_t item{}; // its index in the file, from 0
    Label label{};
    ShortName file{};
    Route route{Route::codec};
    Outcome outcome{Outcome::done};
    std::uint32_t player_error{};        // STATUS player_error (PlayerError) of an error
    ShortName scenario{};                // the item's scn=, empty without one
    std::optional<std::uint32_t> seed{}; // the item's seed=
    std::uint32_t scenario_crc32{};
    PlayRecord play{};
    GapFrames gap{};
    std::optional<std::uint32_t> scheduled_index{}; // the stream index the USB item was to start at; none when it started when ready
    std::uint32_t start_tick{};                     // the RTOS tick (ms) when the item began, before its LOAD
    std::uint32_t end_tick{};                       // and when its play ended
    attenuation::Tenths attenuation{};              // the item's att=
    std::optional<DurationFrames> duration{};       // the item's dur=; none without it
    std::uint32_t loops{};                          // whole passes of the card the frames sent cover
    EndCause end_cause{EndCause::end};

    // A scheduled USB item whose first frame went out elsewhere: it was not
    // ready by its stream index, or the playlist paused past it. An item that
    // sent no frame (STOP while it waited for its index, a fault before it
    // armed) started nowhere, so it never started late.
    [[nodiscard]] constexpr bool start_late() const noexcept { return scheduled_index.has_value() && play.sent_frames != 0U && play.first_frame_index != *scheduled_index; }
};

inline constexpr std::uint32_t first_seq = 1U;

// The result blocks not yet acknowledged, oldest first, numbered in order.
class ResultRing
{
public:
    [[nodiscard]] constexpr bool full() const noexcept { return count_ == blocks_.size(); }

    [[nodiscard]] constexpr std::uint32_t size() const noexcept { return count_; }

    // No block, and numbers from first_seq again: the ring at boot.
    constexpr void clear() noexcept
    {
        first_ = 0U;
        count_ = 0U;
        next_seq_ = first_seq;
    }

    // Keeps `block` as the newest, numbered next_seq(); ring_full, changing
    // nothing, when full().
    [[nodiscard]] constexpr m110::Status push(const ResultBlock& block) noexcept
    {
        if (full())
        {
            return ring_full;
        }

        auto& slot = std::span{blocks_}[(first_ + count_) % blocks_.size()];
        slot = block;
        slot.seq = next_seq_;
        ++next_seq_;
        ++count_;
        return m110::Status::success();
    }

    // Frees the blocks numbered `through` and below: how many. ack_range,
    // freeing nothing, for a number not given yet (next_seq() or later).
    [[nodiscard]] constexpr m110::Result<std::uint32_t> acknowledge(std::uint32_t through) noexcept
    {
        if (through >= next_seq_)
        {
            return ack_range;
        }

        std::uint32_t freed = 0U;

        while (count_ != 0U && std::span{blocks_}[first_].seq <= through)
        {
            first_ = (first_ + 1U) % blocks_.size();
            --count_;
            ++freed;
        }

        return freed;
    }

    // Copies the oldest blocks into `out`, at most its size: how many.
    [[nodiscard]] constexpr std::size_t copy_oldest(std::span<ResultBlock> out) const noexcept
    {
        const std::size_t copied = std::min(out.size(), std::size_t{count_});

        for (std::size_t index = 0U; index < copied; ++index)
        {
            out[index] = std::span{blocks_}[(first_ + index) % blocks_.size()];
        }

        return copied;
    }

    // The oldest block's number, 0 when there is none.
    [[nodiscard]] constexpr std::uint32_t oldest_seq() const noexcept { return count_ == 0U ? 0U : std::span{blocks_}[first_].seq; }

    // The number the next block gets.
    [[nodiscard]] constexpr std::uint32_t next_seq() const noexcept { return next_seq_; }

private:
    std::array<ResultBlock, result_capacity> blocks_{};
    std::size_t first_{};
    std::uint32_t count_{};
    std::uint32_t next_seq_{first_seq};
};

// What the playlist is doing (PLAYLIST? state).
enum class State : std::uint8_t
{
    idle = 0U,             // none since boot
    running = 1U,          // an item is being prepared, waits for its stream index, or plays
    gap = 2U,              // waiting the gap after an item
    paused_ring_full = 3U, // the result ring is full: the next item waits for PLAYLIST ACK
    done = 4U,             // every item ran and the last gap passed
    aborted = 5U,          // STOP
    error = 6U,            // an item failed under the stop policy
};

[[nodiscard]] constexpr std::string_view state_name(State state) noexcept
{
    switch (state)
    {
    case State::idle:
        return "idle";

    case State::running:
        return "running";

    case State::gap:
        return "gap";

    case State::paused_ring_full:
        return "paused_ring_full";

    case State::done:
        return "done";

    case State::aborted:
        return "aborted";

    case State::error:
        return "error";
    }

    return "error";
}

// What an item error does: stop the playlist (the default) or go on with the
// next item (PLAYLIST:<NAME>:CONTINUE).
enum class Policy : std::uint8_t
{
    stop = 0U,
    continue_on_error = 1U,
};

[[nodiscard]] constexpr std::string_view policy_name(Policy policy) noexcept
{
    return policy == Policy::continue_on_error ? "continue" : "stop";
}

inline constexpr std::string_view continue_suffix{"CONTINUE"};

// The running or last playlist.
struct Progress
{
    State state{State::idle};
    ShortName name{};
    std::uint32_t run{}; // playlists started since boot; this one's number
    Policy policy{Policy::stop};
    std::uint32_t items{};     // in the file
    std::uint32_t index{};     // the item running or next; items once all have run
    std::uint32_t completed{}; // items that ended, whatever their outcome
    std::uint32_t failed{};    // of those, the ones that ended in an error
};

// A PLAYLIST? reply's content, read in one critical section: the progress,
// the ring's numbers, the RTOS tick (ms since the scheduler started, the
// blocks' start_tick and end_tick clock) and the oldest blocks. A drain that
// lost its connection tells a board reset (the tick restarted) from a USB
// re-enumeration (it ran on) by the tick.
struct Snapshot
{
    Progress progress{};
    std::uint32_t pending{};
    std::uint32_t oldest_seq{};
    std::uint32_t next_seq{};
    std::uint32_t tick{};
    std::array<ResultBlock, blocks_per_reply> blocks{};
    std::size_t block_count{};
};

[[nodiscard]] constexpr bool is_active(State state) noexcept
{
    return state == State::running || state == State::gap || state == State::paused_ring_full;
}

// The state a playlist ends in after an item ended with `outcome`, or none
// when it goes on (to the gap, then the next item): aborted after STOP,
// error after an item error under the stop policy.
[[nodiscard]] constexpr std::optional<State> end_after(Outcome outcome, Policy policy, bool stop_requested) noexcept
{
    if (stop_requested || outcome == Outcome::aborted)
    {
        return State::aborted;
    }

    if (outcome == Outcome::error && policy == Policy::stop)
    {
        return State::error;
    }

    return std::nullopt;
}

// One packet of the USB sound card's stream per millisecond, 48 frames
// (rt1170/usb_audio.hpp chunk_frames). A USB item's first frame leads a
// packet, and its last packet ends after its last frame.
inline constexpr std::uint32_t chunk_frames = frames_per_millisecond;

// `frames` rounded up to whole packets. Stream indices wrap after 2^32
// frames, and so does this.
[[nodiscard]] constexpr std::uint32_t whole_chunk_frames(std::uint32_t frames) noexcept
{
    const std::uint32_t partial = frames % chunk_frames == 0U ? 0U : 1U;
    return (frames / chunk_frames + partial) * chunk_frames;
}

// The stream index where the gap after a USB item ends, and where the next
// USB item's first frame goes out: from the item's first frame, its frames
// sent rounded up to whole packets, then the gap. Items are thus an exact
// number of frames apart, whatever the time each took to prepare.
[[nodiscard]] constexpr std::uint32_t next_start_index(std::uint32_t first_frame_index, std::uint32_t sent_frames, GapFrames gap) noexcept
{
    return first_frame_index + whole_chunk_frames(sent_frames) + gap.value;
}

// The rules on worked examples: a line with every option (5.25 s, two cuts,
// seed 7, on line 3), a comment, a USB item on an image without the sound
// card, the gap's limits and spelling, and where the next item starts.
[[nodiscard]] consteval bool rules_hold() noexcept
{
    constexpr OfferedRoutes both{.usb = true};
    constexpr LineNumber third{.value = 3U};
    constexpr std::uint32_t example_gap_ms = 5250U;
    constexpr std::uint32_t example_seed = 7U;
    constexpr std::uint32_t example_first_index = 10U * chunk_frames;
    constexpr GapFrames example_gap{.value = 2U * chunk_frames};
    constexpr attenuation::Tenths example_attenuation = attenuation::Tenths::of(205U).value();
    constexpr DurationFrames example_duration{.value = 60500U * frames_per_millisecond};
    const auto item = parse_line("  C24L05.WAV\t5.25 USB cut=0:4800:zero,9600:1:drop seed=7 dur=60.5 att=-20.5 scn=FADE.SCN label=c24-l05 # first", third, both);

    if (!item.has_value())
    {
        return false;
    }

    const auto& entry = item.value();
    const bool parsed = entry.has_value() && entry->gap == GapFrames{.value = example_gap_ms * frames_per_millisecond} && entry->route == Route::usb && entry->cuts.size() == 2U &&
                        entry->seed == example_seed && entry->scenario.view() == "FADE.SCN" && entry->label.view() == "c24-l05" && entry->attenuation == example_attenuation &&
                        entry->duration == example_duration && entry->line == third;
    const auto comment = parse_line(" # only a comment", LineNumber{.value = 1U}, both);
    const auto unoffered = parse_line("A.WAV 0 USB", LineNumber{.value = 1U}, OfferedRoutes{});
    const bool gaps = parse_gap("3600").has_value() && !parse_gap("3600.001").has_value() && !parse_gap("05").has_value() && !parse_gap("1.").has_value() &&
                      parse_gap("0.001").value() == GapFrames{.value = frames_per_millisecond} && parse_gap("0").value() == GapFrames{};
    const bool starts =
        next_start_index(example_first_index, 1U, example_gap) == example_first_index + chunk_frames + example_gap.value && next_start_index(0U, 2U * chunk_frames, GapFrames{}) == 2U * chunk_frames;
    return parsed && comment.has_value() && !comment.value().has_value() && unoffered.status().message == route_unavailable.message && gaps && starts;
}

// att= and dur= on worked examples: a positive level and a second att= are
// refused; a duration is 1 to 86400 s with at most 3 decimals.
[[nodiscard]] consteval bool level_and_length_hold() noexcept
{
    constexpr OfferedRoutes both{.usb = true};
    constexpr LineNumber first{.value = 1U};
    const auto louder = parse_line("A.WAV 0 USB att=3", first, both);
    const auto twice = parse_line("A.WAV 0 USB att=-1 att=-2", first, both);
    const auto shortest = parse_line("A.WAV 0 USB dur=0.999", first, both);
    const bool attenuations = louder.status().message == bad_attenuation.message && twice.status().message == bad_syntax.message;
    const bool durations = parse_duration("1").value() == min_duration && parse_duration("86400.000").value() == max_duration && !parse_duration("86400.001").has_value() &&
                           !parse_duration("01").has_value() && !parse_duration("1.0001").has_value() && shortest.status().message == bad_duration.message;
    return attenuations && durations;
}

static_assert(rules_hold(), "a whole line parses; comments are ignored; gaps are 0 to 3600 s in whole milliseconds; USB needs the image's sound card; "
                            "the next item starts the gap after the packet that ends the item");
static_assert(level_and_length_hold(), "att= never raises the level and is given once; dur= is 1 to 86400 s in whole milliseconds");
static_assert(max_gap.value / frames_per_millisecond == max_gap_milliseconds && max_gap.value < std::numeric_limits<std::uint32_t>::max() / 2U,
              "every gap is a whole number of milliseconds and less than half the stream index's range");
static_assert(end_after(Outcome::error, Policy::continue_on_error, false) == std::nullopt && end_after(Outcome::error, Policy::stop, false) == State::error &&
                  end_after(Outcome::done, Policy::stop, true) == State::aborted && end_after(Outcome::done, Policy::stop, false) == std::nullopt,
              "STOP aborts; an error stops only under the stop policy");
static_assert(is_playlist_name("BATTERY.PL") && is_playlist_name("LADDER") && !is_playlist_name("battery.pl") && !is_playlist_name("TOOLONGNAME.PL") && !is_playlist_name("A.PLAY") &&
                  !is_playlist_name(".PL") && !is_playlist_name("A."),
              "the playlist's name is an upper-case 8.3 name, its extension optional");

} // namespace waveform_generator::playlist

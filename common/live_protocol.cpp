// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "common/live_protocol.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <span>

namespace waveform_generator::live_protocol
{
m110::Result<std::uint64_t> uint64(std::string_view text) noexcept
{
    if (text.empty() || (text.size() > 1 && text.front() == '0'))
    {
        return bad_request;
    }

    std::uint64_t value = 0U;

    for (const char c : text)
    {
        if (c < '0' || c > '9')
        {
            return bad_request;
        }

        const auto digit = static_cast<unsigned>(c - '0');

        if (value > (UINT64_MAX - digit) / 10)
        {
            return bad_request;
        }

        value = value * 10 + digit;
    }

    return value;
}

bool filename(std::string_view text, std::string_view extension) noexcept
{
    const auto dot = text.find('.');

    if (dot == 0 || dot > 8 || dot == text.npos || text.substr(dot + 1) != extension)
    {
        return false;
    }

    for (const char c : text.substr(0, dot))
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
        {
            return false;
        }

    return true;
}

namespace
{
// GENERATE:<rate>:<interleave>:<input>:<output>'s fields, the word with them.
constexpr std::size_t generate_fields = 5U;
// The longest fade and sweep interval, in milliseconds: an hour.
constexpr std::uint32_t longest_milliseconds = 3600000U;
// The most values a sweep takes.
constexpr std::uint32_t most_sweep_steps = 16U;

[[nodiscard]] m110::Result<double> decimal(std::string_view text) noexcept
{
    bool negative = false;

    if (!text.empty() && text.front() == '-')
    {
        negative = true;
        text.remove_prefix(1);
    }

    if (text.empty() || text.size() > 16)
    {
        return bad_request;
    }

    bool fraction = false;
    bool digit = false;
    double scale = 1.0;
    double value = 0.0;

    for (const char c : text)
    {
        if (c == '.' && !fraction && digit)
        {
            fraction = true;
            digit = false;
            continue;
        }

        if (c < '0' || c > '9')
        {
            return bad_request;
        }

        digit = true;

        if (fraction)
        {
            scale *= 0.1;
            value += static_cast<double>(c - '0') * scale;
        }
        else
        {
            value = value * 10.0 + static_cast<double>(c - '0');
        }
    }

    if (!digit)
    {
        return bad_request;
    }

    if (negative)
    {
        value = -value;
    }

    return value;
}

[[nodiscard]] m110::Result<std::uint32_t> number32(std::string_view text) noexcept
{
    const m110::Result<std::uint64_t> parsed = uint64(text);

    if (!parsed.has_value() || parsed.value() > UINT32_MAX)
    {
        return bad_request;
    }

    return static_cast<std::uint32_t>(parsed.value());
}

// A CW slot's number: a canonical decimal below cw_oscillator_capacity, else
// bad_request.
[[nodiscard]] m110::Result<std::uint32_t> cw_slot(std::string_view text) noexcept
{
    const m110::Result<std::uint32_t> slot = number32(text);

    if (!slot.has_value() || slot.value() >= cw_oscillator_capacity)
    {
        return bad_request;
    }

    return slot;
}

// REFERENCE's level: a decimal in (0, 1], else bad_request.
[[nodiscard]] m110::Result<double> reference_level(std::string_view text) noexcept
{
    const m110::Result<double> level = decimal(text);

    if (!level.has_value() || !(level.value() > 0.0) || !(level.value() <= 1.0))
    {
        return bad_request;
    }

    return level;
}

// A fade's duration or a sweep's interval: 1 to longest_milliseconds, else
// bad_request.
[[nodiscard]] m110::Result<std::uint32_t> milliseconds(std::string_view text) noexcept
{
    const m110::Result<std::uint32_t> duration = number32(text);

    if (!duration.has_value() || duration.value() == 0U || duration.value() > longest_milliseconds)
    {
        return bad_request;
    }

    return duration;
}

// The words of a command, in order: `count` of them, the rest empty.
struct Words
{
    static constexpr std::size_t capacity = 8U;

    std::array<std::string_view, capacity> words{};
    std::size_t count{};
};

// `text` cut at each `separator` into one to Words::capacity words, none
// empty, else bad_request.
[[nodiscard]] m110::Result<Words> split(std::string_view text, char separator = ' ') noexcept
{
    Words found{};

    while (!text.empty())
    {
        if (found.count == Words::capacity)
        {
            return bad_request;
        }

        auto end = text.find(separator);

        if (end == text.npos)
        {
            end = text.size();
        }

        if (end == 0)
        {
            return bad_request;
        }

        std::span{found.words}[found.count] = text.substr(0, end);
        ++found.count;

        if (end == text.size())
        {
            return found;
        }

        text.remove_prefix(end + 1);

        if (text.empty())
        {
            return bad_request;
        }
    }

    return bad_request;
}

// A request line's sequence, and the command after it without the line's CR.
struct Sequenced
{
    std::uint32_t seq{};
    std::string_view command;
};

// The frame of a request line: with its LF it fits wire_capacity, it ends in
// at most one CR, it holds printable ASCII only, and it starts with a
// canonical 32-bit sequence above 0 and a space; else bad_request.
[[nodiscard]] m110::Result<Sequenced> read_sequence(std::string_view text) noexcept
{
    constexpr char carriage_return = '\r';
    constexpr unsigned char first_printable = ' ';
    constexpr unsigned char last_printable = '~';

    if (text.size() + 1U > wire_capacity)
    {
        return bad_request;
    }

    if (text.ends_with(carriage_return))
    {
        text.remove_suffix(1U);
    }

    if (std::ranges::any_of(text, [](unsigned char c) { return c < first_printable || c > last_printable; }))
    {
        return bad_request;
    }

    const auto space = text.find(' ');

    if (space == std::string_view::npos)
    {
        return bad_request;
    }

    const m110::Result<std::uint32_t> seq = number32(text.substr(0U, space));

    if (!seq.has_value() || seq.value() == 0U)
    {
        return bad_request;
    }

    return Sequenced{.seq = seq.value(), .command = text.substr(space + 1U)};
}

// CUT, or CUT:<entries>: the entries as given, which the player judges when
// it takes them (cut::validate()); cut::bad_syntax for any other spelling.
[[nodiscard]] m110::Status parse_cut(std::string_view text, Request& request) noexcept
{
    const auto cuts = cut::parse_command(text);

    if (!cuts.has_value())
    {
        return cuts.status();
    }

    request.kind = Kind::cut;
    request.cuts = cuts.value();
    return m110::Status::success();
}

// The canonical uint64 offset of FILES?; its no-offset form is matched by the
// plain query table before this dispatcher.
[[nodiscard]] m110::Status parse_files_offset(std::string_view text, Request& request) noexcept
{
    const m110::Result<std::uint64_t> offset = uint64(text);

    if (!offset.has_value())
    {
        return bad_request;
    }

    request.kind = Kind::files;
    request.integer = offset.value();
    return m110::Status::success();
}

[[nodiscard]] m110::Status store_prepare_request(const std::array<std::string_view, 3U>& filenames, std::string_view policy, bool seed_override, std::string_view seed_text, Request& request) noexcept
{
    if ((policy != "KEEP" && policy != "TEMP") || !filename(filenames[0], "WAV") || !filename(filenames[1], "WAV") || !filename(filenames[2], "SCN"))
    {
        return bad_request;
    }

    const auto seed = seed_override ? uint64(seed_text) : m110::Result<std::uint64_t>{0U};

    if (!seed.has_value())
    {
        return bad_request;
    }

    std::ranges::copy(filenames[0], request.preparation.output.begin());
    std::ranges::copy(filenames[1], request.preparation.source.begin());
    std::ranges::copy(filenames[2], request.preparation.scenario.begin());
    request.preparation.retention = policy == "KEEP" ? prepare::Retention::keep : prepare::Retention::temporary;
    request.preparation.seed_override = seed_override;
    request.preparation.seed = seed.value();
    return m110::Status::success();
}

[[nodiscard]] m110::Status parse_prepare(std::string_view text, Request& request) noexcept
{
    constexpr std::string_view prefix{"PREPARE:"};

    if (!text.starts_with(prefix))
    {
        return bad_request;
    }

    text.remove_prefix(prefix.size());
    std::array<std::string_view, 3U> filenames{};

    for (auto& field : filenames)
    {
        const auto separator = text.find(':');

        if (separator == std::string_view::npos)
        {
            return bad_request;
        }

        field = text.substr(0U, separator);
        text.remove_prefix(separator + 1U);
    }

    const auto policy_separator = text.find(':');
    const auto policy = text.substr(0U, policy_separator);
    const bool seed_override = policy_separator != std::string_view::npos;
    const auto seed_text = seed_override ? text.substr(policy_separator + 1U) : std::string_view{};

    if (const auto stored = store_prepare_request(filenames, policy, seed_override, seed_text, request); !stored.is_ok())
    {
        return stored;
    }

    request.kind = Kind::prepare_file;
    return m110::Status::success();
}

[[nodiscard]] m110::Status parse_prepare_play(std::string_view text, Request& request) noexcept
{
    constexpr std::string_view prefix{"PREPAREPLAY:"};

    if (!text.starts_with(prefix))
    {
        return bad_request;
    }

    text.remove_prefix(prefix.size());
    std::array<std::string_view, 5U> fields{};

    for (auto& field : fields)
    {
        const auto separator = text.find(':');

        if (separator == std::string_view::npos)
        {
            return bad_request;
        }

        field = text.substr(0U, separator);
        text.remove_prefix(separator + 1U);
    }

    const auto seed_separator = text.find(':');
    const auto margin_text = text.substr(0U, seed_separator);
    const bool seed_override = seed_separator != std::string_view::npos;
    const auto seed_text = seed_override ? text.substr(seed_separator + 1U) : std::string_view{};
    const auto rate = number32(fields[4]);
    const auto margin = number32(margin_text);
    const std::array<std::string_view, 3U> filenames{fields[0], fields[1], fields[2]};

    if (!rate.has_value() || rate.value() == 0U || !margin.has_value() || !store_prepare_request(filenames, fields[3], seed_override, seed_text, request).is_ok())
    {
        return bad_request;
    }

    request.preparation_rate = rate.value();
    request.preparation_margin_frames = margin.value();
    request.kind = Kind::prepare_play;
    return m110::Status::success();
}

constexpr std::string_view playlist_word{"PLAYLIST"};
constexpr std::string_view playlist_prefix{"PLAYLIST:"};
constexpr std::string_view playlist_ack_prefix{"PLAYLIST ACK:"};
constexpr char playlist_field_separator = ':';
static_assert(sizeof(Request::name) > playlist::name_max_chars, "a playlist's name fits the request's name with its terminator");

// PLAYLIST:<NAME> or PLAYLIST:<NAME>:CONTINUE, without its prefix: the
// playlist's 8.3 name (playlist::is_playlist_name()) and its error policy.
[[nodiscard]] m110::Status parse_playlist_start(std::string_view text, Request& request) noexcept
{
    const auto separator = text.find(playlist_field_separator);
    const auto name = text.substr(0U, separator);

    if (!playlist::is_playlist_name(name))
    {
        return playlist::bad_name;
    }

    if (separator != std::string_view::npos)
    {
        if (text.substr(separator + 1U) != playlist::continue_suffix)
        {
            return playlist::bad_policy;
        }

        request.policy = playlist::Policy::continue_on_error;
    }

    std::ranges::copy(name, std::span{request.name}.begin());
    request.kind = Kind::playlist_start;
    return m110::Status::success();
}

// The commands with parsers of their own, CUT, PLAYLIST, offset-bearing
// FILES? (the plain query is matched before), and PREPARE admissions: the
// parser's verdict, or none for any other text.
[[nodiscard]] std::optional<m110::Status> parse_delegated(std::string_view text, Request& request) noexcept
{
    if (cut::is_command(text))
    {
        return parse_cut(text, request);
    }

    if (text.starts_with(playlist_word))
    {
        return parse_playlist_command(text, request);
    }

    constexpr std::string_view files_offset_prefix{"FILES? "};

    if (text.starts_with(files_offset_prefix))
    {
        return parse_files_offset(text.substr(files_offset_prefix.size()), request);
    }

    if (text.starts_with("PREPARE:"))
    {
        return parse_prepare(text, request);
    }

    if (text.starts_with("PREPAREPLAY:"))
    {
        return parse_prepare_play(text, request);
    }

    return std::nullopt;
}

// `request`, or bad_request when `verdict`, a delegated parser's, refused it.
[[nodiscard]] m110::Result<Request> judged(const Request& request, m110::Status verdict) noexcept
{
    if (!verdict.is_ok())
    {
        return bad_request;
    }

    return request;
}
} // namespace

m110::Status parse_playlist_command(std::string_view text, Request& request) noexcept
{
    if (text.starts_with(playlist_ack_prefix))
    {
        const m110::Result<std::uint32_t> acknowledged = number32(text.substr(playlist_ack_prefix.size()));

        if (!acknowledged.has_value())
        {
            return playlist::bad_ack;
        }

        request.ack_seq = acknowledged.value();
        request.kind = Kind::playlist_ack;
        return m110::Status::success();
    }

    if (!text.starts_with(playlist_prefix))
    {
        return playlist::bad_command;
    }

    return parse_playlist_start(text.substr(playlist_prefix.size()), request);
}

std::uint32_t reply_sequence(std::string_view text) noexcept
{
    const m110::Result<Sequenced> line = read_sequence(text);
    return line.has_value() ? line.value().seq : 0U;
}

m110::Result<Request> parse(std::string_view text, std::uint32_t& last_sequence) noexcept
{
    const m110::Result<Sequenced> line = read_sequence(text);

    if (!line.has_value())
    {
        return line.status();
    }

    Request request{};
    request.seq = line.value().seq;

    if (request.seq <= last_sequence)
    {
        return bad_request;
    }

    last_sequence = request.seq;
    text = line.value().command;

    if (text.starts_with("AT:"))
    {
        const auto end = text.find(' ');
        const m110::Result<std::uint64_t> frame = uint64(text.substr(3, end - 3));

        if (end == text.npos || !frame.has_value())
        {
            return bad_request;
        }

        request.frame = frame.value();
        request.scheduled = true;
        text.remove_prefix(end + 1);
    }

    if (!request.scheduled)
    {
        const struct
        {
            std::string_view name;
            Kind kind;
        } simple[] =

            {{"INFO?", Kind::info},
             {"STATUS?", Kind::status},
             {"COUNTERS?", Kind::counters},
             {"USB?", Kind::usb_diagnostics},
             {"PLAY", Kind::play},
             {"STOP", Kind::stop},
             {"MEDIA HOST", Kind::media_host},
             {"MEDIA LOCAL", Kind::media_local},
             {"OUTPUT CODEC", Kind::output_codec},
             {"OUTPUT USB", Kind::output_usb},
             {"PLAYLIST?", Kind::playlist_status},
             {"ENGINE?", Kind::engine_status},
             {"FILES?", Kind::files},
             {"PREPARE?", Kind::prepare_status},
             {"PREPAREPLAY?", Kind::prepare_play_status}};

        for (const auto& item : simple)
            if (text == item.name)
            {
                request.kind = item.kind;
                return request;
            }

        if (const auto delegated = parse_delegated(text, request); delegated.has_value())
        {
            return judged(request, *delegated);
        }

        if (text.starts_with("PLAY:"))
        {
            const auto id = text.substr(5);

            if (id.size() != 32)
            {
                return bad_request;
            }

            for (const char c : id)
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                {
                    return bad_request;
                }

            std::memcpy(request.run_id, id.data(), 32);
            request.kind = Kind::play;
            return request;
        }

        if (text.starts_with("LOAD:"))
        {
            text.remove_prefix(5);

            if (!filename(text, "WAV"))
            {
                return bad_request;
            }

            std::memcpy(request.name, text.data(), text.size());
            request.kind = Kind::load;
            return request;
        }

        if (text.starts_with("SEED:"))
        {
            const m110::Result<std::uint64_t> seed = uint64(text.substr(5));

            if (!seed.has_value())
            {
                return bad_request;
            }

            request.kind = Kind::seed;
            request.integer = seed.value();
            return request;
        }

        if (text.starts_with("GENERATE:"))
        {
            const m110::Result<Words> split_fields = split(text, ':');

            if (!split_fields.has_value() || split_fields.value().count != generate_fields)
            {
                return bad_request;
            }

            const auto& fields = split_fields.value().words;
            const m110::Result<std::uint32_t> rate = number32(fields[1]);

            if (!rate.has_value())
            {
                return bad_request;
            }

            request.rate = rate.value();

            if (request.rate != 75 && request.rate != 150 && request.rate != 300 && request.rate != 600 && request.rate != 1200 && request.rate != 2400 && request.rate != 4800)
            {
                return bad_request;
            }

            if (fields[2] == "short")
            {
                request.interleave = 0;
            }
            else if (fields[2] == "long")
            {
                request.interleave = 1;
            }
            else if (fields[2] == "zero")
            {
                request.interleave = 2;
            }
            else
            {
                return bad_request;
            }

            if (!filename(fields[3], "BIN") || !filename(fields[4], "WAV"))
            {
                return bad_request;
            }

            std::memcpy(request.input_name, fields[3].data(), fields[3].size());
            std::memcpy(request.name, fields[4].data(), fields[4].size());
            request.kind = Kind::generate_file;
            return request;
        }

        if (text.starts_with("REFERENCE:"))
        {
            const m110::Result<double> reference = reference_level(text.substr(10));

            if (!reference.has_value())
            {
                return bad_request;
            }

            request.kind = Kind::reference;
            request.value = reference.value();
            return request;
        }
    }

    const m110::Result<Words> split_words = split(text);

    if (!split_words.has_value())
    {
        return bad_request;
    }

    const auto& [words, count] = split_words.value();

    if (!request.scheduled && count == 5 && words[0] == "ENCODE")
    {
        if (words[1].empty() || words[1].size() >= sizeof(request.encoder) || words[2].empty() || words[2].size() >= sizeof(request.profile) || !filename(words[3], "BIN") ||
            !filename(words[4], "WAV"))
        {
            return bad_request;
        }

        for (const char c : words[1])
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            {
                return bad_request;
            }

        for (const char c : words[2])
            if (c == '"' || c == '\\')
            {
                return bad_request;
            }

        std::memset(request.encoder, 0, sizeof(request.encoder));
        std::memcpy(request.encoder, words[1].data(), words[1].size());
        std::memcpy(request.profile, words[2].data(), words[2].size());
        std::memcpy(request.input_name, words[3].data(), words[3].size());
        std::memcpy(request.name, words[4].data(), words[4].size());
        request.kind = Kind::generate_file;
        return request;
    }

    if (count == 2 && (words[0] == "CW" || words[0] == "STATIC") && (words[1] == "ON" || words[1] == "OFF"))
    {
        request.kind = words[0] == "CW" ? Kind::cw_on : Kind::static_on;
        request.value = words[1] == "ON" ? 1 : 0;
        return request;
    }

    if (count == 3 && words[0] == "CW" && (words[2] == "ON" || words[2] == "OFF"))
    {
        const m110::Result<std::uint32_t> oscillator = cw_slot(words[1]);

        if (!oscillator.has_value())
        {
            return bad_request;
        }

        request.oscillator = oscillator.value();
        request.kind = Kind::cw_on;
        request.value = words[2] == "ON" ? 1 : 0;
        return request;
    }

    if (const m110::Result<double> value = decimal(words[2]); count == 3 && value.has_value())
    {
        request.value = value.value();

        if (words[0] == "CW" && words[1] == "FREQ")
        {
            request.kind = Kind::cw_frequency;
        }
        else if (words[0] == "CW" && words[1] == "CI")
        {
            request.kind = Kind::cw_ci;
        }
        else if (words[0] == "STATIC" && words[1] == "RATE")
        {
            request.kind = Kind::static_rate;
        }
        else if (words[0] == "STATIC" && words[1] == "PEAK")
        {
            request.kind = Kind::static_peak;
        }
        else
        {
            return bad_request;
        }

        return request; // live controller validates physical ranges atomically
    }

    if (const m110::Result<double> value = decimal(words[3]); count == 4 && words[0] == "CW" && value.has_value())
    {
        const m110::Result<std::uint32_t> oscillator = cw_slot(words[1]);

        if (!oscillator.has_value())
        {
            return bad_request;
        }

        request.oscillator = oscillator.value();
        request.value = value.value();

        if (words[2] == "FREQ")
        {
            request.kind = Kind::cw_frequency;
        }
        else if (words[2] == "CI")
        {
            request.kind = Kind::cw_ci;
        }
        else
        {
            return bad_request;
        }

        return request;
    }

    if (count == 4 && words[0] == "FADE" && words[1] == "NOW")
    {
        const m110::Result<double> depth = decimal(words[2]);
        const m110::Result<std::uint32_t> duration = milliseconds(words[3]);

        if (!depth.has_value() || !duration.has_value())
        {
            return bad_request;
        }

        request.kind = Kind::fade;
        request.value = depth.value();
        request.duration_ms = duration.value();
        return request;
    }

    if ((count == 7 || count == 8) && words[0] == "SWEEP")
    {
        std::size_t first = 3;

        if (words[1] == "CW")
        {
            std::size_t kind = 2;

            if (count == 8)
            {
                const m110::Result<std::uint32_t> oscillator = cw_slot(words[2]);

                if (!oscillator.has_value())
                {
                    return bad_request;
                }

                request.oscillator = oscillator.value();
                kind = 3;
                first = 4;
            }

            if (words[kind] == "FREQ")
            {
                request.kind = Kind::sweep_frequency;
            }
            else if (words[kind] == "CI")
            {
                request.kind = Kind::sweep_ci;
            }
            else
            {
                return bad_request;
            }
        }
        else if (count == 7 && words[1] == "FADE")
        {
            const m110::Result<std::uint32_t> duration = milliseconds(words[6]);

            if (!duration.has_value())
            {
                return bad_request;
            }

            request.kind = Kind::sweep_fade;
            request.duration_ms = duration.value();
            first = 2;
        }
        else
        {
            return bad_request;
        }

        // The start and end values, the steps and the interval.
        const std::span<const std::string_view> sweep = std::span{words}.subspan(first, 4U);
        const m110::Result<double> start = decimal(sweep[0]);
        const m110::Result<double> end = decimal(sweep[1]);
        const m110::Result<std::uint32_t> steps = number32(sweep[2]);
        const m110::Result<std::uint32_t> interval = milliseconds(sweep[3]);

        if (!start.has_value() || !end.has_value() || !steps.has_value() || steps.value() < 2 || steps.value() > most_sweep_steps || !interval.has_value())
        {
            return bad_request;
        }

        request.value = start.value();
        request.end_value = end.value();
        request.steps = steps.value();
        request.interval_ms = interval.value();
        return request;
    }

    return bad_request;
}
} // namespace waveform_generator::live_protocol

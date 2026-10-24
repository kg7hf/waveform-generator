// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "live_replay.hpp"

#include "common/live_command.hpp"
#include "common/live_protocol.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string_view>
#include <utility>

namespace waveform_generator
{
namespace
{

// A replay file is read whole into memory; anything larger is refused unread.
constexpr std::streamoff maximum_replay_bytes = std::streamoff{1024} * 1024;
// The deepest nesting of a value the reader skips.
constexpr unsigned int maximum_depth = 32U;

// The JSON reader's refusals (invalid_argument); read_live_replay() reports
// each with the message of the part of the plan it was reading.
constexpr m110::Status malformed{.code = m110::StatusCode::invalid_argument, .message = "malformed JSON"};
constexpr m110::Status too_deep{.code = m110::StatusCode::invalid_argument, .message = "JSON nested too deeply"};
constexpr m110::Status member_twice{.code = m110::StatusCode::invalid_argument, .message = "a JSON object has a member twice"};
constexpr m110::Status too_many_values{.code = m110::StatusCode::invalid_argument, .message = "a JSON array has more than 128 values"};
constexpr m110::Status not_ascii{.code = m110::StatusCode::invalid_argument, .message = "a decoded JSON string has a non-ASCII escape"};
constexpr m110::Status not_uint64{.code = m110::StatusCode::invalid_argument, .message = "not an exact uint64"};

// The characters below the space, which a JSON string may not hold as they
// are; the last ASCII character; a \u escape's hex digits, and their base.
constexpr unsigned char first_printable = 0x20U;
constexpr unsigned int last_ascii = 0x7FU;
constexpr unsigned int unicode_escape_digits = 4U;
constexpr unsigned int hex_base = 16U;
constexpr unsigned int decimal_digits = 10U;
constexpr std::array<std::string_view, 3U> json_literals{"true", "false", "null"};

// A hex digit's value, or malformed.
[[nodiscard]] constexpr m110::Result<unsigned int> hex_digit(char digit) noexcept
{
    if (digit >= '0' && digit <= '9')
    {
        return static_cast<unsigned int>(digit - '0');
    }

    if (digit >= 'a' && digit <= 'f')
    {
        return static_cast<unsigned int>(digit - 'a') + decimal_digits;
    }

    if (digit >= 'A' && digit <= 'F')
    {
        return static_cast<unsigned int>(digit - 'A') + decimal_digits;
    }

    return malformed;
}

// Each range's ends, and the characters just outside them.
static_assert(hex_digit('0').value() == 0U && hex_digit('9').value() == decimal_digits - 1U && hex_digit('a').value() == decimal_digits && hex_digit('f').value() == hex_base - 1U &&
              hex_digit('A').value() == decimal_digits && hex_digit('F').value() == hex_base - 1U && !hex_digit('g').has_value() && !hex_digit('G').has_value() && !hex_digit('/').has_value() &&
              !hex_digit(':').has_value() && !hex_digit('@').has_value() && !hex_digit('`').has_value());

// The device's JSON reader intentionally holds only 256 nodes. A host journal
// can contain 128 commands plus snapshots, so scan values without constructing
// a DOM and retain only the exported top-level members and control records.
class JsonReader
{
public:
    using Members = std::vector<std::pair<std::string, std::string_view>>;
    explicit JsonReader(std::string_view text) : text_{text} {}

    // The text is one object: its members, each name with its value's text,
    // or the Status of the first refusal.
    [[nodiscard]] m110::Result<Members> object()
    {
        Members members;

        if (!take('{'))
        {
            return malformed;
        }

        if (take('}'))
        {
            return ended(std::move(members));
        }

        for (;;)
        {
            std::string key;

            if (const m110::Status read = string(&key); !read.is_ok())
            {
                return read;
            }

            if (!take(':'))
            {
                return malformed;
            }

            whitespace();
            const auto begin = cursor_;

            if (const m110::Status read = value(0U); !read.is_ok())
            {
                return read;
            }

            for (const auto& member : members)
                if (member.first == key)
                {
                    return member_twice;
                }

            members.emplace_back(std::move(key), text_.substr(begin, cursor_ - begin));

            if (take('}'))
            {
                return ended(std::move(members));
            }

            if (!take(','))
            {
                return malformed;
            }
        }
    }

    // The text is one array of at most live_capture_capacity values: each
    // value's text, or the Status of the first refusal.
    [[nodiscard]] m110::Result<std::vector<std::string_view>> array()
    {
        std::vector<std::string_view> values;

        if (!take('['))
        {
            return malformed;
        }

        if (take(']'))
        {
            return ended(std::move(values));
        }

        for (;;)
        {
            whitespace();
            const auto begin = cursor_;

            if (const m110::Status read = value(0U); !read.is_ok())
            {
                return read;
            }

            values.push_back(text_.substr(begin, cursor_ - begin));

            if (values.size() > signal_lab::live_capture_capacity)
            {
                return too_many_values;
            }

            if (take(']'))
            {
                return ended(std::move(values));
            }

            if (!take(','))
            {
                return malformed;
            }
        }
    }

    // The text is one string: its characters, escapes decoded (ASCII only),
    // or the Status of the refusal.
    [[nodiscard]] m110::Result<std::string> decoded()
    {
        std::string result;

        if (const m110::Status read = string(&result); !read.is_ok())
        {
            return read;
        }

        return ended(std::move(result));
    }

private:
    void whitespace()
    {
        while (cursor_ < text_.size() && (text_[cursor_] == ' ' || text_[cursor_] == '\t' || text_[cursor_] == '\r' || text_[cursor_] == '\n'))
        {
            ++cursor_;
        }
    }

    bool finish()
    {
        whitespace();
        return cursor_ == text_.size();
    }

    // `read`, when only whitespace follows it; malformed otherwise.
    template <typename T> [[nodiscard]] m110::Result<T> ended(T read)
    {
        if (!finish())
        {
            return malformed;
        }

        return read;
    }

    bool take(char c)
    {
        whitespace();

        if (cursor_ == text_.size() || text_[cursor_] != c)
        {
            return false;
        }

        ++cursor_;
        return true;
    }

    // A string at the cursor, its characters appended to `output` (escapes
    // decoded; a non-ASCII one refused) or skipped when it is null: success,
    // or the Status of the refusal.
    [[nodiscard]] m110::Status string(std::string* output)
    {
        if (!take('"'))
        {
            return malformed;
        }

        while (cursor_ < text_.size())
        {
            const auto character = static_cast<unsigned char>(text_[cursor_++]);

            if (character == '"')
            {
                return m110::Status::success();
            }

            if (character < first_printable)
            {
                return malformed;
            }

            const m110::Result<unsigned char> kept = character == '\\' ? escape(output != nullptr) : m110::Result<unsigned char>{character};

            if (!kept.has_value())
            {
                return kept.error();
            }

            if (output != nullptr)
            {
                output->push_back(static_cast<char>(kept.value()));
            }
        }

        return malformed;
    }

    // The character the escape at the cursor (after its backslash) stands
    // for, or the Status of the refusal. `decoding`: the string is kept, so
    // a non-ASCII \u escape is refused.
    [[nodiscard]] m110::Result<unsigned char> escape(bool decoding)
    {
        if (cursor_ == text_.size())
        {
            return malformed;
        }

        const char escaped = text_[cursor_++];

        switch (escaped)
        {
        case '"':
        case '\\':
        case '/':
            return static_cast<unsigned char>(escaped);

        case 'b':
            return static_cast<unsigned char>('\b');

        case 'f':
            return static_cast<unsigned char>('\f');

        case 'n':
            return static_cast<unsigned char>('\n');

        case 'r':
            return static_cast<unsigned char>('\r');

        case 't':
            return static_cast<unsigned char>('\t');

        case 'u':
            return unicode_escape(decoding);

        default:
            return malformed;
        }
    }

    // The four hex digits of a \u escape at the cursor, as the character they
    // give, or the Status of the refusal. Protocol commands and schema names
    // are ASCII; other metadata is skipped without decoding and may hold any
    // Unicode.
    [[nodiscard]] m110::Result<unsigned char> unicode_escape(bool decoding)
    {
        unsigned int code = 0U;

        for (unsigned int digit = 0U; digit < unicode_escape_digits; ++digit)
        {
            const m110::Result<unsigned int> value = cursor_ == text_.size() ? m110::Result<unsigned int>{malformed} : hex_digit(text_[cursor_++]);

            if (!value.has_value())
            {
                return value.error();
            }

            code = code * hex_base + value.value();
        }

        if (decoding && code > last_ascii)
        {
            return not_ascii;
        }

        return static_cast<unsigned char>(code);
    }

    // Skips one value at the cursor, nested at most maximum_depth deep below
    // `depth`: success, or the Status of the refusal.
    [[nodiscard]] m110::Status value(unsigned int depth)
    {
        whitespace();

        if (cursor_ == text_.size())
        {
            return malformed;
        }

        if (depth > maximum_depth)
        {
            return too_deep;
        }

        const char first = text_[cursor_];

        if (first == '"')
        {
            return string(nullptr);
        }

        if (first == '{' || first == '[')
        {
            return container(first == '{', depth);
        }

        for (const std::string_view literal : json_literals)
        {
            if (text_.substr(cursor_, literal.size()) == literal)
            {
                cursor_ += literal.size();
                return m110::Status::success();
            }
        }

        return number();
    }

    // Skips the object or array at the cursor, its values nested at most
    // maximum_depth deep below `depth`: success, or the Status of the refusal.
    [[nodiscard]] m110::Status container(bool object, unsigned int depth)
    {
        const char end = object ? '}' : ']';
        ++cursor_;

        if (take(end))
        {
            return m110::Status::success();
        }

        for (;;)
        {
            if (object && (!string(nullptr).is_ok() || !take(':')))
            {
                return malformed;
            }

            if (const m110::Status read = value(depth + 1U); !read.is_ok())
            {
                return read;
            }

            if (take(end))
            {
                return m110::Status::success();
            }

            if (!take(','))
            {
                return malformed;
            }
        }
    }

    // Skips the number at the cursor (a minus, an integer part without a
    // leading zero, then a fraction and an exponent, each with a digit):
    // success, or malformed.
    [[nodiscard]] m110::Status number()
    {
        if (text_[cursor_] == '-')
        {
            ++cursor_;
        }

        if (cursor_ < text_.size() && text_[cursor_] == '0')
        {
            ++cursor_;
        }
        else if (skip_digits() == 0U)
        {
            return malformed;
        }

        if (cursor_ < text_.size() && text_[cursor_] == '.')
        {
            ++cursor_;

            if (skip_digits() == 0U)
            {
                return malformed;
            }
        }

        if (cursor_ < text_.size() && (text_[cursor_] == 'e' || text_[cursor_] == 'E'))
        {
            ++cursor_;

            if (cursor_ < text_.size() && (text_[cursor_] == '+' || text_[cursor_] == '-'))
            {
                ++cursor_;
            }

            if (skip_digits() == 0U)
            {
                return malformed;
            }
        }

        return m110::Status::success();
    }

    // Skips the decimal digits at the cursor: how many there were.
    std::size_t skip_digits()
    {
        const auto begin = cursor_;

        while (cursor_ < text_.size() && text_[cursor_] >= '0' && text_[cursor_] <= '9')
        {
            ++cursor_;
        }

        return cursor_ - begin;
    }

    std::string_view text_;
    std::size_t cursor_{};
};

std::string_view find(const JsonReader::Members& members, const char* name)
{
    for (const auto& member : members)
        if (member.first == name)
        {
            return member.second;
        }

    return {};
}

// A JSON number or string token that is an exact uint64 in decimal: its
// value, or not_uint64.
[[nodiscard]] m110::Result<std::uint64_t> integer(std::string_view token)
{
    std::string decoded;

    if (!token.empty() && token.front() == '"')
    {
        m110::Result<std::string> text = JsonReader(token).decoded();

        if (!text.has_value())
        {
            return not_uint64;
        }

        decoded = std::move(text).value();
        token = decoded;
    }

    const m110::Result<std::uint64_t> value = live_protocol::uint64(token);

    if (!value.has_value())
    {
        return not_uint64;
    }

    return value.value();
}

bool same_string(std::string_view token, std::string_view expected)
{
    const m110::Result<std::string> decoded = JsonReader(token).decoded();
    return decoded.has_value() && decoded.value() == expected;
}

// The plan's schema, and the protocol it was recorded with (live_protocol::version without its NUL).
constexpr std::string_view replay_schema = "wfg-live.replay/1";
constexpr std::string_view replay_protocol{std::begin(live_protocol::version), std::prev(std::end(live_protocol::version))};

// One record of a plan's controls, as the host recorded it.
struct RecordedControl
{
    std::string command;
    std::uint64_t frame{};
    std::uint64_t events{};
};

// A control record (an object with a string command and uint64 apply_frame
// and events): its members, or the Status of the first refusal.
[[nodiscard]] m110::Result<RecordedControl> recorded_control(std::string_view text)
{
    const m110::Result<JsonReader::Members> record = JsonReader(text).object();

    if (!record.has_value())
    {
        return record.error();
    }

    m110::Result<std::string> command = JsonReader(find(record.value(), "command")).decoded();
    const m110::Result<std::uint64_t> frame = integer(find(record.value(), "apply_frame"));
    const m110::Result<std::uint64_t> events = integer(find(record.value(), "events"));

    if (!command.has_value())
    {
        return command.error();
    }

    if (!frame.has_value() || !events.has_value())
    {
        return not_uint64;
    }

    return RecordedControl{.command = std::move(command).value(), .frame = frame.value(), .events = events.value()};
}

// A plan the reader refuses: invalid_argument with the static message.
[[nodiscard]] m110::Status refused(const char* message)
{
    return m110::Status{.code = m110::StatusCode::invalid_argument, .message = message};
}

} // namespace

m110::Status read_live_replay(const std::string& path, LiveReplay& replay)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);

    if (!file || file.tellg() < 0 || file.tellg() > maximum_replay_bytes)
    {
        return m110::Status{.code = m110::StatusCode::io_error, .message = "live replay must be a readable JSON file no larger than 1 MiB"};
    }

    file.seekg(0);
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    const m110::Result<JsonReader::Members> document = JsonReader(text).object();

    if (!document.has_value() || !same_string(find(document.value(), "schema"), replay_schema) || !same_string(find(document.value(), "protocol"), replay_protocol))
    {
        return refused("invalid wfg-live.replay/1 JSON document or protocol");
    }

    const JsonReader::Members& root = document.value();
    LiveReplay parsed;

    if (const m110::Result<std::uint64_t> seed = integer(find(root, "live_seed")); seed.has_value())
    {
        parsed.seed = seed.value();
    }
    else
    {
        return refused("live replay requires an exact uint64 live_seed");
    }

    const std::string reference{find(root, "live_reference_rms")};
    char* end = nullptr;
    parsed.reference_rms = std::strtod(reference.c_str(), &end);

    if (end == reference.c_str() || *end != '\0' || !std::isfinite(parsed.reference_rms) || parsed.reference_rms <= 0.0)
    {
        return refused("live replay requires the effective positive live_reference_rms");
    }

    const auto frames = find(root, "live_frames");

    if (!frames.empty())
    {
        parsed.has_frames = true;
        const m110::Result<std::uint64_t> count = integer(frames);

        if (!count.has_value())
        {
            return refused("invalid live_frames");
        }

        parsed.frames = count.value();
    }

    const m110::Result<std::vector<std::string_view>> listed = JsonReader(find(root, "controls")).array();

    if (!listed.has_value())
    {
        return refused("live replay controls must be an array of at most 128 records");
    }

    const std::vector<std::string_view>& controls = listed.value();
    signal_lab::LiveController validator;

    for (const auto control : controls)
    {
        const m110::Result<RecordedControl> recorded = recorded_control(control);

        if (!recorded.has_value())
        {
            return refused("invalid live replay command, apply_frame, or event count");
        }

        const std::string& command = recorded.value().command;
        // apply_control() writes the acknowledgement's apply_frame back: the same frame, scheduled.
        std::uint64_t frame = recorded.value().frame;
        const std::uint64_t expected_events = recorded.value().events;
        std::uint32_t last_sequence = 0U;
        const m110::Result<live_protocol::Request> parsed_command = live_protocol::parse("1 " + command, last_sequence);

        if (!parsed_command.has_value() || !live_protocol::is_control(parsed_command.value().kind) || parsed_command.value().scheduled)
        {
            return refused("replay command must be an unscheduled live control; apply_frame supplies its recorded time");
        }

        live_protocol::Request request = parsed_command.value();
        request.scheduled = true;
        request.frame = frame;

        if (!validator.reset(parsed.seed, parsed.reference_rms).is_ok())
        {
            return refused("live reference RMS is outside the supported range");
        }

        std::uint32_t count = 0U;

        if (!live_protocol::apply_control(request, validator, frame, count).is_ok() || count != expected_events)
        {
            return refused("live replay control was invalid or its event count disagrees with the recorded acknowledgement");
        }

        if (parsed.events.size() + count > signal_lab::live_capture_capacity)
        {
            return refused("live replay exceeds the 128-event capture capacity");
        }

        parsed.events.insert(parsed.events.end(), validator.capture_data(), validator.capture_data() + validator.capture_count());
    }

    std::stable_sort(parsed.events.begin(), parsed.events.end(), [](const auto& left, const auto& right) { return left.frame < right.frame; });

    std::size_t simultaneous = 0U;
    std::uint64_t previous_frame = 0U;

    for (const auto& event : parsed.events)
    {
        simultaneous = simultaneous != 0U && event.frame == previous_frame ? simultaneous + 1U : 1U;

        if (simultaneous > signal_lab::live_pending_capacity)
        {
            return refused("live replay exceeds the 64-event pending capacity at one output frame");
        }

        previous_frame = event.frame;
    }

    replay = std::move(parsed);
    return m110::Status::success();
}

} // namespace waveform_generator

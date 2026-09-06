#include "live_replay.hpp"

#include "common/live_command.hpp"
#include "common/live_protocol.hpp"

#include <algorithm>
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

// The device's JSON reader intentionally holds only 256 nodes. A host journal
// can contain 128 commands plus snapshots, so scan values without constructing
// a DOM and retain only the exported top-level members and control records.
class JsonReader
{
public:
    using Members = std::vector<std::pair<std::string, std::string_view>>;
    explicit JsonReader(std::string_view text) : text_{text} {}

    bool object(Members& members)
    {
        if (!take('{')) return false;
        if (take('}')) return finish();
        for (;;)
        {
            std::string key;
            if (!string(&key) || !take(':')) return false;
            whitespace();
            const auto begin = cursor_;
            if (!value(0U)) return false;
            for (const auto& member : members)
                if (member.first == key) return false;
            members.emplace_back(std::move(key), text_.substr(begin, cursor_ - begin));
            if (take('}')) return finish();
            if (!take(',')) return false;
        }
    }

    bool array(std::vector<std::string_view>& values)
    {
        if (!take('[')) return false;
        if (take(']')) return finish();
        for (;;)
        {
            whitespace();
            const auto begin = cursor_;
            if (!value(0U)) return false;
            values.push_back(text_.substr(begin, cursor_ - begin));
            if (values.size() > signal_lab::live_capture_capacity) return false;
            if (take(']')) return finish();
            if (!take(',')) return false;
        }
    }

    bool decoded(std::string& result) { return string(&result) && finish(); }

private:
    void whitespace()
    {
        while (cursor_ < text_.size() && (text_[cursor_] == ' ' || text_[cursor_] == '\t' || text_[cursor_] == '\r' || text_[cursor_] == '\n')) ++cursor_;
    }
    bool finish() { whitespace(); return cursor_ == text_.size(); }
    bool take(char c)
    {
        whitespace();
        if (cursor_ == text_.size() || text_[cursor_] != c) return false;
        ++cursor_;
        return true;
    }
    bool string(std::string* output)
    {
        if (!take('"')) return false;
        while (cursor_ < text_.size())
        {
            unsigned char c = static_cast<unsigned char>(text_[cursor_++]);
            if (c == '"') return true;
            if (c < 0x20U) return false;
            if (c == '\\')
            {
                if (cursor_ == text_.size()) return false;
                c = static_cast<unsigned char>(text_[cursor_++]);
                switch (c)
                {
                case '"': case '\\': case '/': break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u':
                {
                    unsigned int code = 0U;
                    for (unsigned int digit = 0U; digit < 4U; ++digit)
                    {
                        if (cursor_ == text_.size()) return false;
                        const char h = text_[cursor_++];
                        const unsigned int n = h >= '0' && h <= '9' ? static_cast<unsigned int>(h - '0') :
                                               h >= 'a' && h <= 'f' ? static_cast<unsigned int>(h - 'a') + 10U :
                                               h >= 'A' && h <= 'F' ? static_cast<unsigned int>(h - 'A') + 10U : 16U;
                        if (n == 16U) return false;
                        code = code * 16U + n;
                    }
                    // Protocol commands and schema names are ASCII. Other metadata
                    // is skipped without decoding and may contain arbitrary Unicode.
                    if (output != nullptr && code > 0x7FU) return false;
                    c = static_cast<unsigned char>(code);
                    break;
                }
                default: return false;
                }
            }
            if (output != nullptr) output->push_back(static_cast<char>(c));
        }
        return false;
    }
    bool value(unsigned int depth)
    {
        whitespace();
        if (cursor_ == text_.size() || depth > 32U) return false;
        const char first = text_[cursor_];
        if (first == '"') return string(nullptr);
        if (first == '{' || first == '[')
        {
            const char end = first == '{' ? '}' : ']';
            ++cursor_;
            if (take(end)) return true;
            for (;;)
            {
                if (first == '{' && (!string(nullptr) || !take(':'))) return false;
                if (!value(depth + 1U)) return false;
                if (take(end)) return true;
                if (!take(',')) return false;
            }
        }
        for (const std::string_view literal : {"true", "false", "null"})
        {
            if (text_.substr(cursor_, literal.size()) == literal)
            {
                cursor_ += literal.size();
                return true;
            }
        }
        if (first == '-') ++cursor_;
        if (cursor_ == text_.size()) return false;
        if (text_[cursor_] == '0') ++cursor_;
        else
        {
            if (text_[cursor_] < '1' || text_[cursor_] > '9') return false;
            do { ++cursor_; } while (cursor_ < text_.size() && text_[cursor_] >= '0' && text_[cursor_] <= '9');
        }
        if (cursor_ < text_.size() && text_[cursor_] == '.')
        {
            ++cursor_;
            const auto begin = cursor_;
            while (cursor_ < text_.size() && text_[cursor_] >= '0' && text_[cursor_] <= '9') ++cursor_;
            if (cursor_ == begin) return false;
        }
        if (cursor_ < text_.size() && (text_[cursor_] == 'e' || text_[cursor_] == 'E'))
        {
            ++cursor_;
            if (cursor_ < text_.size() && (text_[cursor_] == '+' || text_[cursor_] == '-')) ++cursor_;
            const auto begin = cursor_;
            while (cursor_ < text_.size() && text_[cursor_] >= '0' && text_[cursor_] <= '9') ++cursor_;
            if (cursor_ == begin) return false;
        }
        return true;
    }
    std::string_view text_;
    std::size_t cursor_{};
};

std::string_view find(const JsonReader::Members& members, const char* name)
{
    for (const auto& member : members)
        if (member.first == name) return member.second;
    return {};
}

bool integer(std::string_view token, std::uint64_t& result)
{
    if (!token.empty() && token.front() == '"')
    {
        std::string decoded;
        return JsonReader(token).decoded(decoded) && live_protocol::uint64(decoded, result);
    }
    return live_protocol::uint64(token, result);
}

bool same_string(std::string_view token, const char* expected)
{
    std::string decoded;
    return JsonReader(token).decoded(decoded) && decoded == expected;
}

bool failed(std::string& error, const char* message)
{
    error = message;
    return false;
}

} // namespace

bool read_live_replay(const std::string& path, LiveReplay& replay, std::string& error)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() < 0 || file.tellg() > 1024 * 1024)
        return failed(error, "live replay must be a readable JSON file no larger than 1 MiB");
    file.seekg(0);
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    JsonReader::Members root;
    if (!JsonReader(text).object(root) || !same_string(find(root, "schema"), "wfg-live.replay/1") ||
        !same_string(find(root, "protocol"), live_protocol::version))
        return failed(error, "invalid wfg-live.replay/1 JSON document or protocol");

    LiveReplay parsed;
    if (!integer(find(root, "live_seed"), parsed.seed))
        return failed(error, "live replay requires an exact uint64 live_seed");
    const std::string reference{find(root, "live_reference_rms")};
    char* end = nullptr;
    parsed.reference_rms = std::strtod(reference.c_str(), &end);
    if (end == reference.c_str() || *end != '\0' || !std::isfinite(parsed.reference_rms) || parsed.reference_rms <= 0.0)
        return failed(error, "live replay requires the effective positive live_reference_rms");
    const auto frames = find(root, "live_frames");
    if (!frames.empty())
    {
        parsed.has_frames = true;
        if (!integer(frames, parsed.frames)) return failed(error, "invalid live_frames");
    }
    std::vector<std::string_view> controls;
    if (!JsonReader(find(root, "controls")).array(controls))
        return failed(error, "live replay controls must be an array of at most 128 records");
    signal_lab::LiveController validator;
    for (const auto control : controls)
    {
        JsonReader::Members record;
        std::string command;
        std::uint64_t frame = 0U;
        std::uint64_t expected_events = 0U;
        if (!JsonReader(control).object(record) || !JsonReader(find(record, "command")).decoded(command) ||
            !integer(find(record, "apply_frame"), frame) || !integer(find(record, "events"), expected_events))
            return failed(error, "invalid live replay command, apply_frame, or event count");
        live_protocol::Request request;
        std::uint32_t last_sequence = 0U;
        if (!live_protocol::parse("1 " + command, last_sequence, request) || !live_protocol::is_control(request.kind) || request.scheduled)
            return failed(error, "replay command must be an unscheduled live control; apply_frame supplies its recorded time");
        request.scheduled = true;
        request.frame = frame;
        if (!validator.reset(parsed.seed, parsed.reference_rms)) return failed(error, "live reference RMS is outside the supported range");
        std::uint32_t count = 0U;
        if (live_protocol::apply_control(request, validator, frame, count) != signal_lab::ControlResult::accepted || count != expected_events)
            return failed(error, "live replay control was invalid or its event count disagrees with the recorded acknowledgement");
        if (parsed.events.size() + count > signal_lab::live_capture_capacity)
            return failed(error, "live replay exceeds the 128-event capture capacity");
        parsed.events.insert(parsed.events.end(), validator.capture_data(), validator.capture_data() + validator.capture_count());
    }
    std::stable_sort(parsed.events.begin(), parsed.events.end(), [](const auto& left, const auto& right) { return left.frame < right.frame; });
    std::size_t simultaneous = 0U;
    std::uint64_t previous_frame = 0U;
    for (const auto& event : parsed.events)
    {
        simultaneous = simultaneous != 0U && event.frame == previous_frame ? simultaneous + 1U : 1U;
        if (simultaneous > signal_lab::live_pending_capacity)
            return failed(error, "live replay exceeds the 64-event pending capacity at one output frame");
        previous_frame = event.frame;
    }
    replay = std::move(parsed);
    return true;
}

} // namespace waveform_generator

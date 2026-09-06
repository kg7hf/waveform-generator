#include "common/live_protocol.hpp"

#include <cstring>
#include <limits>

namespace waveform_generator::live_protocol
{
bool uint64(std::string_view text, std::uint64_t& value) noexcept
{
    if (text.empty() || (text.size() > 1 && text.front() == '0')) return false;
    value = 0;
    for (const char c : text)
    {
        if (c < '0' || c > '9') return false;
        const auto digit = static_cast<unsigned>(c - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    return true;
}

bool filename(std::string_view text, std::string_view extension) noexcept
{
    const auto dot = text.find('.');
    if (dot == 0 || dot > 8 || dot == text.npos || text.substr(dot + 1) != extension) return false;
    for (const char c : text.substr(0, dot))
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

namespace
{
bool decimal(std::string_view text, double& value) noexcept
{
    bool negative = false;
    if (!text.empty() && text.front() == '-') { negative = true; text.remove_prefix(1); }
    if (text.empty() || text.size() > 16) return false;
    bool fraction = false;
    bool digit = false;
    double scale = 1.0;
    value = 0;
    for (const char c : text)
    {
        if (c == '.' && !fraction && digit) { fraction = true; digit = false; continue; }
        if (c < '0' || c > '9') return false;
        digit = true;
        if (fraction) { scale *= 0.1; value += static_cast<double>(c - '0') * scale; }
        else { value = value * 10.0 + static_cast<double>(c - '0'); }
    }
    if (!digit) return false;
    if (negative) value = -value;
    return true;
}
bool number32(std::string_view text, std::uint32_t& value) noexcept
{
    std::uint64_t parsed{};
    if (!uint64(text, parsed) || parsed > UINT32_MAX) return false;
    value = static_cast<std::uint32_t>(parsed);
    return true;
}
bool split(std::string_view text, std::string_view* tokens, std::size_t& count, char separator = ' ') noexcept
{
    count = 0;
    while (!text.empty())
    {
        if (count == 8) return false;
        auto end = text.find(separator);
        if (end == text.npos) end = text.size();
        if (end == 0) return false;
        tokens[count++] = text.substr(0, end);
        if (end == text.size()) return true;
        text.remove_prefix(end + 1);
        if (text.empty()) return false;
    }
    return false;
}
}

bool parse(std::string_view text, std::uint32_t& last_sequence, Request& request) noexcept
{
    request = {};
    if (text.size() + 1 > wire_capacity) return false;
    if (!text.empty() && text.back() == '\r') text.remove_suffix(1);
    for (const unsigned char c : text) if (c < 32 || c > 126) return false;
    const auto space = text.find(' ');
    std::uint64_t seq{};
    if (space == text.npos || !uint64(text.substr(0, space), seq) || seq == 0 || seq > UINT32_MAX) return false;
    request.seq = static_cast<std::uint32_t>(seq);
    if (seq <= last_sequence) return false;
    last_sequence = request.seq;
    text.remove_prefix(space + 1);
    if (text.starts_with("AT:"))
    {
        const auto end = text.find(' ');
        if (end == text.npos || !uint64(text.substr(3, end - 3), request.frame)) return false;
        request.scheduled = true;
        text.remove_prefix(end + 1);
    }
    if (!request.scheduled)
    {
        const struct { std::string_view name; Kind kind; } simple[] = {
            {"INFO?", Kind::info}, {"STATUS?", Kind::status}, {"COUNTERS?", Kind::counters},
            {"PLAY", Kind::play}, {"STOP", Kind::stop}, {"MEDIA HOST", Kind::media_host}, {"MEDIA LOCAL", Kind::media_local}};
        for (const auto& item : simple) if (text == item.name) { request.kind = item.kind; return true; }
        if (text.starts_with("PLAY:"))
        {
            const auto id = text.substr(5);
            if (id.size() != 32) return false;
            for (const char c : id) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
            std::memcpy(request.run_id, id.data(), 32);
            request.kind = Kind::play;
            return true;
        }
        if (text.starts_with("LOAD:"))
        {
            text.remove_prefix(5);
            if (!filename(text, "WAV")) return false;
            std::memcpy(request.name, text.data(), text.size());
            request.kind = Kind::load;
            return true;
        }
        if (text.starts_with("SEED:")) { request.kind = Kind::seed; return uint64(text.substr(5), request.integer); }
        if (text.starts_with("GENERATE:"))
        {
            std::string_view fields[8]; std::size_t count{};
            if (!split(text, fields, count, ':') || count != 5 || !number32(fields[1], request.rate)) return false;
            if (request.rate != 75 && request.rate != 150 && request.rate != 300 && request.rate != 600 &&
                request.rate != 1200 && request.rate != 2400 && request.rate != 4800) return false;
            if (fields[2] == "short") request.interleave = 0;
            else if (fields[2] == "long") request.interleave = 1;
            else if (fields[2] == "zero") request.interleave = 2;
            else return false;
            if (!filename(fields[3], "BIN") || !filename(fields[4], "WAV")) return false;
            std::memcpy(request.input_name, fields[3].data(), fields[3].size());
            std::memcpy(request.name, fields[4].data(), fields[4].size());
            request.kind = Kind::generate_file;
            return true;
        }
        if (text.starts_with("REFERENCE:"))
        {
            request.kind = Kind::reference;
            return decimal(text.substr(10), request.value) && request.value > 0 && request.value <= 1;
        }
    }
    std::string_view words[8]; std::size_t count{};
    if (!split(text, words, count)) return false;
    if (!request.scheduled && count == 5 && words[0] == "ENCODE")
    {
        if (words[1].empty() || words[1].size() >= sizeof(request.encoder) || words[2].empty() || words[2].size() >= sizeof(request.profile) ||
            !filename(words[3], "BIN") || !filename(words[4], "WAV")) return false;
        for (const char c : words[1]) if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
        for (const char c : words[2]) if (c == '"' || c == '\\') return false;
        std::memset(request.encoder, 0, sizeof(request.encoder));
        std::memcpy(request.encoder, words[1].data(), words[1].size());
        std::memcpy(request.profile, words[2].data(), words[2].size());
        std::memcpy(request.input_name, words[3].data(), words[3].size());
        std::memcpy(request.name, words[4].data(), words[4].size());
        request.kind = Kind::generate_file;
        return true;
    }
    if (count == 2 && (words[0] == "CW" || words[0] == "STATIC") && (words[1] == "ON" || words[1] == "OFF"))
    {
        request.kind = words[0] == "CW" ? Kind::cw_on : Kind::static_on;
        request.value = words[1] == "ON" ? 1 : 0;
        return true;
    }
    if (count == 3 && decimal(words[2], request.value))
    {
        if (words[0] == "CW" && words[1] == "FREQ") request.kind = Kind::cw_frequency;
        else if (words[0] == "CW" && words[1] == "CI") request.kind = Kind::cw_ci;
        else if (words[0] == "STATIC" && words[1] == "RATE") request.kind = Kind::static_rate;
        else if (words[0] == "STATIC" && words[1] == "PEAK") request.kind = Kind::static_peak;
        else return false;
        return true; // live controller validates physical ranges atomically
    }
    if (count == 4 && words[0] == "FADE" && words[1] == "NOW")
    {
        request.kind = Kind::fade;
        return decimal(words[2], request.value) && number32(words[3], request.duration_ms) && request.duration_ms > 0 && request.duration_ms <= 3600000;
    }
    if (count == 7 && words[0] == "SWEEP")
    {
        std::size_t first = 3;
        if (words[1] == "CW" && words[2] == "FREQ") request.kind = Kind::sweep_frequency;
        else if (words[1] == "CW" && words[2] == "CI") request.kind = Kind::sweep_ci;
        else if (words[1] == "FADE")
        {
            request.kind = Kind::sweep_fade; first = 2;
            if (!number32(words[6], request.duration_ms) || request.duration_ms == 0 || request.duration_ms > 3600000) return false;
        }
        else return false;
        return decimal(words[first], request.value) && decimal(words[first + 1], request.end_value) &&
            number32(words[first + 2], request.steps) && request.steps >= 2 && request.steps <= 16 &&
            number32(words[first + 3], request.interval_ms) && request.interval_ms > 0 && request.interval_ms <= 3600000;
    }
    return false;
}
} // namespace waveform_generator::live_protocol

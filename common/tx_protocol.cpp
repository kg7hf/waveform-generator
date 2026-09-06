#include "common/tx_protocol.hpp"

#include <cstring>
#include <string_view>

namespace waveform_generator::tx_protocol
{
namespace
{
struct Mode { const char* token; std::uint16_t rate; std::uint8_t interleave; };
constexpr Mode modes[] = {
    {"75S",75,0}, {"75L",75,1}, {"150S",150,0}, {"150L",150,1},
    {"300S",300,0}, {"300L",300,1}, {"600S",600,0}, {"600L",600,1},
    {"1200S",1200,0}, {"1200L",1200,1}, {"2400S",2400,0},
    {"2400L",2400,1}, {"4800U",4800,2}
};
bool copy_name(std::string_view value, char (&output)[13], const char* extension) noexcept
{
    if (value.size() > 12U)
    {
        return false;
    }
    char candidate[13]{};
    std::memcpy(candidate, value.data(), value.size());
    if (!valid_filename(candidate, extension))
    {
        return false;
    }
    std::memcpy(output, candidate, sizeof(candidate));
    return true;
}
}

bool valid_filename(const char* text, const char* extension) noexcept
{
    if (text == nullptr || extension == nullptr)
    {
        return false;
    }
    std::size_t stem{};
    while (stem < 9U && text[stem] != '\0' && text[stem] != '.')
    {
        const char ch = text[stem];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_'))
        {
            return false;
        }
        ++stem;
    }
    return stem >= 1U && stem <= 8U && text[stem] == '.' &&
        std::strcmp(text + stem + 1U, extension) == 0;
}

const char* mode_token(std::uint16_t rate, std::uint8_t interleave) noexcept
{
    for (const auto& mode : modes)
    {
        if (mode.rate == rate && mode.interleave == interleave)
        {
            return mode.token;
        }
    }
    return nullptr;
}

void CommandParser::reset() noexcept
{
    size_ = 0U;
    text_[0] = '\0';
    request_ = {};
    ready_ = valid_ = mode_query_ = overflow_ = false;
}

void CommandParser::consume() noexcept
{
    reset();
}

void CommandParser::feed(std::uint8_t byte) noexcept
{
    if (ready_)
    {
        return;
    }
    if (byte == '\r' || byte == '\n')
    {
        if (size_ != 0U || overflow_)
        {
            parse(true);
        }
        return;
    }
    if (overflow_)
    {
        return;
    }
    if (size_ == sizeof(text_) - 1U || byte < 32U || byte > 126U)
    {
        overflow_ = true;
        return;
    }
    text_[size_++] = static_cast<char>(byte);
    text_[size_] = '\0';
    parse(false);
}

void CommandParser::parse(bool terminated) noexcept
{
    if (overflow_)
    {
        ready_ = terminated;
        valid_ = false;
        return;
    }
    const std::string_view command{text_, size_};
    constexpr std::string_view info{"CMD:MODEM INFO:?"};
    constexpr std::string_view send{"CMD:SENDBUFFER"};
    constexpr std::string_view reset_command{"CMD:RESET MDM"};
    constexpr std::string_view rate{"CMD:DATA RATE:"};
    constexpr std::string_view select{"CMD:WAV FILE:"};
    constexpr std::string_view generate{"CMD:TX FILE:"};
    constexpr std::string_view status{"CMD:STATUS:?"};
    if (command == info)
    {
        request_.kind = Kind::info;
        ready_ = valid_ = true;
    }
    else if (command == send)
    {
        request_.kind = Kind::send;
        ready_ = valid_ = true;
    }
    else if (command == reset_command)
    {
        request_.kind = Kind::reset;
        ready_ = valid_ = true;
    }
    else if (command.starts_with(rate))
    {
        const auto token = command.substr(rate.size());
        if (token == "?")
        {
            request_.kind = Kind::query;
            mode_query_ = ready_ = valid_ = true;
        }
        else
        {
            for (const auto& mode : modes)
            {
                if (token == mode.token)
                {
                    request_.kind = Kind::mode;
                    request_.rate = mode.rate;
                    request_.interleave = mode.interleave;
                    ready_ = valid_ = true;
                    break;
                }
            }
        }
    }
    else if (terminated && command.starts_with(select))
    {
        request_.kind = Kind::select;
        valid_ = copy_name(command.substr(select.size()), request_.name, "WAV");
        ready_ = true;
    }
    else if (terminated && command.starts_with(generate))
    {
        const auto names = command.substr(generate.size());
        const auto colon = names.find(':');
        request_.kind = Kind::generate_file;
        valid_ = colon != std::string_view::npos &&
            copy_name(names.substr(0U, colon), request_.input_name, "BIN") &&
            copy_name(names.substr(colon + 1U), request_.name, "WAV");
        ready_ = true;
    }
    else if (terminated && command == status)
    {
        request_.kind = Kind::query;
        ready_ = valid_ = true;
    }
    if (terminated && !ready_)
    {
        ready_ = true;
        valid_ = false;
    }
}
}

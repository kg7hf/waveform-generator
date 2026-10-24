// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "cli/options.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <string>
#include <system_error>

namespace channel_sim::cli
{

namespace
{
using m110::Status;
using m110::StatusCode;

enum class Field : std::uint8_t
{
    help,
    input,
    output,
    truth,
    seed,
    snr,
    spread,
    offset,
    delay,
    chain,
    resample_gain
};

struct OptionInfo
{
    std::string_view name;
    Field field;
    std::size_t slot;
    bool takes_value;
};

constexpr std::array option_table{
    OptionInfo{"help", Field::help, 0U, false},         OptionInfo{"input_file", Field::input, 0U, true},
    OptionInfo{"output_file", Field::output, 0U, true}, OptionInfo{"truth-out", Field::truth, 0U, true},
    OptionInfo{"seed", Field::seed, 0U, true},          OptionInfo{"snr", Field::snr, 0U, true},
    OptionInfo{"spread", Field::spread, 0U, true},      OptionInfo{"offset", Field::offset, 0U, true},
    OptionInfo{"delay2", Field::delay, 1U, true},       OptionInfo{"spread2", Field::spread, 1U, true},
    OptionInfo{"offset2", Field::offset, 1U, true},     OptionInfo{"delay3", Field::delay, 2U, true},
    OptionInfo{"spread3", Field::spread, 2U, true},     OptionInfo{"offset3", Field::offset, 2U, true},
    OptionInfo{"chain", Field::chain, 0U, true},        OptionInfo{"resample-gain-db", Field::resample_gain, 0U, true},
};

// The lane resampler's accepted gain range (parse_gain).
constexpr double lowest_resample_gain_db = -120.0;
constexpr double highest_resample_gain_db = 24.0;

[[nodiscard]] const OptionInfo* find_option(std::string_view name) noexcept
{
    const auto* const found = std::ranges::find(option_table, name, &OptionInfo::name);
    return found == option_table.end() ? nullptr : found;
}

[[nodiscard]] Status duplicate() noexcept
{
    return {StatusCode::invalid_argument, "option given twice"};
}

// A finite decimal number; the whole text must parse (std::from_chars, correctly rounded).
[[nodiscard]] m110::Result<double> parse_number(std::string_view text) noexcept
{
    double value = 0.0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);

    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value))
    {
        return Status{StatusCode::invalid_argument, "not a finite number"};
    }

    return value;
}

[[nodiscard]] m110::Result<std::uint32_t> parse_seed(std::string_view text) noexcept
{
    std::uint32_t value = 0U;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);

    if (error != std::errc{} || end != text.data() + text.size() || text.empty())
    {
        return Status{StatusCode::invalid_argument, "seed must be a decimal integer 0..4294967295"};
    }

    return value;
}

[[nodiscard]] m110::Result<ChainMode> parse_mode(std::string_view text) noexcept
{
    if (text == "auto")
    {
        return ChainMode::automatic;
    }

    if (text == "8k")
    {
        return ChainMode::pathsim_8k;
    }

    if (text == "48k")
    {
        return ChainMode::lane_48k;
    }

    return Status{StatusCode::invalid_argument, "chain must be auto, 8k or 48k"};
}

// The NUL-terminated tail of an argument from offset on; the argument's own
// terminator ends it.
[[nodiscard]] const char* argument_tail(const char* argument, std::size_t offset) noexcept
{
    const std::span<const char> whole{argument, std::char_traits<char>::length(argument) + 1U};
    return whole.subspan(offset).data();
}

[[nodiscard]] Status set_file(const char*& target, const char* value) noexcept
{
    if (target != nullptr)
    {
        return duplicate();
    }

    if (std::string_view{value}.empty())
    {
        return {StatusCode::invalid_argument, "empty file name"};
    }

    target = value;
    return Status::success();
}

[[nodiscard]] Status set_number(std::optional<double>& target, std::string_view value) noexcept
{
    if (target.has_value())
    {
        return duplicate();
    }

    const m110::Result<double> number = parse_number(value);

    if (!number)
    {
        return number.status();
    }

    target = number.value();
    return Status::success();
}

[[nodiscard]] Status set_seed(std::optional<std::uint32_t>& target, std::string_view value) noexcept
{
    if (target.has_value())
    {
        return duplicate();
    }

    const m110::Result<std::uint32_t> seed = parse_seed(value);

    if (!seed)
    {
        return seed.status();
    }

    target = seed.value();
    return Status::success();
}

[[nodiscard]] Status set_resample_gain(std::optional<double>& target, std::string_view value) noexcept
{
    if (target.has_value())
    {
        return duplicate();
    }

    const m110::Result<double> gain = parse_number(value);

    if (!gain)
    {
        return gain.status();
    }

    if (gain.value() < lowest_resample_gain_db || gain.value() > highest_resample_gain_db)
    {
        return {StatusCode::invalid_argument, "resample gain must be -120..24 dB"};
    }

    target = gain.value();
    return Status::success();
}

[[nodiscard]] Status set_mode(std::optional<ChainMode>& target, std::string_view value) noexcept
{
    if (target.has_value())
    {
        return duplicate();
    }

    const m110::Result<ChainMode> mode = parse_mode(value);

    if (!mode)
    {
        return mode.status();
    }

    target = mode.value();
    return Status::success();
}

[[nodiscard]] Status apply_path(ChannelSpec& channel, const CliOptions& options, std::size_t slot) noexcept
{
    const std::optional<double>& spread = std::span{options.spread}[slot];
    const std::optional<double>& offset = std::span{options.offset}[slot];
    const std::optional<double>& delay = std::span{options.delay}[slot];
    Status status = Status::success();

    if (spread.has_value())
    {
        status = channel.set_spread(slot, Hertz{*spread});
    }

    if (status.is_ok() && offset.has_value())
    {
        status = channel.set_offset(slot, Hertz{*offset});
    }

    if (status.is_ok() && delay.has_value())
    {
        status = channel.set_delay(slot, Milliseconds{*delay});
    }

    return status;
}
} // namespace

Status CliParser::parse(std::span<const char* const> arguments) noexcept
{
    options_ = CliOptions{};
    failed_ = {};

    for (std::size_t index = 0U; index < arguments.size(); ++index)
    {
        const std::string_view argument{arguments[index]};
        Status status = Status::success();

        if (argument.starts_with("--") && argument.size() > 2U)
        {
            status = parse_long(arguments, index);
        }
        else if (argument == "-i" || argument == "-o")
        {
            if (index + 1U >= arguments.size())
            {
                failed_ = argument;
                return {StatusCode::invalid_argument, "option needs a value"};
            }

            ++index;
            status = apply(argument == "-i" ? "input_file" : "output_file", arguments[index]);
        }
        else if (argument.size() > 1U && argument.front() == '-')
        {
            status = {StatusCode::invalid_argument, "unknown option"};
        }
        else
        {
            status = positional(arguments[index]);
        }

        if (!status.is_ok())
        {
            if (failed_.empty())
            {
                failed_ = argument;
            }

            return status;
        }
    }

    return Status::success();
}

const CliOptions& CliParser::options() const noexcept
{
    return options_;
}

std::string_view CliParser::failed_argument() const noexcept
{
    return failed_;
}

Status CliParser::parse_long(std::span<const char* const> arguments, std::size_t& index) noexcept
{
    const std::string_view body = std::string_view{arguments[index]}.substr(2U);
    const std::size_t equals = body.find('=');
    const bool inline_value = equals != std::string_view::npos;
    const std::string_view name = body.substr(0U, equals);

    if (is_preset(name))
    {
        if (inline_value)
        {
            return {StatusCode::invalid_argument, "a propagation condition takes no value"};
        }

        if (!options_.preset.empty())
        {
            return {StatusCode::invalid_argument, "use just one propagation condition"};
        }

        options_.preset = name;
        return Status::success();
    }

    const OptionInfo* const info = find_option(name);

    if (info == nullptr)
    {
        return {StatusCode::invalid_argument, "unknown option"};
    }

    if (!info->takes_value)
    {
        options_.help = true;
        return inline_value ? Status{StatusCode::invalid_argument, "option takes no value"} : Status::success();
    }

    if (inline_value)
    {
        // "--" + name + "=": the value is the rest of the argument.
        return apply(name, argument_tail(arguments[index], 2U + equals + 1U));
    }

    if (index + 1U >= arguments.size())
    {
        return {StatusCode::invalid_argument, "option needs a value"};
    }

    ++index;
    return apply(name, arguments[index]);
}

Status CliParser::apply(std::string_view name, const char* value) noexcept
{
    const OptionInfo* const info = find_option(name);

    if (info == nullptr)
    {
        return {StatusCode::internal_error, "option table mismatch"};
    }

    switch (info->field)
    {
    case Field::input:
        return set_file(options_.input, value);

    case Field::output:
        return set_file(options_.output, value);

    case Field::truth:
        return set_file(options_.truth, value);

    case Field::seed:
        return set_seed(options_.seed, value);

    case Field::snr:
        return set_number(options_.snr, value);

    case Field::spread:
        return set_number(std::span{options_.spread}[info->slot], value);

    case Field::offset:
        return set_number(std::span{options_.offset}[info->slot], value);

    case Field::delay:
        return set_number(std::span{options_.delay}[info->slot], value);

    case Field::chain:
        return set_mode(options_.mode, value);

    case Field::resample_gain:
        return set_resample_gain(options_.resample_gain_db, value);

    case Field::help:
        break;
    }

    return {StatusCode::internal_error, "option table mismatch"};
}

Status CliParser::positional(const char* value) noexcept
{
    if (options_.input == nullptr)
    {
        options_.input = value;
        return Status::success();
    }

    if (options_.output == nullptr)
    {
        options_.output = value;
        return Status::success();
    }

    return {StatusCode::invalid_argument, "more than two file names"};
}

m110::Result<PathsimJob> build_job(const CliOptions& options) noexcept
{
    PathsimJob job{};

    if (!options.preset.empty())
    {
        const Status preset = job.channel.apply_preset(options.preset);

        if (!preset.is_ok())
        {
            return preset;
        }
    }

    if (options.snr.has_value())
    {
        job.channel.set_snr(Decibels{*options.snr});
    }

    for (std::size_t slot = 0U; slot < max_paths; ++slot)
    {
        const Status path = apply_path(job.channel, options, slot);

        if (!path.is_ok())
        {
            return path;
        }
    }

    const Status valid = job.channel.validate();

    if (!valid.is_ok())
    {
        return valid;
    }

    job.seed = Seed{options.seed.value_or(1U)};
    job.truth_path = options.truth != nullptr ? std::string_view{options.truth} : std::string_view{};
    return job;
}

} // namespace channel_sim::cli

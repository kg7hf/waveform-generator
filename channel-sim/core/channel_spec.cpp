// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "core/channel_spec.hpp"

#include <Delay.h>
#include <PathSimParams.h>

#include <cmath>
#include <utility>

namespace channel_sim
{

static_assert(std::cmp_equal(pathsim_delay_ring_samples, PathSim::Delay::BUFSIZE), "the delay limit follows pathsim's delay line");

namespace
{
using m110::Status;
using m110::StatusCode;

// pathsim's preset table (PathSimParams.cpp), by command-line name.
[[nodiscard]] const PathSim::PathSimParams* find_preset(std::string_view name) noexcept
{
    for (const PathSim::PathSimParams& params : PathSim::default_params())
    {
        if (std::string_view{params.cmdline_param} == name)
        {
            return &params;
        }
    }

    return nullptr;
}

[[nodiscard]] bool within(double value, double lowest, double highest) noexcept
{
    return std::isfinite(value) && value >= lowest && value <= highest;
}

// pathsim lags the path by int(8 * ms) samples in its delay ring (Delay::add_delay);
// 8 * ms is exact in double, so this is 0 <= lag < ring length.
[[nodiscard]] bool valid_path_delay(Milliseconds delay) noexcept
{
    return std::isfinite(delay.value) && delay.value >= 0.0 && delay.value < path_delay_limit.value;
}

[[nodiscard]] Status check_path(const PathSpec& path, bool first) noexcept
{
    if (!within(path.spread.value, 0.0, max_spread.value))
    {
        return {StatusCode::invalid_argument, "path spread must be 0..30 Hz"};
    }

    if (!within(path.offset.value, -max_offset.value, max_offset.value))
    {
        return {StatusCode::invalid_argument, "path offset must be -4000..4000 Hz"};
    }

    // Path 1 is the undelayed reference: pathsim never delays it, and no option sets it.
    if (first ? path.delay.value != 0.0 : !valid_path_delay(path.delay))
    {
        return {StatusCode::invalid_argument, "path delay must be at least 0 and below 306 ms (path 1: 0)"};
    }

    return Status::success();
}
} // namespace

Status ChannelSpec::apply_preset(std::string_view name) noexcept
{
    if (!preset_.empty())
    {
        return {StatusCode::invalid_argument, "use just one propagation condition (preset)"};
    }

    if (modified_)
    {
        return {StatusCode::invalid_configuration, "a preset must be applied before other channel settings"};
    }

    const PathSim::PathSimParams* const preset = find_preset(name);

    if (preset == nullptr)
    {
        return {StatusCode::invalid_argument, "unknown propagation condition (preset)"};
    }

    if (preset->paths.size() > max_paths)
    {
        return {StatusCode::invalid_configuration, "preset has more paths than the chain supports"};
    }

    path_count_ = 0U;

    for (const PathSim::PathParams& path : preset->paths)
    {
        slot_ref(path_count_) = PathSpec{Milliseconds{path.delay}, Hertz{path.spread}, Hertz{path.offset}};
        ++path_count_;
    }

    noise_ = NoiseSpec{preset->noise.has_awgn, Decibels{preset->noise.snr}};
    preset_ = std::string_view{preset->cmdline_param};
    return Status::success();
}

void ChannelSpec::set_snr(Decibels snr) noexcept
{
    noise_ = NoiseSpec{true, snr};
    modified_ = true;
}

Status ChannelSpec::set_delay(std::size_t slot, Milliseconds delay) noexcept
{
    if (slot == 0U)
    {
        return {StatusCode::invalid_argument, "path 1 has no delay option"};
    }

    const Status reached = reach(slot);

    if (reached.is_ok())
    {
        slot_ref(slot).delay = delay;
    }

    return reached;
}

Status ChannelSpec::set_spread(std::size_t slot, Hertz spread) noexcept
{
    const Status reached = reach(slot);

    if (reached.is_ok())
    {
        slot_ref(slot).spread = spread;
    }

    return reached;
}

Status ChannelSpec::set_offset(std::size_t slot, Hertz offset) noexcept
{
    const Status reached = reach(slot);

    if (reached.is_ok())
    {
        slot_ref(slot).offset = offset;
    }

    return reached;
}

Status ChannelSpec::validate() const noexcept
{
    bool first = true;

    for (const PathSpec& path : paths())
    {
        const Status checked = check_path(path, first);

        if (!checked.is_ok())
        {
            return checked;
        }

        first = false;
    }

    if (noise_.enabled && !within(noise_.snr.value, -max_snr_magnitude.value, max_snr_magnitude.value))
    {
        return {StatusCode::invalid_argument, "SNR must be finite and within +/-150 dB"};
    }

    return Status::success();
}

std::span<const PathSpec> ChannelSpec::paths() const noexcept
{
    return std::span<const PathSpec>{paths_}.first(path_count_);
}

const NoiseSpec& ChannelSpec::noise() const noexcept
{
    return noise_;
}

std::string_view ChannelSpec::preset() const noexcept
{
    return preset_;
}

Status ChannelSpec::reach(std::size_t slot) noexcept
{
    if (slot >= max_paths)
    {
        return {StatusCode::invalid_argument, "path slot out of range (paths 1..3)"};
    }

    while (path_count_ <= slot)
    {
        slot_ref(path_count_) = PathSpec{};
        ++path_count_;
    }

    modified_ = true;
    return Status::success();
}

PathSpec& ChannelSpec::slot_ref(std::size_t slot) noexcept
{
    return std::span<PathSpec>{paths_}[slot];
}

std::size_t preset_count() noexcept
{
    return PathSim::default_params().size();
}

std::string_view preset_name(std::size_t index) noexcept
{
    const auto& table = PathSim::default_params();
    return index < table.size() ? std::string_view{table[index].cmdline_param} : std::string_view{};
}

std::string_view preset_title(std::size_t index) noexcept
{
    const auto& table = PathSim::default_params();
    return index < table.size() ? std::string_view{table[index].title} : std::string_view{};
}

bool is_preset(std::string_view name) noexcept
{
    return find_preset(name) != nullptr;
}

} // namespace channel_sim

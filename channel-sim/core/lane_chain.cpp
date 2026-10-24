// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "core/lane_chain.hpp"

#include <cmath>

namespace channel_sim
{

namespace
{
constexpr double decibels_per_decade = 20.0;
constexpr double decade = 10.0;
// The upsampling step passes no gain argument: the tool's gain_db is 0.
constexpr Decibels upsample_gain{0.0};
} // namespace

double amplitude_from_db(Decibels gain) noexcept
{
    // The tool: std::pow(10.0, gain_db / 20.0), evaluated at run time.
    return std::pow(decade, gain.value / decibels_per_decade);
}

m110::Result<LaneChainReport> LaneChain::run(const PathsimJob& job, const LaneChainConfig& config, UnitSource& source, Pcm16Sink& sink) noexcept
{
    const m110::Status down = downsampler_.attach(source, lane_sample_rate, pathsim_sample_rate, amplitude_from_db(config.downsample_gain));

    if (!down.is_ok())
    {
        return down;
    }

    const m110::Status up = upsampler_.attach(sink, pathsim_sample_rate, lane_sample_rate, amplitude_from_db(upsample_gain));

    if (!up.is_ok())
    {
        return up;
    }

    const m110::Result<PathsimRunReport> pathsim = driver_.run(job, downsampler_, upsampler_);

    if (!pathsim)
    {
        return pathsim.status();
    }

    const m110::Status finished = upsampler_.finish();

    if (!finished.is_ok())
    {
        return finished;
    }

    LaneChainReport report{};
    report.pathsim = pathsim.value();
    report.input_samples = downsampler_.resampler().inputs();
    report.output_samples = upsampler_.resampler().outputs();
    report.downsample_kernel_misses = downsampler_.resampler().kernel_misses();
    report.upsample_kernel_misses = upsampler_.resampler().kernel_misses();
    return report;
}

} // namespace channel_sim

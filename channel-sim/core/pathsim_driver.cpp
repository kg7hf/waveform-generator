// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "core/pathsim_driver.hpp"

#include "core/convert.hpp"
#include "rng/pathsim_rng.hpp"

#include <PathSimParams.h>

#include <algorithm>
#include <cmath>
#include <span>
#include <string>

namespace channel_sim
{

namespace
{
using m110::Status;
using m110::StatusCode;

// pathsim's parameter set for the channel (allocates on the host heap: pathsim's types).
[[nodiscard]] PathSim::PathSimParams to_pathsim_params(const ChannelSpec& channel)
{
    PathSim::PathSimParams params;

    for (const PathSpec& path : channel.paths())
    {
        params.paths.push_back(PathSim::PathParams{path.delay.value, path.spread.value, path.offset.value});
    }

    params.noise = PathSim::NoiseParams{channel.noise().enabled, channel.noise().snr.value};
    return params;
}
} // namespace

m110::Result<PathsimRunReport> PathsimDriver::run(const PathsimJob& job, Pcm16Source& source, Pcm16Sink& sink) noexcept
{
    const Status valid = job.channel.validate();

    if (!valid.is_ok())
    {
        return valid;
    }

    const m110::Result<ActiveLevel> level = measure_active_level(source);

    if (!level)
    {
        return level.status();
    }

    const Status rewound = source.rewind();

    if (!rewound.is_ok())
    {
        return rewound;
    }

    PathSim::PathSimProcessor& processor = start_processor(job, level.value());
    const m110::Result<std::uint64_t> blocks = process_blocks(processor, source, sink, level.value().samples);
    // Destroying the processor closes a truth file.
    processor_.reset();

    if (!blocks)
    {
        return blocks.status();
    }

    PathsimRunReport report{};
    report.samples = level.value().samples;
    report.active_samples = level.value().active;
    report.seed_rms = level.value().active > 0U ? std::sqrt(level.value().sum_squares / as_double(level.value().active)) : 0.0;
    report.blocks = blocks.value();
    return report;
}

m110::Result<PathsimDriver::ActiveLevel> PathsimDriver::measure_active_level(Pcm16Source& source) noexcept
{
    ActiveLevel level{};

    for (;;)
    {
        const m110::Result<std::size_t> got = source.read(pcm_);

        if (!got)
        {
            return got.status();
        }

        if (got.value() == 0U)
        {
            return level;
        }

        if (got.value() > pcm_.size())
        {
            return Status{StatusCode::internal_error, "source returned more samples than requested"};
        }

        // main.cpp: acc += v * v over |v| > 1e-4, in stream order.
        for (const Pcm16 sample : std::span<const Pcm16>{pcm_}.first(got.value()))
        {
            const double value = pcm16_to_unit(sample);

            if (std::fabs(value) > active_threshold)
            {
                level.sum_squares += value * value;
                ++level.active;
            }
        }

        level.samples += got.value();
    }
}

m110::Result<std::size_t> PathsimDriver::fill_block(Pcm16Source& source) noexcept
{
    std::size_t filled = 0U;

    while (filled < block_samples)
    {
        const m110::Result<std::size_t> got = source.read(std::span<Pcm16>{pcm_}.subspan(filled));

        if (!got)
        {
            return got.status();
        }

        if (got.value() == 0U)
        {
            break;
        }

        if (got.value() > block_samples - filled)
        {
            return Status{StatusCode::internal_error, "source returned more samples than requested"};
        }

        filled += got.value();
    }

    return filled;
}

PathSim::PathSimProcessor& PathsimDriver::start_processor(const PathsimJob& job, const ActiveLevel& level) noexcept
{
    // main.cpp: srand(seed) comes before init(), whose Rayleigh preload draws first.
    seed_pathsim_rng(job.seed);
    processor_.reset();
    PathSim::PathSimProcessor& processor = processor_.emplace();
    processor.init(to_pathsim_params(job.channel));

    if (!job.truth_path.empty())
    {
        processor.enable_truth_capture(std::string{job.truth_path}, job.seed.value);
    }

    if (level.active > 0U)
    {
        processor.seed_signal_rms(std::sqrt(level.sum_squares / as_double(level.active)));
    }

    processor.warm_up_fading(fading_warm_up.value);
    return processor;
}

m110::Result<std::uint64_t> PathsimDriver::process_blocks(PathSim::PathSimProcessor& processor, Pcm16Source& source, Pcm16Sink& sink, std::uint64_t expected) noexcept
{
    std::uint64_t samples = 0U;
    std::uint64_t blocks = 0U;

    for (;;)
    {
        const m110::Result<std::size_t> filled = fill_block(source);

        if (!filled)
        {
            return filled.status();
        }

        const std::size_t count = filled.value();

        if (count == 0U)
        {
            break;
        }

        // main.cpp pads the last block with zeros and keeps only the input's length.
        std::ranges::transform(std::span<const Pcm16>{pcm_}.first(count), block_.begin(), pcm16_to_unit);
        std::ranges::fill(std::span<double>{block_}.subspan(count), 0.0);
        processor.process_buffer(block_.data());
        std::ranges::transform(std::span<const double>{block_}.first(count), pcm_.begin(), unit_to_pcm16_truncated);
        const Status written = sink.write(std::span<const Pcm16>{pcm_}.first(count));

        if (!written.is_ok())
        {
            return written;
        }

        samples += count;
        ++blocks;

        if (count < block_samples)
        {
            break;
        }
    }

    if (samples != expected)
    {
        return Status{StatusCode::io_error, "the source delivered a different length on its second pass"};
    }

    return blocks;
}

} // namespace channel_sim

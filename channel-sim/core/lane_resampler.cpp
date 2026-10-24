// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "core/lane_resampler.hpp"

#include "core/convert.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <span>

namespace channel_sim
{

namespace
{
using m110::Status;
using m110::StatusCode;

// The lane tool's constants (wav_resample.cpp). Its pi literal, 3.1415926535897932384626433832795,
// is the same double as std::numbers::pi.
constexpr double pi = std::numbers::pi;
constexpr double two_pi = 2.0 * pi; // exact: the tool's 2.0 * pi * u
constexpr double cutoff_fraction = 0.45;
constexpr double sinc_bandwidth_factor = 2.0; // the tool's 2.0 * cutoff
constexpr double blackman_a0 = 0.42;
constexpr double blackman_a1 = 0.5;
constexpr double blackman_a2 = 0.08;
constexpr double sinc_zero = 1.0e-12;
constexpr double weight_sum_floor = 1.0e-12;
constexpr double radius = as_double(lane_filter_radius);
// Tap t of the window is input c + t, t = -95 .. 96.
constexpr std::int64_t last_tap = to_signed_index(lane_filter_radius);
constexpr std::int64_t first_tap = 1 - last_tap;
constexpr std::uint64_t history_mask = LaneResampler::history_capacity - 1U;
// Inputs are limited so that the output count (at most 24 x the input count)
// stays within convert.hpp's exact, unsaturated range: about 3e15 inputs.
constexpr std::uint64_t max_rate_ratio = lane_highest_rate.hz / lane_lowest_rate.hz;
constexpr std::uint64_t max_inputs = max_sample_count / max_rate_ratio;

[[nodiscard]] double sinc(double value) noexcept
{
    return std::abs(value) < sinc_zero ? 1.0 : std::sin(pi * value) / (pi * value);
}

[[nodiscard]] bool lane_rate(SampleRate rate) noexcept
{
    return rate.hz >= lane_lowest_rate.hz && rate.hz <= lane_highest_rate.hz;
}
} // namespace

Status LaneResampler::configure(SampleRate input, SampleRate output, double gain) noexcept
{
    if (!lane_rate(input) || !lane_rate(output) || !std::isfinite(gain))
    {
        configured_ = false;
        return {StatusCode::invalid_argument, "resampler rates must be 8000..192000 Hz with a finite gain"};
    }

    input_rate_ = input;
    output_rate_ = output;
    // The tool: input_per_output = double(in) / out; cutoff = 0.45 * min(1.0, double(out) / in).
    input_per_output_ = as_double(input) / as_double(output);
    const double cutoff = cutoff_fraction * std::min(1.0, as_double(output) / as_double(input));
    two_cutoff_ = sinc_bandwidth_factor * cutoff;
    gain_ = gain;
    configured_ = true;
    kernels_used_ = 0U;
    next_victim_ = 0U;
    hint_ = 0U;
    kernel_misses_ = 0U;
    restart();
    return Status::success();
}

void LaneResampler::restart() noexcept
{
    history_.fill(0.0);
    pushed_ = 0U;
    produced_ = 0U;
    output_count_ = 0U;
    finished_ = false;
    locate_next();
}

Status LaneResampler::push(double sample) noexcept
{
    if (!configured_ || finished_)
    {
        return {StatusCode::invalid_configuration, "resampler is not accepting input"};
    }

    // Taking every ready output first keeps the oldest tap still needed
    // (center - 95 >= pushed - 191) clear of the slot this sample overwrites.
    if (ready())
    {
        return {StatusCode::busy, "resampler has a ready output; take it first"};
    }

    // The interior fast path multiplies every tap, including those of weight +0.0
    // that the lane tool skips; 0 * inf would be NaN where the tool has a value.
    // Finite input keeps the two bit-identical, and a WAV with inf or NaN is not audio.
    if (!std::isfinite(sample))
    {
        return {StatusCode::invalid_argument, "resampler input must be finite (no inf or NaN samples)"};
    }

    if (pushed_ >= max_inputs)
    {
        return {StatusCode::invalid_argument, "resampler input exceeds 2^56 / 24 samples"};
    }

    std::span<double>{history_}[pushed_ & history_mask] = sample;
    ++pushed_;
    return Status::success();
}

void LaneResampler::finish() noexcept
{
    if (finished_)
    {
        return;
    }

    finished_ = true;
    // The tool: llround(double(N) * out / in).
    output_count_ = to_unsigned_index(std::llround(as_double(pushed_) * as_double(output_rate_) / as_double(input_rate_)));
}

bool LaneResampler::ready() const noexcept
{
    if (!configured_)
    {
        return false;
    }

    if (finished_)
    {
        return produced_ < output_count_;
    }

    // Before the end, an output is ready once its last tap has arrived. With
    // out/in >= 1/24 such an output is always below the final count.
    return center_ + last_tap < to_signed_index(pushed_);
}

bool LaneResampler::done() const noexcept
{
    return finished_ && produced_ >= output_count_;
}

m110::Result<Pcm16> LaneResampler::next() noexcept
{
    if (!ready())
    {
        return Status{StatusCode::invalid_configuration, "no resampler output is ready"};
    }

    // f = p - c is exact (Sterbenz for c >= 1; p itself for c = 0).
    const double frac = position_ - as_double(center_);
    const Kernel& kernel = kernel_for(frac);
    const std::int64_t first = center_ + first_tap;
    const std::int64_t last = center_ + last_tap;
    const bool interior = first >= 0 && last < to_signed_index(pushed_);
    const double value = interior ? interior_value(kernel, to_unsigned_index(first)) : edge_value(kernel, first);
    ++produced_;
    locate_next();
    return unit_to_pcm16_rounded(value * gain_);
}

std::uint64_t LaneResampler::inputs() const noexcept
{
    return pushed_;
}

std::uint64_t LaneResampler::outputs() const noexcept
{
    return produced_;
}

std::uint64_t LaneResampler::kernel_misses() const noexcept
{
    return kernel_misses_;
}

const LaneResampler::Kernel& LaneResampler::kernel_for(double frac) noexcept
{
    const std::span<Kernel> kernels{kernels_};

    for (std::size_t probe = 0U; probe < kernels_used_; ++probe)
    {
        const std::size_t index = (hint_ + probe) % kernels_used_;

        // The key is the exact double: the weights are a function of its bits.
        if (kernels[index].frac == frac)
        {
            hint_ = index + 1U;
            return kernels[index];
        }
    }

    std::size_t index = kernels_used_;

    if (kernels_used_ < kernel_cache_entries)
    {
        ++kernels_used_;
    }
    else
    {
        index = next_victim_;
        next_victim_ = (next_victim_ + 1U) % kernel_cache_entries;
    }

    ++kernel_misses_;
    compute_kernel(kernels[index], frac);
    hint_ = index + 1U;
    return kernels[index];
}

void LaneResampler::compute_kernel(Kernel& kernel, double frac) const noexcept
{
    const std::span<double> weights{kernel.weights};
    double sum = 0.0;

    for (std::size_t index = 0U; index < lane_filter_taps; ++index)
    {
        const double tap = as_double(to_signed_index(index) + first_tap);
        const double distance = frac - tap;
        const double normalized = distance / radius;
        double weight = 0.0;

        if (std::abs(normalized) < 1.0)
        {
            const double window = blackman_a0 + blackman_a1 * std::cos(pi * normalized) + blackman_a2 * std::cos(two_pi * normalized);
            weight = two_cutoff_ * sinc(two_cutoff_ * distance) * window;
        }

        weights[index] = weight;
        sum += weight;
    }

    kernel.frac = frac;
    kernel.weight_sum = sum;
}

void LaneResampler::locate_next() noexcept
{
    // The tool: p = double(i) * input_per_output; c = int64(floor(p)).
    position_ = as_double(produced_) * input_per_output_;
    center_ = floor_to_index(position_);
}

double LaneResampler::interior_value(const Kernel& kernel, std::uint64_t first) const noexcept
{
    const std::span<const double> history{history_};
    const std::span<const double> weights{kernel.weights};
    double weighted_sum = 0.0;

    for (std::size_t index = 0U; index < lane_filter_taps; ++index)
    {
        weighted_sum += history[(first + index) & history_mask] * weights[index];
    }

    return std::abs(kernel.weight_sum) > weight_sum_floor ? weighted_sum / kernel.weight_sum : 0.0;
}

double LaneResampler::edge_value(const Kernel& kernel, std::int64_t first) const noexcept
{
    const std::span<const double> history{history_};
    const std::span<const double> weights{kernel.weights};
    const std::int64_t available = to_signed_index(pushed_);
    double weighted_sum = 0.0;
    double weight_sum = 0.0;

    for (std::size_t index = 0U; index < lane_filter_taps; ++index)
    {
        const std::int64_t source = first + to_signed_index(index);

        if (source < 0 || source >= available)
        {
            continue;
        }

        weighted_sum += history[to_unsigned_index(source) & history_mask] * weights[index];
        weight_sum += weights[index];
    }

    return std::abs(weight_sum) > weight_sum_floor ? weighted_sum / weight_sum : 0.0;
}

Status LaneDownsampler::attach(UnitSource& upstream, SampleRate input, SampleRate output, double gain) noexcept
{
    upstream_ = &upstream;
    staged_ = 0U;
    consumed_ = 0U;
    upstream_ended_ = false;
    return resampler_.configure(input, output, gain);
}

m110::Result<std::size_t> LaneDownsampler::read(std::span<Pcm16> destination) noexcept
{
    if (upstream_ == nullptr)
    {
        return Status{StatusCode::invalid_configuration, "downsampler has no source"};
    }

    std::size_t written = 0U;

    while (written < destination.size() && !resampler_.done())
    {
        if (resampler_.ready())
        {
            const m110::Result<Pcm16> sample = resampler_.next();

            if (!sample)
            {
                return sample.status();
            }

            destination[written] = sample.value();
            ++written;
            continue;
        }

        const Status fed = feed();

        if (!fed.is_ok())
        {
            return fed;
        }
    }

    return written;
}

Status LaneDownsampler::rewind() noexcept
{
    if (upstream_ == nullptr)
    {
        return {StatusCode::invalid_configuration, "downsampler has no source"};
    }

    staged_ = 0U;
    consumed_ = 0U;
    upstream_ended_ = false;
    resampler_.restart();
    return upstream_->rewind();
}

const LaneResampler& LaneDownsampler::resampler() const noexcept
{
    return resampler_;
}

// Moves one input sample into the resampler, refilling the staging buffer from the
// source or marking the end of the input when it is empty.
Status LaneDownsampler::feed() noexcept
{
    if (consumed_ < staged_)
    {
        const double sample = std::span<const double>{staging_}[consumed_];
        ++consumed_;
        return resampler_.push(sample);
    }

    if (upstream_ended_)
    {
        resampler_.finish();
        return Status::success();
    }

    const m110::Result<std::size_t> got = upstream_->read(staging_);

    if (!got)
    {
        return got.status();
    }

    if (got.value() > staging_.size())
    {
        return {StatusCode::internal_error, "source returned more samples than requested"};
    }

    upstream_ended_ = got.value() == 0U;
    staged_ = got.value();
    consumed_ = 0U;
    return Status::success();
}

Status LaneUpsampler::attach(Pcm16Sink& downstream, SampleRate input, SampleRate output, double gain) noexcept
{
    downstream_ = &downstream;
    staged_ = 0U;
    return resampler_.configure(input, output, gain);
}

Status LaneUpsampler::write(std::span<const Pcm16> samples) noexcept
{
    if (downstream_ == nullptr)
    {
        return {StatusCode::invalid_configuration, "upsampler has no sink"};
    }

    for (const Pcm16 sample : samples)
    {
        const Status drained = drain();

        if (!drained.is_ok())
        {
            return drained;
        }

        const Status pushed = resampler_.push(pcm16_to_unit(sample));

        if (!pushed.is_ok())
        {
            return pushed;
        }
    }

    return drain();
}

Status LaneUpsampler::finish() noexcept
{
    if (downstream_ == nullptr)
    {
        return {StatusCode::invalid_configuration, "upsampler has no sink"};
    }

    resampler_.finish();
    const Status drained = drain();
    return drained.is_ok() ? flush() : drained;
}

const LaneResampler& LaneUpsampler::resampler() const noexcept
{
    return resampler_;
}

Status LaneUpsampler::drain() noexcept
{
    while (resampler_.ready())
    {
        const m110::Result<Pcm16> sample = resampler_.next();

        if (!sample)
        {
            return sample.status();
        }

        std::span<Pcm16>{staging_}[staged_] = sample.value();
        ++staged_;

        if (staged_ == staging_.size())
        {
            const Status flushed = flush();

            if (!flushed.is_ok())
            {
                return flushed;
            }
        }
    }

    return Status::success();
}

Status LaneUpsampler::flush() noexcept
{
    const Status written = downstream_->write(std::span<const Pcm16>{staging_}.first(staged_));
    staged_ = 0U;
    return written;
}

} // namespace channel_sim

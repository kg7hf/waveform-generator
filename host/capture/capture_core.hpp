// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace waveform_capture
{

inline constexpr std::size_t capture_block_samples = 480U;
inline constexpr std::size_t capture_queue_blocks = 512U;

enum class ChannelPolicy : std::uint8_t
{
    left,
    right,
    average
};

[[nodiscard]] inline constexpr std::string_view channel_policy_name(ChannelPolicy policy) noexcept
{
    switch (policy)
    {
        case ChannelPolicy::left:
            return "left";

        case ChannelPolicy::right:
            return "right";

        case ChannelPolicy::average:
            return "average";
    }

    return "left";
}

[[nodiscard]] inline float pcm16_to_float(std::int16_t sample) noexcept
{
    return static_cast<float>(sample) / 32768.0F;
}

[[nodiscard]] inline float select_channels_to_mono(float left, float right,
        ChannelPolicy policy) noexcept
{
    return policy == ChannelPolicy::left ? left :
           policy == ChannelPolicy::right ? right : (left + right) * 0.5F;
}

[[nodiscard]] inline std::size_t pcm16_stereo_frame_count(std::size_t byte_count,
        std::size_t maximum_frames) noexcept
{
    return std::min(byte_count / (2U * sizeof(std::int16_t)), maximum_frames);
}

inline void downmix_pcm16_stereo_to_float(std::span<const std::int16_t> input,
        std::span<float> output,
        ChannelPolicy policy) noexcept
{
    const auto frames = std::min(input.size() / 2U, output.size());

    for (std::size_t index = 0U; index < frames; ++index)
    {
        const auto left = pcm16_to_float(input[index * 2U]);
        const auto right = pcm16_to_float(input[index * 2U + 1U]);
        output[index] = select_channels_to_mono(left, right, policy);
    }
}

struct SampleBlock
{
    std::uint32_t count{};
    std::array<float, capture_block_samples> samples{};
};

template <std::size_t Capacity>
class BoundedSampleQueue
{
    static_assert(Capacity > 1U);

public:
    [[nodiscard]] bool push(std::span<const float> samples) noexcept
    {
        if (samples.empty() || samples.size() > capture_block_samples)
        {
            return false;
        }

        const auto head = head_.load(std::memory_order_relaxed);
        const auto tail = tail_.load(std::memory_order_acquire);

        if (head - tail >= Capacity)
        {
            return false;
        }

        auto& block = blocks_[static_cast<std::size_t>(head % Capacity)];
        std::copy(samples.begin(), samples.end(), block.samples.begin());
        block.count = static_cast<std::uint32_t>(samples.size());
        head_.store(head + 1U, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool pop(SampleBlock& destination) noexcept
    {
        const auto tail = tail_.load(std::memory_order_relaxed);
        const auto head = head_.load(std::memory_order_acquire);

        if (tail == head)
        {
            return false;
        }

        destination = blocks_[static_cast<std::size_t>(tail % Capacity)];
        tail_.store(tail + 1U, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::uint64_t size() const noexcept
    {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }

private:
    std::array<SampleBlock, Capacity> blocks_{};
    std::atomic<std::uint64_t> head_{};
    std::atomic<std::uint64_t> tail_{};
};

struct CaptureMeasurements
{
    std::uint64_t samples{};
    std::uint64_t finite_samples{};
    std::uint64_t clipped_samples{};
    std::uint64_t nonfinite_samples{};
    float peak{};
    double sum_squares{};

    void observe(std::span<const float> input) noexcept
    {
        for (const auto sample : input)
        {
            ++samples;

            if (!std::isfinite(sample))
            {
                ++nonfinite_samples;
                continue;
            }

            ++finite_samples;
            const auto magnitude = std::abs(sample);
            peak = std::max(peak, magnitude);
            clipped_samples += magnitude >= 0.999F ? 1U : 0U;
            sum_squares += static_cast<double>(sample) * static_cast<double>(sample);
        }
    }

    [[nodiscard]] double rms() const noexcept
    {
        return finite_samples == 0U ? 0.0 : std::sqrt(sum_squares / static_cast<double>(finite_samples));
    }

    [[nodiscard]] double peak_dbfs() const noexcept
    {
        return peak == 0.0F ? -std::numeric_limits<double>::infinity() : 20.0 * std::log10(static_cast<double>(peak));
    }

    [[nodiscard]] double rms_dbfs() const noexcept
    {
        const auto value = rms();
        return value == 0.0 ? -std::numeric_limits<double>::infinity() : 20.0 * std::log10(value);
    }
};

} // namespace waveform_capture

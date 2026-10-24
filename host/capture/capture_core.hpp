// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

#include "common/status.hpp"

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

enum class CaptureSampleType : std::uint8_t
{
    pcm16,
    ieee_float32
};

enum class CompletionReason : std::uint8_t
{
    stop,
    host_disconnected,
    duration_limit,
    capture_error
};

// A backend may report failure while STOP is being processed. Resolve the
// final result after the capture worker has stopped, when its latch is stable.
[[nodiscard]] inline constexpr CompletionReason capture_completion_reason(CompletionReason observed, bool backend_failed, bool integrity_failed) noexcept
{
    return backend_failed || integrity_failed ? CompletionReason::capture_error : observed;
}

[[nodiscard]] inline constexpr bool valid_capture_format(std::uint32_t sample_rate, std::uint16_t channels, CaptureSampleType sample_type, std::uint16_t bits_per_sample, std::uint16_t block_align,
                                                         std::uint32_t average_bytes_per_second) noexcept
{
    if (sample_rate != 48000U || (channels != 1U && channels != 2U))
    {
        return false;
    }

    const auto expected_bits = sample_type == CaptureSampleType::pcm16 ? 16U : 32U;
    const auto bytes_per_sample = expected_bits / 8U;
    const auto expected_block_align = static_cast<std::uint16_t>(channels * bytes_per_sample);
    return bits_per_sample == expected_bits && block_align == expected_block_align && average_bytes_per_second == sample_rate * expected_block_align;
}

[[nodiscard]] inline constexpr bool valid_capture_packet(std::uint32_t reported_frames, std::uint32_t acquired_frames, std::size_t packet_capacity_frames, bool silent, bool data_present) noexcept
{
    return reported_frames == acquired_frames && acquired_frames <= packet_capacity_frames && (silent || data_present);
}

inline void latch_capture_failure(std::atomic<bool>& accepting, std::atomic<std::uint64_t>& dropped_blocks) noexcept
{
    dropped_blocks.fetch_add(1U, std::memory_order_relaxed);
    accepting.store(false, std::memory_order_release);
}

[[nodiscard]] inline bool capture_failure_latched(const std::atomic<bool>& worker_failed, const std::atomic<std::uint64_t>& dropped_blocks) noexcept
{
    return worker_failed.load(std::memory_order_acquire) || dropped_blocks.load(std::memory_order_acquire) != 0U;
}

[[nodiscard]] inline constexpr bool capture_start_allowed(bool opened, bool stopped, bool accepting, bool worker_active) noexcept
{
    return opened && !stopped && !accepting && !worker_active;
}

// Publish the first callback's timestamp before its ready flag. If callbacks
// overlap, only one claims the timestamp; a losing publisher acquires that
// claim before making the flag visible to the controller.
inline void publish_first_callback(std::atomic<bool>& ready, std::atomic<std::uint64_t>& first_ns, std::uint64_t timestamp_ns) noexcept
{
    std::uint64_t unset{};
    static_cast<void>(first_ns.compare_exchange_strong(unset, timestamp_ns, std::memory_order_acq_rel, std::memory_order_acquire));
    ready.store(true, std::memory_order_release);
}

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

[[nodiscard]] inline float select_channels_to_mono(float left, float right, ChannelPolicy policy) noexcept
{
    return policy == ChannelPolicy::left ? left : policy == ChannelPolicy::right ? right : (left + right) * 0.5F;
}

[[nodiscard]] inline std::size_t pcm16_stereo_frame_count(std::size_t byte_count, std::size_t maximum_frames) noexcept
{
    return std::min(byte_count / (2U * sizeof(std::int16_t)), maximum_frames);
}

inline void downmix_pcm16_stereo_to_float(std::span<const std::int16_t> input, std::span<float> output, ChannelPolicy policy) noexcept
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

template <typename Header, typename Unprepare> [[nodiscard]] inline m110::Status unprepare_prepared_prefix(std::span<Header> headers, std::size_t prepared_count, Unprepare unprepare) noexcept
{
    if (prepared_count > headers.size())
    {
        return {m110::StatusCode::invalid_argument, "prepared header count exceeds its buffer count"};
    }

    for (std::size_t index = 0U; index < prepared_count; ++index)
    {
        if (!unprepare(headers[index]))
        {
            return {m110::StatusCode::io_error, "a prepared capture header could not be unprepared"};
        }
    }

    return m110::Status::success();
}

// push()'s refusals: the block is dropped.
inline constexpr m110::Status bad_sample_block{.code = m110::StatusCode::invalid_argument, .message = "a sample block holds no samples or more than capture_block_samples"};
inline constexpr m110::Status sample_queue_full{.code = m110::StatusCode::busy, .message = "the sample queue is full"};

template <std::size_t Capacity> class BoundedSampleQueue
{
    static_assert(Capacity > 1U);

public:
    // Copies `samples` in as one block: success, or bad_sample_block or
    // sample_queue_full, and the block is dropped. Caller: the one producer.
    [[nodiscard]] m110::Status push(std::span<const float> samples) noexcept
    {
        if (samples.empty() || samples.size() > capture_block_samples)
        {
            return bad_sample_block;
        }

        const auto head = head_.load(std::memory_order_relaxed);
        const auto tail = tail_.load(std::memory_order_acquire);

        if (head - tail >= Capacity)
        {
            return sample_queue_full;
        }

        auto& block = blocks_[static_cast<std::size_t>(head % Capacity)];
        std::copy(samples.begin(), samples.end(), block.samples.begin());
        block.count = static_cast<std::uint32_t>(samples.size());
        head_.store(head + 1U, std::memory_order_release);
        return m110::Status::success();
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

    [[nodiscard]] std::uint64_t size() const noexcept { return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire); }

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

    [[nodiscard]] double rms() const noexcept { return finite_samples == 0U ? 0.0 : std::sqrt(sum_squares / static_cast<double>(finite_samples)); }

    [[nodiscard]] double peak_dbfs() const noexcept { return peak == 0.0F ? -std::numeric_limits<double>::infinity() : 20.0 * std::log10(static_cast<double>(peak)); }

    [[nodiscard]] double rms_dbfs() const noexcept
    {
        const auto value = rms();
        return value == 0.0 ? -std::numeric_limits<double>::infinity() : 20.0 * std::log10(value);
    }
};

} // namespace waveform_capture

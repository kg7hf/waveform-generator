// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "host/capture/capture_core.hpp"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>

TEST_CASE("capture core bounded queue, measurements and PCM downmix", "[capture_core]")
{
    waveform_capture::BoundedSampleQueue<2U> queue;
    const std::array<float, 3> first{1.0F, -0.5F, 0.0F};
    const std::array<float, 2> second{0.25F, -0.25F};
    const std::array<float, waveform_capture::capture_block_samples + 1U> oversized{};
    // Keep the original short-circuit sequence: rejected pushes must not
    // disturb the queue before the two successful pushes fill it.
    REQUIRE((queue.push(std::span<const float>{}).code == m110::StatusCode::invalid_argument && queue.push(oversized).code == m110::StatusCode::invalid_argument && queue.size() == 0U &&
             queue.push(first).is_ok() && queue.push(second).is_ok() && queue.push(first).code == m110::StatusCode::busy && queue.size() == 2U));
    waveform_capture::SampleBlock block{};
    REQUIRE((queue.pop(block) && block.count == first.size() && block.samples[0] == first[0] && queue.pop(block) && block.count == second.size() && !queue.pop(block)));
    waveform_capture::CaptureMeasurements measurements;
    const std::array<float, 5> samples{0.5F, -1.0F, 0.25F, NAN, 0.0F};
    measurements.observe(samples);
    REQUIRE((measurements.samples == 5U && measurements.finite_samples == 4U && measurements.nonfinite_samples == 1U && measurements.clipped_samples == 1U && measurements.peak == 1.0F &&
             std::abs(measurements.rms() - std::sqrt(1.3125 / 4.0)) <= 1.0e-6));
    const std::array<std::int16_t, 6> stereo{-32768, -32768, 32767, 32767, -32768, 32767};
    std::array<float, 3> mono{};
    waveform_capture::downmix_pcm16_stereo_to_float(stereo, mono, waveform_capture::ChannelPolicy::average);
    REQUIRE((mono[0] == -1.0F && std::abs(mono[1] - (32767.0F / 32768.0F)) <= 1.0e-7F && std::abs(mono[2] + (1.0F / 65536.0F)) <= 1.0e-7F));
    const std::array<std::int16_t, 4> opposite_polarity{8192, -8192, -16384, -8192};
    std::array<float, 2> selected{};
    waveform_capture::downmix_pcm16_stereo_to_float(opposite_polarity, selected, waveform_capture::ChannelPolicy::left);
    REQUIRE((std::abs(selected[0] - 0.25F) <= 1.0e-7F && std::abs(selected[1] + 0.5F) <= 1.0e-7F));
    waveform_capture::downmix_pcm16_stereo_to_float(opposite_polarity, selected, waveform_capture::ChannelPolicy::right);
    REQUIRE((std::abs(selected[0] + 0.25F) <= 1.0e-7F && std::abs(selected[1] + 0.25F) <= 1.0e-7F));
    waveform_capture::downmix_pcm16_stereo_to_float(opposite_polarity, selected, waveform_capture::ChannelPolicy::average);
    REQUIRE((std::abs(selected[0]) <= 1.0e-7F && std::abs(selected[1] + 0.375F) <= 1.0e-7F));
    REQUIRE((waveform_capture::select_channels_to_mono(0.25F, -0.25F, waveform_capture::ChannelPolicy::left) == 0.25F &&
             waveform_capture::select_channels_to_mono(0.25F, -0.25F, waveform_capture::ChannelPolicy::right) == -0.25F &&
             waveform_capture::select_channels_to_mono(0.25F, -0.25F, waveform_capture::ChannelPolicy::average) == 0.0F));
    REQUIRE((waveform_capture::pcm16_stereo_frame_count(7U, 480U) == 1U && waveform_capture::pcm16_stereo_frame_count(9600U, 480U) == 480U));
}

TEST_CASE("capture adapter validation rejects malformed formats and packets", "[capture_core][capture_adapters]")
{
    using waveform_capture::CaptureSampleType;

    REQUIRE(waveform_capture::valid_capture_format(48000U, 1U, CaptureSampleType::pcm16, 16U, 2U, 96000U));
    REQUIRE(waveform_capture::valid_capture_format(48000U, 2U, CaptureSampleType::pcm16, 16U, 4U, 192000U));
    REQUIRE(waveform_capture::valid_capture_format(48000U, 1U, CaptureSampleType::ieee_float32, 32U, 4U, 192000U));
    REQUIRE(waveform_capture::valid_capture_format(48000U, 2U, CaptureSampleType::ieee_float32, 32U, 8U, 384000U));
    REQUIRE_FALSE(waveform_capture::valid_capture_format(44100U, 2U, CaptureSampleType::pcm16, 16U, 4U, 176400U));
    REQUIRE_FALSE(waveform_capture::valid_capture_format(48000U, 3U, CaptureSampleType::pcm16, 16U, 6U, 288000U));
    REQUIRE_FALSE(waveform_capture::valid_capture_format(48000U, 2U, CaptureSampleType::pcm16, 24U, 6U, 288000U));
    REQUIRE_FALSE(waveform_capture::valid_capture_format(48000U, 2U, CaptureSampleType::pcm16, 16U, 8U, 384000U));
    REQUIRE_FALSE(waveform_capture::valid_capture_format(48000U, 2U, CaptureSampleType::pcm16, 16U, 4U, 384000U));

    REQUIRE(waveform_capture::valid_capture_packet(480U, 480U, 480U, false, true));
    REQUIRE(waveform_capture::valid_capture_packet(480U, 480U, 480U, true, false));
    REQUIRE_FALSE(waveform_capture::valid_capture_packet(481U, 480U, 480U, false, true));
    REQUIRE_FALSE(waveform_capture::valid_capture_packet(481U, 481U, 480U, false, true));
    REQUIRE_FALSE(waveform_capture::valid_capture_packet(480U, 480U, 480U, false, false));
}

TEST_CASE("capture callback and requeue failures stop further delivery", "[capture_core][capture_adapters]")
{
    std::atomic<bool> accepting{true};
    std::atomic<bool> worker_failed{};
    std::atomic<std::uint64_t> dropped_blocks{};

    REQUIRE_FALSE(waveform_capture::capture_failure_latched(worker_failed, dropped_blocks));

    waveform_capture::latch_capture_failure(accepting, dropped_blocks);
    REQUIRE_FALSE(accepting.load(std::memory_order_acquire));
    REQUIRE(dropped_blocks.load(std::memory_order_acquire) == 1U);
    REQUIRE(waveform_capture::capture_failure_latched(worker_failed, dropped_blocks));

    waveform_capture::latch_capture_failure(accepting, dropped_blocks);
    REQUIRE_FALSE(accepting.load(std::memory_order_acquire));
    REQUIRE(dropped_blocks.load(std::memory_order_acquire) == 2U);
}

TEST_CASE("capture callback timestamp is visible before its ready flag", "[capture_core][capture_adapters]")
{
    std::atomic<bool> ready{};
    std::atomic<std::uint64_t> first_ns{};
    std::uint64_t observed{};

    std::thread reader{[&ready, &first_ns, &observed]
                       {
                           while (!ready.load(std::memory_order_acquire))
                           {
                               std::this_thread::yield();
                           }

                           observed = first_ns.load(std::memory_order_acquire);
                       }};
    std::thread first{[&ready, &first_ns] { waveform_capture::publish_first_callback(ready, first_ns, 17U); }};
    std::thread second{[&ready, &first_ns] { waveform_capture::publish_first_callback(ready, first_ns, 23U); }};

    first.join();
    second.join();
    reader.join();

    REQUIRE((observed == 17U || observed == 23U));
    const auto claimed = first_ns.load(std::memory_order_acquire);
    REQUIRE(claimed == observed);
    waveform_capture::publish_first_callback(ready, first_ns, 41U);
    REQUIRE(first_ns.load(std::memory_order_acquire) == claimed);
}

TEST_CASE("capture start gate refuses a stopped or active adapter", "[capture_core][capture_adapters]")
{
    using waveform_capture::capture_start_allowed;

    REQUIRE(capture_start_allowed(true, false, false, false));
    REQUIRE_FALSE(capture_start_allowed(false, false, false, false));
    REQUIRE_FALSE(capture_start_allowed(true, true, false, false));
    REQUIRE_FALSE(capture_start_allowed(true, false, true, false));
    REQUIRE_FALSE(capture_start_allowed(true, false, false, true));
}

TEST_CASE("late backend failure overrides a requested capture stop", "[capture_core][capture_adapters]")
{
    using waveform_capture::CompletionReason;
    std::atomic<bool> worker_failed{};
    std::atomic<std::uint64_t> dropped_blocks{};

    REQUIRE(waveform_capture::capture_completion_reason(CompletionReason::stop, waveform_capture::capture_failure_latched(worker_failed, dropped_blocks), false) == CompletionReason::stop);

    // The stop request wins the controller loop, then the worker reports its
    // failure before joining. The final CLI result must not claim success.
    worker_failed.store(true, std::memory_order_release);
    REQUIRE(waveform_capture::capture_completion_reason(CompletionReason::stop, waveform_capture::capture_failure_latched(worker_failed, dropped_blocks), false) == CompletionReason::capture_error);
    REQUIRE(waveform_capture::capture_completion_reason(CompletionReason::stop, false, true) == CompletionReason::capture_error);
    REQUIRE(waveform_capture::capture_completion_reason(CompletionReason::duration_limit, false, false) == CompletionReason::duration_limit);
}

TEST_CASE("WinMM cleanup only unprepares the successfully prepared prefix", "[capture_adapters][winmm]")
{
    std::array<int, 16U> headers{};
    std::size_t unprepared{};
    const auto succeeded = waveform_capture::unprepare_prepared_prefix(std::span<int>{headers}, 4U,
                                                                       [&unprepared](int&) noexcept
                                                                       {
                                                                           ++unprepared;
                                                                           return true;
                                                                       });

    REQUIRE(succeeded.is_ok());
    REQUIRE(unprepared == 4U);

    unprepared = 0U;
    const auto invalid_count = waveform_capture::unprepare_prepared_prefix(std::span<int>{headers}, headers.size() + 1U,
                                                                           [&unprepared](int&) noexcept
                                                                           {
                                                                               ++unprepared;
                                                                               return true;
                                                                           });
    REQUIRE(invalid_count.code == m110::StatusCode::invalid_argument);
    REQUIRE(unprepared == 0U);

    unprepared = 0U;
    const auto refusal = waveform_capture::unprepare_prepared_prefix(std::span<int>{headers}, 4U,
                                                                     [&unprepared](int&) noexcept
                                                                     {
                                                                         ++unprepared;
                                                                         return unprepared != 3U;
                                                                     });
    REQUIRE(refusal.code == m110::StatusCode::io_error);
    REQUIRE(unprepared == 3U);
}

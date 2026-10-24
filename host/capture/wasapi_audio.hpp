// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

#include "common/status.hpp"
#include "host/capture/capture_core.hpp"
#include "host/capture/winmm_audio.hpp"

#include <atomic>
#include <cstdint>
#include <span>
#include <string_view>

namespace waveform_capture
{

class WasapiCapture
{
public:
    WasapiCapture() = default;
    ~WasapiCapture() noexcept;
    WasapiCapture(const WasapiCapture&) = delete;
    WasapiCapture& operator=(const WasapiCapture&) = delete;
    WasapiCapture(WasapiCapture&&) = delete;
    WasapiCapture& operator=(WasapiCapture&&) = delete;

    [[nodiscard]] static std::uint32_t enumerate(DeviceVisitor visitor, void* context) noexcept;
    // Selects the one active capture endpoint whose friendly name is
    // `expected_name`: success, or the Status of the step that failed.
    [[nodiscard]] m110::Status open(std::string_view expected_name, SamplesCallback callback, void* context, ChannelPolicy channel_policy) noexcept;
    // Starts the worker once per open; use a new capture instance for another
    // run after stop. Worker failures come through status().
    [[nodiscard]] m110::Status start() noexcept;
    void stop() noexcept;
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept { return state_ != nullptr; }

    [[nodiscard]] bool failed() const noexcept { return capture_failure_latched(worker_failed_, counters_.dropped_blocks); }

    // The first worker failure or a rejected sample block once failed();
    // success until then.
    [[nodiscard]] m110::Status status() const noexcept;

    [[nodiscard]] std::string_view native_name() const noexcept { return native_name_; }

    [[nodiscard]] std::string_view endpoint_id() const noexcept { return endpoint_id_; }

    [[nodiscard]] ChannelPolicy channel_policy() const noexcept { return channel_policy_; }

    [[nodiscard]] WinmmCounters counters() const noexcept;
    [[nodiscard]] std::string_view format_name() const noexcept { return format_name_; }

private:
    struct State;
    void worker_main() noexcept;
    // The worker's first failure: kept for status(), then failed() is true.
    // Caller: the worker thread, the one writer of both.
    void fail_worker(m110::Status failure) noexcept;
    void record_stop_result(long result) noexcept;
    void record_drained_packet(std::size_t samples) noexcept;

    State* state_{};
    char native_name_[128]{};
    char endpoint_id_[512]{};
    char format_name_[64]{"unknown"};
    SamplesCallback callback_{};
    void* context_{};
    ChannelPolicy channel_policy_{ChannelPolicy::left};
    std::atomic<bool> accepting_{};
    std::atomic<bool> stopping_{};
    std::atomic<bool> stopped_{true};
    std::atomic<bool> worker_failed_{};
    // Written by the worker before worker_failed_ is set (release), read only
    // once failed() has seen it set (acquire).
    m110::Status worker_status_{};
    struct AtomicCounters
    {
        std::atomic<std::uint64_t> input_buffers{};
        std::atomic<std::uint64_t> partial_buffers{};
        std::atomic<std::uint64_t> reset_buffers{};
        std::atomic<std::uint64_t> reset_partial_buffers{};
        std::atomic<std::uint64_t> reset_samples{};
        std::atomic<std::uint64_t> dropped_blocks{};
        std::atomic<std::uint64_t> samples_delivered{};
        std::atomic<std::uint64_t> data_discontinuity_packets{};
        std::atomic<std::uint64_t> startup_discontinuity_packets{};
        std::atomic<std::uint64_t> silent_packets{};
        std::atomic<std::uint64_t> position_packets{};
        std::atomic<std::uint64_t> position_errors{};
        std::atomic<std::uint64_t> first_device_position{};
        std::atomic<std::uint64_t> first_qpc_position{};
        std::atomic<std::uint64_t> last_device_position{};
        std::atomic<std::uint64_t> last_qpc_position{};
    } counters_{};
};

} // namespace waveform_capture

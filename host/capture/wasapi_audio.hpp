// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#pragma once

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

    [[nodiscard]] static std::uint32_t enumerate(DeviceVisitor visitor, void* context) noexcept;
    [[nodiscard]] bool open(std::string_view expected_name, SamplesCallback callback, void* context,
                            ChannelPolicy channel_policy) noexcept;
    [[nodiscard]] bool start() noexcept;
    void stop() noexcept;
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept
    {
        return state_ != nullptr;
    }
    [[nodiscard]] bool failed() const noexcept
    {
        return worker_failed_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::string_view native_name() const noexcept
    {
        return native_name_;
    }
    [[nodiscard]] std::string_view endpoint_id() const noexcept
    {
        return endpoint_id_;
    }
    [[nodiscard]] ChannelPolicy channel_policy() const noexcept
    {
        return channel_policy_;
    }
    [[nodiscard]] WinmmCounters counters() const noexcept;
    [[nodiscard]] std::string_view format_name() const noexcept
    {
        return format_name_;
    }
    [[nodiscard]] const char* error() const noexcept
    {
        return error_;
    }

private:
    struct State;
    void worker_main() noexcept;
    void set_error(const char* message) noexcept;

    State* state_{};
    char native_name_[128] {};
    char endpoint_id_[512] {};
    char format_name_[64] {"unknown"};
    SamplesCallback callback_{};
    void* context_{};
    ChannelPolicy channel_policy_{ChannelPolicy::left};
    std::atomic<bool> accepting_{};
    std::atomic<bool> stopping_{};
    std::atomic<bool> stopped_{true};
    std::atomic<bool> worker_failed_{};
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
    char error_[128] {"ok"};
};

} // namespace waveform_capture

// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#pragma once

#include "host/capture/capture_core.hpp"

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <span>
#include <string_view>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace waveform_capture
{

struct WinmmDevice
{
    std::uint32_t index{};
    std::string_view native_name{};
    std::uint32_t max_input_channels{};
};

using DeviceVisitor = void (*)(void* context, const WinmmDevice& device) noexcept;
using SamplesCallback = bool (*)(void* context, std::span<const float> samples) noexcept;

struct WinmmCounters
{
    std::uint64_t input_buffers{};
    std::uint64_t partial_buffers{};
    std::uint64_t reset_buffers{};
    std::uint64_t reset_partial_buffers{};
    std::uint64_t reset_samples{};
    std::uint64_t dropped_blocks{};
    std::uint64_t samples_delivered{};
    std::uint64_t data_discontinuity_packets{};
    std::uint64_t startup_discontinuity_packets{};
    std::uint64_t silent_packets{};
    std::uint64_t position_packets{};
    std::uint64_t position_errors{};
    std::uint64_t first_device_position{};
    std::uint64_t first_qpc_position{};
    std::uint64_t last_device_position{};
    std::uint64_t last_qpc_position{};
};

class WinmmCapture
{
public:
    struct PlatformState;

    WinmmCapture() = default;
    ~WinmmCapture() noexcept;
    WinmmCapture(const WinmmCapture&) = delete;
    WinmmCapture& operator=(const WinmmCapture&) = delete;

    [[nodiscard]] static std::uint32_t enumerate(DeviceVisitor visitor, void* context) noexcept;
    [[nodiscard]] bool open(std::uint32_t device_index, std::string_view expected_name,
                            SamplesCallback callback, void* context,
                            ChannelPolicy channel_policy) noexcept;
    [[nodiscard]] bool start() noexcept;
    void stop() noexcept;
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept
    {
        return handle_ != nullptr;
    }
    [[nodiscard]] bool failed() const noexcept
    {
        return false;
    }
    [[nodiscard]] bool is_running() const noexcept
    {
        return accepting_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint32_t device_index() const noexcept
    {
        return device_index_;
    }
    [[nodiscard]] std::string_view native_name() const noexcept
    {
        return native_name_;
    }
    [[nodiscard]] std::string_view endpoint_id() const noexcept
    {
        return {};
    }
    [[nodiscard]] ChannelPolicy channel_policy() const noexcept
    {
        return channel_policy_;
    }
    [[nodiscard]] WinmmCounters counters() const noexcept;
    [[nodiscard]] static constexpr std::string_view input_format() noexcept
    {
        return "PCM16_LE_STEREO_48000_DOWNMIX_MONO";
    }
    [[nodiscard]] const char* error() const noexcept
    {
        return error_;
    }

private:
    static void CALLBACK input_callback(void* handle, unsigned int message,
                                        std::uintptr_t instance, std::uintptr_t parameter,
                                        std::uintptr_t reserved) noexcept;
    void complete_buffer(void* header) noexcept;
    void wait_callbacks_idle() noexcept;
    void set_error(const char* message) noexcept;

    void* handle_{};
    std::uint32_t device_index_{};
    char native_name_[32] {};
    SamplesCallback callback_{};
    void* context_{};
    ChannelPolicy channel_policy_{ChannelPolicy::left};
    std::atomic<bool> accepting_{};
    std::atomic<bool> resetting_{};
    std::atomic<bool> stopped_{};
    std::atomic<std::uint32_t> callbacks_active_{};
    HANDLE callbacks_idle_event_{};
    struct AtomicCounters
    {
        std::atomic<std::uint64_t> input_buffers{};
        std::atomic<std::uint64_t> partial_buffers{};
        std::atomic<std::uint64_t> reset_buffers{};
        std::atomic<std::uint64_t> reset_partial_buffers{};
        std::atomic<std::uint64_t> reset_samples{};
        std::atomic<std::uint64_t> dropped_blocks{};
        std::atomic<std::uint64_t> samples_delivered{};
    } counters_{};
    char error_[128] {"ok"};

    PlatformState* platform_{};
};

} // namespace waveform_capture

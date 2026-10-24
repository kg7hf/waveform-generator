// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

#include "common/status.hpp"
#include "host/capture/capture_core.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string_view>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <mmsystem.h>

namespace waveform_capture
{

struct WinmmDevice
{
    std::uint32_t index{};
    std::string_view native_name{};
    std::uint32_t max_input_channels{};
};

using DeviceVisitor = void (*)(void* context, const WinmmDevice& device) noexcept;
// Called on the adapter-owned worker, never inside WinMM's native callback.
// Takes one block of mono samples: success, or the Status with which the
// block was dropped (counted in dropped_blocks).
using SamplesCallback = m110::Status (*)(void* context, std::span<const float> samples) noexcept;

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

    // Driver calls are grouped here so failure paths can be exercised without
    // an input device. Production uses the native WinMM functions.
    struct DriverApi
    {
        decltype(&::waveInGetNumDevs) get_num_devs{&::waveInGetNumDevs};
        decltype(&::waveInGetDevCapsA) get_dev_caps{&::waveInGetDevCapsA};
        decltype(&::waveInOpen) open{&::waveInOpen};
        decltype(&::waveInPrepareHeader) prepare_header{&::waveInPrepareHeader};
        decltype(&::waveInAddBuffer) add_buffer{&::waveInAddBuffer};
        decltype(&::waveInStart) start{&::waveInStart};
        decltype(&::waveInStop) stop{&::waveInStop};
        decltype(&::waveInReset) reset{&::waveInReset};
        decltype(&::waveInUnprepareHeader) unprepare_header{&::waveInUnprepareHeader};
        decltype(&::waveInClose) close{&::waveInClose};
        decltype(&::SetEvent) set_event{&::SetEvent};
        decltype(&::ResetEvent) reset_event{&::ResetEvent};
        decltype(&::WaitForSingleObject) wait{&::WaitForSingleObject};
    };

    WinmmCapture() = default;
    explicit WinmmCapture(const DriverApi& driver) noexcept : driver_{driver} {}
    ~WinmmCapture() noexcept;
    WinmmCapture(const WinmmCapture&) = delete;
    WinmmCapture& operator=(const WinmmCapture&) = delete;
    WinmmCapture(WinmmCapture&&) = delete;
    WinmmCapture& operator=(WinmmCapture&&) = delete;

    [[nodiscard]] static std::uint32_t enumerate(DeviceVisitor visitor, void* context) noexcept;
    // Opens input `device_index` for PCM16 stereo 48 kHz once its name is still
    // `expected_name`, with every buffer queued: success, or the Status of the
    // step that failed (nothing is left open then).
    [[nodiscard]] m110::Status open(std::uint32_t device_index, std::string_view expected_name, SamplesCallback callback, void* context, ChannelPolicy channel_policy) noexcept;
    // Starts an open capture once. stop() resets and drains queued buffers;
    // close() and open() are required before another start.
    [[nodiscard]] m110::Status start() noexcept;
    void stop() noexcept;
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept { return handle_ != nullptr; }

    // True after an input callback drops a block or a driver buffer cannot be requeued.
    [[nodiscard]] bool failed() const noexcept;

    // The reason for failed(), or success while capture is healthy.
    [[nodiscard]] m110::Status status() const noexcept;

    [[nodiscard]] bool is_running() const noexcept { return accepting_.load(std::memory_order_acquire); }

    [[nodiscard]] std::uint32_t device_index() const noexcept { return device_index_; }

    [[nodiscard]] std::string_view native_name() const noexcept { return native_name_; }

    [[nodiscard]] std::string_view endpoint_id() const noexcept { return {}; }

    [[nodiscard]] ChannelPolicy channel_policy() const noexcept { return channel_policy_; }

    [[nodiscard]] WinmmCounters counters() const noexcept;
    [[nodiscard]] static constexpr std::string_view input_format() noexcept { return "PCM16_LE_STEREO_48000_DOWNMIX_MONO"; }

private:
    static void CALLBACK input_callback(void* handle, unsigned int message, std::uintptr_t instance, std::uintptr_t parameter, std::uintptr_t reserved) noexcept;
    static DWORD WINAPI worker_entry(void* context) noexcept;
    void run_worker() noexcept;
    void enqueue_completed(void* header) noexcept;
    void complete_buffer(void* header, bool reset_buffer) noexcept;
    void wait_callbacks_idle() noexcept;
    void join_worker() noexcept;

    DriverApi driver_{};
    void* handle_{};
    std::uint32_t device_index_{};
    char native_name_[32]{};
    SamplesCallback callback_{};
    void* context_{};
    ChannelPolicy channel_policy_{ChannelPolicy::left};
    std::atomic<bool> accepting_{};
    std::atomic<bool> resetting_{};
    std::atomic<bool> stopped_{};
    // The completed-header ring has one producer. Reject overlapping driver
    // callbacks instead of allowing concurrent writes to that ring.
    std::atomic_flag completion_callback_active_ = ATOMIC_FLAG_INIT;
    std::atomic<std::uint32_t> callbacks_active_{};
    HANDLE callbacks_idle_event_{};
    HANDLE worker_event_{};
    HANDLE worker_thread_{};
    std::atomic<bool> worker_stop_{};
    std::mutex driver_mutex_{};
    bool driver_started_{};
    std::size_t prepared_headers_{};
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

    PlatformState* platform_{};
};

} // namespace waveform_capture

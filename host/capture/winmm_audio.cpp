// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#include "host/capture/winmm_audio.hpp"

#include "host/capture/capture_core.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <new>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmreg.h>
#include <mmsystem.h>

namespace waveform_capture
{

struct WinmmCapture::PlatformState
{
    static constexpr std::size_t buffer_count = 16U;
    std::array<WAVEHDR, buffer_count> headers{};
    std::array<std::array<std::int16_t, capture_block_samples * 2U>, buffer_count> samples{};
    std::array<std::array<float, capture_block_samples>, buffer_count> converted{};
};

void WinmmCapture::set_error(const char* message) noexcept
{
    std::strncpy(error_, message, sizeof(error_) - 1U);
    error_[sizeof(error_) - 1U] = '\0';
}

std::uint32_t WinmmCapture::enumerate(DeviceVisitor visitor, void* context) noexcept
{
    const auto count = waveInGetNumDevs();

    for (UINT index = 0U; index < count; ++index)
    {
        WAVEINCAPSA capabilities{};

        if (waveInGetDevCapsA(index, &capabilities, sizeof(capabilities)) == MMSYSERR_NOERROR)
        {
            const WinmmDevice device{index, std::string_view{capabilities.szPname}, capabilities.wChannels};

            if (visitor != nullptr)
            {
                visitor(context, device);
            }
        }
    }

    return count;
}

bool WinmmCapture::open(std::uint32_t device_index, std::string_view expected_name,
                        SamplesCallback callback, void* context, ChannelPolicy channel_policy) noexcept
{
    if (handle_ != nullptr || callback == nullptr || device_index >= waveInGetNumDevs() || expected_name.empty())
    {
        set_error("invalid WinMM capture arguments");
        return false;
    }

    WAVEINCAPSA capabilities{};

    if (waveInGetDevCapsA(device_index, &capabilities, sizeof(capabilities)) != MMSYSERR_NOERROR ||
            expected_name != std::string_view{capabilities.szPname})
    {
        set_error("WinMM input identity changed or does not match exactly");
        return false;
    }
    platform_ = new (std::nothrow) PlatformState{};

    if (platform_ == nullptr)
    {
        set_error("cannot allocate WinMM capture buffers");
        return false;
    }

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2U;
    format.nSamplesPerSec = 48000U;
    format.wBitsPerSample = 16U;
    format.nBlockAlign = static_cast<WORD>(2U * sizeof(std::int16_t));
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    if (waveInOpen(nullptr, device_index, &format, 0U, 0U, WAVE_FORMAT_QUERY) != MMSYSERR_NOERROR)
    {
        delete platform_;
        platform_ = nullptr;
        set_error("WinMM input does not accept requested PCM16 stereo 48 kHz format");
        return false;
    }

    HWAVEIN handle{};

    if (waveInOpen(&handle, device_index, &format,
                   reinterpret_cast<DWORD_PTR>(&WinmmCapture::input_callback),
                   reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION) != MMSYSERR_NOERROR)
    {
        delete platform_;
        platform_ = nullptr;
        set_error("waveInOpen failed for requested PCM16 stereo 48 kHz");
        return false;
    }

    handle_ = handle;
    stopped_.store(false, std::memory_order_release);
    device_index_ = device_index;
    std::strncpy(native_name_, capabilities.szPname, sizeof(native_name_) - 1U);
    native_name_[sizeof(native_name_) - 1U] = '\0';
    callback_ = callback;
    context_ = context;
    channel_policy_ = channel_policy;
    callbacks_idle_event_ = CreateEventW(nullptr, TRUE, TRUE, nullptr);

    if (callbacks_idle_event_ == nullptr)
    {
        set_error("cannot create WinMM callback synchronization event");
        close();
        return false;
    }

    for (auto& header : platform_->headers)
    {
        const auto index = static_cast<std::size_t>(&header - platform_->headers.data());
        header.lpData = reinterpret_cast<LPSTR>(platform_->samples[index].data());
        header.dwBufferLength = static_cast<DWORD>(capture_block_samples * 2U * sizeof(std::int16_t));

        if (waveInPrepareHeader(static_cast<HWAVEIN>(handle_), &header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR ||
                waveInAddBuffer(static_cast<HWAVEIN>(handle_), &header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
        {
            set_error("WinMM input buffer preparation failed");
            close();
            return false;
        }
    }

    set_error("ok");
    return true;
}

bool WinmmCapture::start() noexcept
{
    if (handle_ == nullptr || accepting_.load(std::memory_order_acquire))
    {
        set_error("WinMM capture is not startable");
        return false;
    }

    stopped_.store(false, std::memory_order_release);
    resetting_.store(false, std::memory_order_release);
    accepting_.store(true, std::memory_order_release);

    if (waveInStart(static_cast<HWAVEIN>(handle_)) != MMSYSERR_NOERROR)
    {
        accepting_.store(false, std::memory_order_release);
        set_error("waveInStart failed");
        return false;
    }

    return true;
}

void WinmmCapture::stop() noexcept
{
    if (handle_ == nullptr || stopped_.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }

    accepting_.store(false, std::memory_order_release);
    resetting_.store(true, std::memory_order_release);

    if (callbacks_idle_event_ != nullptr)
    {
        static_cast<void>(ResetEvent(callbacks_idle_event_));
    }

    static_cast<void>(waveInStop(static_cast<HWAVEIN>(handle_)));

    static_cast<void>(waveInReset(static_cast<HWAVEIN>(handle_)));

    wait_callbacks_idle();

    resetting_.store(false, std::memory_order_release);
}

void WinmmCapture::close() noexcept
{
    if (handle_ == nullptr)
    {
        delete platform_;
        platform_ = nullptr;

        if (callbacks_idle_event_ != nullptr)
        {
            static_cast<void>(CloseHandle(callbacks_idle_event_));
            callbacks_idle_event_ = nullptr;
        }

        return;
    }

    stop();

    for (auto& header : platform_->headers)
    {
        static_cast<void>(waveInUnprepareHeader(static_cast<HWAVEIN>(handle_), &header, sizeof(WAVEHDR)));
    }

    static_cast<void>(waveInClose(static_cast<HWAVEIN>(handle_)));

    handle_ = nullptr;

    if (callbacks_idle_event_ != nullptr)
    {
        static_cast<void>(CloseHandle(callbacks_idle_event_));
        callbacks_idle_event_ = nullptr;
    }

    delete platform_;
    platform_ = nullptr;
}

WinmmCapture::~WinmmCapture() noexcept
{
    close();
}

void CALLBACK WinmmCapture::input_callback(void*, unsigned int message,
        std::uintptr_t instance, std::uintptr_t parameter,
        std::uintptr_t) noexcept
{
    if (message == WIM_DATA && instance != 0U && parameter != 0U)
    {
        auto* capture = static_cast<WinmmCapture*>(reinterpret_cast<void*>(instance));
        capture->callbacks_active_.fetch_add(1U, std::memory_order_acq_rel);
        capture->complete_buffer(reinterpret_cast<void*>(parameter));

        if (capture->callbacks_active_.fetch_sub(1U, std::memory_order_acq_rel) == 1U &&
                capture->callbacks_idle_event_ != nullptr)
        {
            static_cast<void>(SetEvent(capture->callbacks_idle_event_));
        }
    }
}

void WinmmCapture::complete_buffer(void* raw_header) noexcept
{
    if (platform_ == nullptr)
    {
        return;
    }

    auto* header = static_cast<WAVEHDR*>(raw_header);
    std::size_t index = 0U;

    for (; index < platform_->headers.size(); ++index)
        if (&platform_->headers[index] == header)
        {
            break;
        }

    if (index == platform_->headers.size())
    {
        counters_.dropped_blocks.fetch_add(1U, std::memory_order_relaxed);
        return;
    }

    const bool reset_buffer = resetting_.load(std::memory_order_acquire);
    counters_.input_buffers.fetch_add(1U, std::memory_order_relaxed);
    const auto count = pcm16_stereo_frame_count(header->dwBytesRecorded, capture_block_samples);

    if (reset_buffer)
    {
        counters_.reset_buffers.fetch_add(1U, std::memory_order_relaxed);
        counters_.reset_samples.fetch_add(count, std::memory_order_relaxed);

        if (count != 0U && count < capture_block_samples)
        {
            counters_.reset_partial_buffers.fetch_add(1U, std::memory_order_relaxed);
        }
    }
    else if (header->dwBytesRecorded != capture_block_samples * 2U * sizeof(std::int16_t))
    {
        counters_.partial_buffers.fetch_add(1U, std::memory_order_relaxed);
    }

    counters_.samples_delivered.fetch_add(count, std::memory_order_relaxed);

    if (count != 0U)
    {
        downmix_pcm16_stereo_to_float(
        std::span<const std::int16_t> {platform_->samples[index].data(), count * 2U},
        std::span<float> {platform_->converted[index].data(), count}, channel_policy_);

        if (!callback_(context_, std::span<const float> {platform_->converted[index].data(), count}))
        counters_.dropped_blocks.fetch_add(1U, std::memory_order_relaxed);
    }

    header->dwBytesRecorded = 0U;

    if (accepting_.load(std::memory_order_acquire) &&
            waveInAddBuffer(static_cast<HWAVEIN>(handle_), header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
    {
        counters_.dropped_blocks.fetch_add(1U, std::memory_order_relaxed);
    }
}

void WinmmCapture::wait_callbacks_idle() noexcept
{
    if (callbacks_idle_event_ != nullptr)
    {
        static_cast<void>(WaitForSingleObject(callbacks_idle_event_, 5000U));
    }
}

WinmmCounters WinmmCapture::counters() const noexcept
{
    return
    {
        counters_.input_buffers.load(std::memory_order_acquire),
        counters_.partial_buffers.load(std::memory_order_acquire),
        counters_.reset_buffers.load(std::memory_order_acquire),
        counters_.reset_partial_buffers.load(std::memory_order_acquire),
        counters_.reset_samples.load(std::memory_order_acquire),
        counters_.dropped_blocks.load(std::memory_order_acquire),
        counters_.samples_delivered.load(std::memory_order_acquire),
        0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
    };
}

} // namespace waveform_capture

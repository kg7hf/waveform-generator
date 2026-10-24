// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "host/capture/winmm_audio.hpp"

#include "host/capture/capture_core.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <new>

#define WIN32_LEAN_AND_MEAN
#include <mmreg.h>
#include <mmsystem.h>
#include <windows.h>

namespace waveform_capture
{

struct WinmmCapture::PlatformState
{
    static constexpr std::size_t buffer_count = 16U;
    std::array<WAVEHDR, buffer_count> headers{};
    std::array<std::array<std::int16_t, capture_block_samples * 2U>, buffer_count> samples{};
    std::array<std::array<float, capture_block_samples>, buffer_count> converted{};
    struct Completion
    {
        void* header{};
        bool reset_buffer{};
    };
    std::array<Completion, buffer_count> completed_headers{};
    std::atomic<std::uint64_t> completed_head{};
    std::atomic<std::uint64_t> completed_tail{};
};

namespace
{

// open()'s and start()'s refusals.
constexpr m110::Status invalid_arguments{.code = m110::StatusCode::invalid_argument, .message = "invalid WinMM capture arguments"};
constexpr m110::Status identity_changed{.code = m110::StatusCode::unavailable, .message = "WinMM input identity changed or does not match exactly"};
constexpr m110::Status no_buffers{.code = m110::StatusCode::unavailable, .message = "cannot allocate WinMM capture buffers"};
constexpr m110::Status format_refused{.code = m110::StatusCode::invalid_configuration, .message = "WinMM input does not accept requested PCM16 stereo 48 kHz format"};
constexpr m110::Status open_failed{.code = m110::StatusCode::io_error, .message = "waveInOpen failed for requested PCM16 stereo 48 kHz"};
constexpr m110::Status no_event{.code = m110::StatusCode::unavailable, .message = "cannot create WinMM callback synchronization event"};
constexpr m110::Status no_worker{.code = m110::StatusCode::unavailable, .message = "cannot start WinMM capture worker"};
constexpr m110::Status buffers_refused{.code = m110::StatusCode::io_error, .message = "WinMM input buffer preparation failed"};
constexpr m110::Status not_startable{.code = m110::StatusCode::unavailable, .message = "WinMM capture is not startable"};
constexpr m110::Status start_failed{.code = m110::StatusCode::io_error, .message = "waveInStart failed"};
constexpr m110::Status stop_failed{.code = m110::StatusCode::io_error, .message = "waveInStop failed during shutdown"};
constexpr m110::Status reset_failed{.code = m110::StatusCode::io_error, .message = "waveInReset failed during shutdown"};
constexpr m110::Status callback_event_failed{.code = m110::StatusCode::io_error, .message = "ResetEvent failed during WinMM shutdown"};
constexpr m110::Status callback_wait_failed{.code = m110::StatusCode::io_error, .message = "failed or timed out waiting for WinMM callbacks to finish"};
constexpr m110::Status worker_wait_failed{.code = m110::StatusCode::io_error, .message = "failed or timed out waiting for WinMM capture worker"};
constexpr m110::Status unprepare_failed{.code = m110::StatusCode::io_error, .message = "waveInUnprepareHeader failed during shutdown"};
constexpr m110::Status close_failed{.code = m110::StatusCode::io_error, .message = "waveInClose failed during shutdown"};
constexpr m110::Status callback_failed{.code = m110::StatusCode::io_error, .message = "WinMM capture dropped a sample block"};

} // namespace

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

m110::Status WinmmCapture::open(std::uint32_t device_index, std::string_view expected_name, SamplesCallback callback, void* context, ChannelPolicy channel_policy) noexcept
{
    if (handle_ != nullptr || callback == nullptr || device_index >= driver_.get_num_devs() || expected_name.empty())
    {
        return invalid_arguments;
    }

    // A closed capture retains its final counters for inspection. A new open
    // begins a separate session; no driver callback can still own this object.
    counters_.input_buffers.store(0U, std::memory_order_relaxed);
    counters_.partial_buffers.store(0U, std::memory_order_relaxed);
    counters_.reset_buffers.store(0U, std::memory_order_relaxed);
    counters_.reset_partial_buffers.store(0U, std::memory_order_relaxed);
    counters_.reset_samples.store(0U, std::memory_order_relaxed);
    counters_.dropped_blocks.store(0U, std::memory_order_relaxed);
    counters_.samples_delivered.store(0U, std::memory_order_relaxed);

    WAVEINCAPSA capabilities{};

    if (driver_.get_dev_caps(device_index, &capabilities, sizeof(capabilities)) != MMSYSERR_NOERROR || expected_name != std::string_view{capabilities.szPname})
    {
        return identity_changed;
    }

    platform_ = new (std::nothrow) PlatformState{};

    if (platform_ == nullptr)
    {
        return no_buffers;
    }

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2U;
    format.nSamplesPerSec = 48000U;
    format.wBitsPerSample = 16U;
    format.nBlockAlign = static_cast<WORD>(2U * sizeof(std::int16_t));
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    if (driver_.open(nullptr, device_index, &format, 0U, 0U, WAVE_FORMAT_QUERY) != MMSYSERR_NOERROR)
    {
        delete platform_;
        platform_ = nullptr;
        return format_refused;
    }

    HWAVEIN handle{};

    if (driver_.open(&handle, device_index, &format, reinterpret_cast<DWORD_PTR>(&WinmmCapture::input_callback), reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION) != MMSYSERR_NOERROR)
    {
        delete platform_;
        platform_ = nullptr;
        return open_failed;
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
        close();
        return no_event;
    }

    worker_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (worker_event_ == nullptr)
    {
        close();
        return no_event;
    }

    for (auto& header : platform_->headers)
    {
        const auto index = static_cast<std::size_t>(&header - platform_->headers.data());
        header.lpData = reinterpret_cast<LPSTR>(platform_->samples[index].data());
        header.dwBufferLength = static_cast<DWORD>(capture_block_samples * 2U * sizeof(std::int16_t));

        if (driver_.prepare_header(static_cast<HWAVEIN>(handle_), &header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
        {
            close();
            return buffers_refused;
        }

        ++prepared_headers_;

        if (driver_.add_buffer(static_cast<HWAVEIN>(handle_), &header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
        {
            close();
            return buffers_refused;
        }
    }

    return m110::Status::success();
}

m110::Status WinmmCapture::start() noexcept
{
    if (!capture_start_allowed(handle_ != nullptr, stopped_.load(std::memory_order_acquire), accepting_.load(std::memory_order_acquire), worker_thread_ != nullptr))
    {
        return not_startable;
    }

    stopped_.store(false, std::memory_order_release);
    resetting_.store(false, std::memory_order_release);
    worker_stop_.store(false, std::memory_order_release);
    worker_thread_ = CreateThread(nullptr, 0U, &WinmmCapture::worker_entry, this, 0U, nullptr);

    if (worker_thread_ == nullptr)
    {
        return no_worker;
    }

    accepting_.store(true, std::memory_order_release);

    if (driver_.start(static_cast<HWAVEIN>(handle_)) != MMSYSERR_NOERROR)
    {
        accepting_.store(false, std::memory_order_release);
        return start_failed;
    }

    driver_started_ = true;

    return m110::Status::success();
}

void WinmmCapture::stop() noexcept
{
    if (handle_ == nullptr || stopped_.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }

    {
        // Requeue and reset must not race over ownership of a driver header.
        const std::lock_guard lock{driver_mutex_};
        accepting_.store(false, std::memory_order_release);
        resetting_.store(true, std::memory_order_release);

        if (callbacks_idle_event_ != nullptr && driver_.reset_event(callbacks_idle_event_) == FALSE)
        {
            m110::critical_error(callback_event_failed);
        }

        if (driver_started_ && driver_.stop(static_cast<HWAVEIN>(handle_)) != MMSYSERR_NOERROR)
        {
            m110::critical_error(stop_failed);
        }

        if (driver_.reset(static_cast<HWAVEIN>(handle_)) != MMSYSERR_NOERROR)
        {
            m110::critical_error(reset_failed);
        }
        driver_started_ = false;
    }

    // A reset with no active callback has nobody left to signal the event.
    // This also covers callbacks that completed before waveInReset returned.
    if (callbacks_idle_event_ != nullptr && callbacks_active_.load(std::memory_order_acquire) == 0U && SetEvent(callbacks_idle_event_) == FALSE)
    {
        m110::critical_error(callback_event_failed);
    }

    wait_callbacks_idle();
    join_worker();
    resetting_.store(false, std::memory_order_release);
}

void WinmmCapture::close() noexcept
{
    if (handle_ == nullptr)
    {
        prepared_headers_ = 0U;
        delete platform_;
        platform_ = nullptr;

        if (callbacks_idle_event_ != nullptr)
        {
            static_cast<void>(CloseHandle(callbacks_idle_event_));
            callbacks_idle_event_ = nullptr;
        }
        if (worker_event_ != nullptr)
        {
            static_cast<void>(CloseHandle(worker_event_));
            worker_event_ = nullptr;
        }

        return;
    }

    stop();

    const m110::Status headers_unprepared =
        unprepare_prepared_prefix(std::span<WAVEHDR>{platform_->headers}, prepared_headers_,
                                  [this, handle = static_cast<HWAVEIN>(handle_)](WAVEHDR& header) noexcept { return driver_.unprepare_header(handle, &header, sizeof(WAVEHDR)) == MMSYSERR_NOERROR; });

    if (!headers_unprepared.is_ok())
    {
        m110::critical_error(unprepare_failed);
    }

    prepared_headers_ = 0U;

    if (driver_.close(static_cast<HWAVEIN>(handle_)) != MMSYSERR_NOERROR)
    {
        m110::critical_error(close_failed);
    }

    handle_ = nullptr;

    if (callbacks_idle_event_ != nullptr)
    {
        static_cast<void>(CloseHandle(callbacks_idle_event_));
        callbacks_idle_event_ = nullptr;
    }
    if (worker_event_ != nullptr)
    {
        static_cast<void>(CloseHandle(worker_event_));
        worker_event_ = nullptr;
    }

    delete platform_;
    platform_ = nullptr;
}

WinmmCapture::~WinmmCapture() noexcept
{
    close();
}

void CALLBACK WinmmCapture::input_callback(void*, unsigned int message, std::uintptr_t instance, std::uintptr_t parameter, std::uintptr_t) noexcept
{
    if (message == WIM_DATA && instance != 0U && parameter != 0U)
    {
        // open() handed waveInOpen `this` as the callback instance; this reverses that cast.
        auto* capture = reinterpret_cast<WinmmCapture*>(instance);
        capture->callbacks_active_.fetch_add(1U, std::memory_order_acq_rel);
        if (capture->completion_callback_active_.test_and_set(std::memory_order_acquire))
        {
            // The completed-header ring has one producer at a time.
            latch_capture_failure(capture->accepting_, capture->counters_.dropped_blocks);
        }
        else
        {
            capture->enqueue_completed(reinterpret_cast<void*>(parameter));
            capture->completion_callback_active_.clear(std::memory_order_release);
        }

        if (capture->callbacks_active_.fetch_sub(1U, std::memory_order_acq_rel) == 1U && capture->callbacks_idle_event_ != nullptr)
        {
            static_cast<void>(SetEvent(capture->callbacks_idle_event_));
        }
    }
}

void WinmmCapture::enqueue_completed(void* header) noexcept
{
    if (platform_ == nullptr)
    {
        return;
    }

    const auto head = platform_->completed_head.load(std::memory_order_relaxed);
    const auto tail = platform_->completed_tail.load(std::memory_order_acquire);
    if (head - tail >= PlatformState::buffer_count)
    {
        latch_capture_failure(accepting_, counters_.dropped_blocks);
        return;
    }

    platform_->completed_headers[static_cast<std::size_t>(head % PlatformState::buffer_count)] = {header, resetting_.load(std::memory_order_acquire)};
    platform_->completed_head.store(head + 1U, std::memory_order_release);
    if (worker_event_ != nullptr && driver_.set_event(worker_event_) == FALSE)
    {
        latch_capture_failure(accepting_, counters_.dropped_blocks);
    }
}

DWORD WINAPI WinmmCapture::worker_entry(void* context) noexcept
{
    static_cast<WinmmCapture*>(context)->run_worker();
    return 0U;
}

void WinmmCapture::run_worker() noexcept
{
    for (;;)
    {
        const auto tail = platform_->completed_tail.load(std::memory_order_relaxed);
        const auto head = platform_->completed_head.load(std::memory_order_acquire);
        if (tail != head)
        {
            const auto completion = platform_->completed_headers[static_cast<std::size_t>(tail % PlatformState::buffer_count)];
            platform_->completed_tail.store(tail + 1U, std::memory_order_release);
            complete_buffer(completion.header, completion.reset_buffer);
            continue;
        }

        if (worker_stop_.load(std::memory_order_acquire))
        {
            return;
        }

        if (WaitForSingleObject(worker_event_, INFINITE) != WAIT_OBJECT_0)
        {
            m110::critical_error(worker_wait_failed);
        }
    }
}

void WinmmCapture::complete_buffer(void* raw_header, bool reset_buffer) noexcept
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
        downmix_pcm16_stereo_to_float(std::span<const std::int16_t>{platform_->samples[index].data(), count * 2U}, std::span<float>{platform_->converted[index].data(), count}, channel_policy_);
        // The block just converted: buffer `index` of converted (index < buffer_count, found above).
        const std::span<const float> mono{std::span{platform_->converted}.subspan(index).front().data(), count};

        if (!callback_(context_, mono).is_ok())
        {
            latch_capture_failure(accepting_, counters_.dropped_blocks);
        }
    }

    header->dwBytesRecorded = 0U;

    {
        const std::lock_guard lock{driver_mutex_};
        if (accepting_.load(std::memory_order_acquire) && driver_.add_buffer(static_cast<HWAVEIN>(handle_), header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
        {
            latch_capture_failure(accepting_, counters_.dropped_blocks);
        }
    }
}

void WinmmCapture::join_worker() noexcept
{
    if (worker_thread_ == nullptr)
    {
        return;
    }

    worker_stop_.store(true, std::memory_order_release);
    if (SetEvent(worker_event_) == FALSE || WaitForSingleObject(worker_thread_, 5000U) != WAIT_OBJECT_0)
    {
        m110::critical_error(worker_wait_failed);
    }
    static_cast<void>(CloseHandle(worker_thread_));
    worker_thread_ = nullptr;
}

void WinmmCapture::wait_callbacks_idle() noexcept
{
    if (callbacks_idle_event_ != nullptr)
    {
        const auto result = driver_.wait(callbacks_idle_event_, 5000U);

        if (result != WAIT_OBJECT_0)
        {
            m110::critical_error(callback_wait_failed);
        }
    }
}

bool WinmmCapture::failed() const noexcept
{
    return counters_.dropped_blocks.load(std::memory_order_acquire) != 0U;
}

m110::Status WinmmCapture::status() const noexcept
{
    return failed() ? callback_failed : m110::Status::success();
}

WinmmCounters WinmmCapture::counters() const noexcept
{
    return {
        counters_.input_buffers.load(std::memory_order_acquire),
        counters_.partial_buffers.load(std::memory_order_acquire),
        counters_.reset_buffers.load(std::memory_order_acquire),
        counters_.reset_partial_buffers.load(std::memory_order_acquire),
        counters_.reset_samples.load(std::memory_order_acquire),
        counters_.dropped_blocks.load(std::memory_order_acquire),
        counters_.samples_delivered.load(std::memory_order_acquire),
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
    };
}

} // namespace waveform_capture

// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "host/capture/winmm_audio.hpp"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

namespace
{

struct FakeWinmm;
FakeWinmm* active_driver{};
thread_local bool inside_native_callback{};

UINT WINAPI get_num_devs() noexcept;
MMRESULT WINAPI get_dev_caps(UINT_PTR device, LPWAVEINCAPSA caps, UINT size) noexcept;
MMRESULT WINAPI open(HWAVEIN* handle, UINT device, LPCWAVEFORMATEX format, DWORD_PTR callback, DWORD_PTR instance, DWORD flags) noexcept;
MMRESULT WINAPI prepare_header(HWAVEIN handle, LPWAVEHDR header, UINT size) noexcept;
MMRESULT WINAPI add_buffer(HWAVEIN handle, LPWAVEHDR header, UINT size) noexcept;
MMRESULT WINAPI start(HWAVEIN handle) noexcept;
MMRESULT WINAPI stop(HWAVEIN handle) noexcept;
MMRESULT WINAPI reset(HWAVEIN handle) noexcept;
MMRESULT WINAPI unprepare_header(HWAVEIN handle, LPWAVEHDR header, UINT size) noexcept;
MMRESULT WINAPI close(HWAVEIN handle) noexcept;
BOOL WINAPI set_event(HANDLE event) noexcept;

struct FakeWinmm
{
    using InputCallback = void(CALLBACK*)(void*, unsigned int, std::uintptr_t, std::uintptr_t, std::uintptr_t);
    waveform_capture::WinmmCapture::DriverApi api{};
    std::array<LPWAVEHDR, 16U> headers{};
    std::array<std::atomic<bool>, 16U> queued{};
    InputCallback callback{};
    DWORD_PTR callback_instance{};
    std::size_t prepared{};
    std::atomic<std::size_t> added{};
    std::atomic<std::size_t> unprepared{};
    std::atomic<std::size_t> closed{};
    std::atomic<bool> requeued_inside_callback{};
    std::size_t prepare_failure_at{};
    std::size_t add_failure_at{};
    bool stop_failure{};
    bool start_failure{};
    bool reset_failure{};
    bool unprepare_failure{};
    bool close_failure{};
    bool wait_failure{};
    bool signal_failure{};
    std::atomic<bool>* reset_returned{};
    std::atomic<bool>* callback_signal_entered{};
    std::atomic<bool>* release_callback_signal{};

    FakeWinmm() noexcept
    {
        active_driver = this;
        api.get_num_devs = &get_num_devs;
        api.get_dev_caps = &get_dev_caps;
        api.open = &open;
        api.prepare_header = &prepare_header;
        api.add_buffer = &add_buffer;
        api.start = &start;
        api.stop = &stop;
        api.reset = &reset;
        api.unprepare_header = &unprepare_header;
        api.close = &close;
        api.set_event = &set_event;
        api.wait = [](HANDLE event, DWORD timeout) noexcept -> DWORD { return active_driver->wait_failure ? WAIT_TIMEOUT : WaitForSingleObject(event, timeout); };
    }

    ~FakeWinmm() noexcept { active_driver = nullptr; }
    FakeWinmm(const FakeWinmm&) = delete;
    FakeWinmm& operator=(const FakeWinmm&) = delete;
    FakeWinmm(FakeWinmm&&) = delete;
    FakeWinmm& operator=(FakeWinmm&&) = delete;

    void deliver(std::size_t index, DWORD bytes_recorded) noexcept
    {
        queued[index] = false;
        headers[index]->dwBytesRecorded = bytes_recorded;
        inside_native_callback = true;
        callback(reinterpret_cast<HWAVEIN>(this), WIM_DATA, callback_instance, reinterpret_cast<DWORD_PTR>(headers[index]), 0U);
        inside_native_callback = false;
    }
};

UINT WINAPI get_num_devs() noexcept
{
    return 1U;
}

MMRESULT WINAPI get_dev_caps(UINT_PTR, LPWAVEINCAPSA caps, UINT size) noexcept
{
    if (size != sizeof(WAVEINCAPSA))
    {
        return MMSYSERR_INVALPARAM;
    }

    constexpr char name[] = "Fake input";
    std::memcpy(caps->szPname, name, sizeof(name));
    caps->wChannels = 2U;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI open(HWAVEIN* handle, UINT, LPCWAVEFORMATEX, DWORD_PTR callback, DWORD_PTR instance, DWORD flags) noexcept
{
    if (flags == WAVE_FORMAT_QUERY)
    {
        return MMSYSERR_NOERROR;
    }

    if (flags != CALLBACK_FUNCTION || handle == nullptr)
    {
        return MMSYSERR_INVALPARAM;
    }

    active_driver->callback = reinterpret_cast<FakeWinmm::InputCallback>(callback);
    active_driver->callback_instance = instance;
    *handle = reinterpret_cast<HWAVEIN>(active_driver);
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI prepare_header(HWAVEIN, LPWAVEHDR header, UINT) noexcept
{
    auto& fake = *active_driver;
    ++fake.prepared;
    if (fake.prepared == fake.prepare_failure_at)
    {
        return MMSYSERR_ERROR;
    }

    fake.headers[fake.prepared - 1U] = header;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI add_buffer(HWAVEIN, LPWAVEHDR header, UINT) noexcept
{
    auto& fake = *active_driver;
    if (inside_native_callback)
    {
        fake.requeued_inside_callback.store(true, std::memory_order_release);
    }
    const auto added = fake.added.fetch_add(1U, std::memory_order_relaxed) + 1U;
    if (added == fake.add_failure_at)
    {
        return MMSYSERR_ERROR;
    }

    for (std::size_t index = 0U; index < fake.headers.size(); ++index)
    {
        if (fake.headers[index] == header)
        {
            fake.queued[index] = true;
            return MMSYSERR_NOERROR;
        }
    }

    return MMSYSERR_INVALPARAM;
}

MMRESULT WINAPI start(HWAVEIN) noexcept
{
    return active_driver->start_failure ? MMSYSERR_ERROR : MMSYSERR_NOERROR;
}
MMRESULT WINAPI stop(HWAVEIN) noexcept
{
    return active_driver->stop_failure ? MMSYSERR_ERROR : MMSYSERR_NOERROR;
}

MMRESULT WINAPI reset(HWAVEIN) noexcept
{
    auto& fake = *active_driver;
    if (fake.reset_failure)
    {
        return MMSYSERR_ERROR;
    }

    for (std::size_t index = 0U; index < fake.queued.size(); ++index)
    {
        if (fake.queued[index])
        {
            fake.deliver(index, 0U);
        }
    }

    if (fake.reset_returned != nullptr)
    {
        fake.reset_returned->store(true, std::memory_order_release);
    }

    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI unprepare_header(HWAVEIN, LPWAVEHDR header, UINT) noexcept
{
    auto& fake = *active_driver;
    if (fake.unprepare_failure)
    {
        return MMSYSERR_ERROR;
    }

    for (std::size_t index = 0U; index < fake.headers.size(); ++index)
    {
        if (fake.headers[index] == header)
        {
            if (fake.queued[index])
            {
                return WAVERR_STILLPLAYING;
            }

            ++fake.unprepared;
            return MMSYSERR_NOERROR;
        }
    }

    return MMSYSERR_INVALPARAM;
}

MMRESULT WINAPI close(HWAVEIN) noexcept
{
    ++active_driver->closed;
    return active_driver->close_failure ? MMSYSERR_ERROR : MMSYSERR_NOERROR;
}

BOOL WINAPI set_event(HANDLE event) noexcept
{
    auto& fake = *active_driver;
    if (inside_native_callback && fake.callback_signal_entered != nullptr)
    {
        fake.callback_signal_entered->store(true, std::memory_order_release);
        while (!fake.release_callback_signal->load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
    }
    if (inside_native_callback && fake.signal_failure)
    {
        return FALSE;
    }
    return SetEvent(event);
}

struct Sink
{
    std::atomic<std::size_t> calls{};
    std::atomic<std::size_t> frames{};
    std::array<float, 2U> first_samples{};
    bool refuse{};
    std::atomic<bool>* entered{};
    std::atomic<bool>* release{};
    std::atomic<bool>* called_inside_native_callback{};
};

m110::Status samples_callback(void* context, std::span<const float> samples) noexcept
{
    auto& sink = *static_cast<Sink*>(context);
    const auto call = sink.calls.fetch_add(1U, std::memory_order_relaxed);
    if (call < sink.first_samples.size() && !samples.empty())
    {
        sink.first_samples[call] = samples.front();
    }
    if (sink.called_inside_native_callback != nullptr && inside_native_callback)
    {
        sink.called_inside_native_callback->store(true, std::memory_order_release);
    }
    sink.frames.fetch_add(samples.size(), std::memory_order_release);
    if (sink.entered != nullptr)
    {
        sink.entered->store(true, std::memory_order_release);
        while (!sink.release->load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
    }
    return sink.refuse ? m110::Status{.code = m110::StatusCode::io_error, .message = "test refusal"} : m110::Status::success();
}

[[nodiscard]] bool wait_for(const std::atomic<bool>& flag) noexcept
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!flag.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return flag.load(std::memory_order_acquire);
}

[[nodiscard]] bool wait_for_count(const std::atomic<std::size_t>& count, std::size_t minimum) noexcept
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (count.load(std::memory_order_acquire) < minimum && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return count.load(std::memory_order_acquire) >= minimum;
}

[[nodiscard]] bool wait_for_failure(const waveform_capture::WinmmCapture& capture) noexcept
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!capture.failed() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return capture.failed();
}

enum class FatalFault : std::uint8_t
{
    stop,
    reset,
    wait,
    unprepare,
    close
};

FatalFault expected_fault{};

void fatal_handler(m110::Status status, m110::CriticalSite) noexcept
{
    const auto& fake = *active_driver;
    const bool before_release = fake.unprepared == 0U && fake.closed == 0U;
    bool expected = false;

    switch (expected_fault)
    {
    case FatalFault::stop:
        expected = std::strcmp(status.message, "waveInStop failed during shutdown") == 0 && before_release;
        break;
    case FatalFault::reset:
        expected = std::strcmp(status.message, "waveInReset failed during shutdown") == 0 && before_release;
        break;
    case FatalFault::wait:
        expected = std::strcmp(status.message, "failed or timed out waiting for WinMM callbacks to finish") == 0 && before_release;
        break;
    case FatalFault::unprepare:
        expected = std::strcmp(status.message, "waveInUnprepareHeader failed during shutdown") == 0 && before_release;
        break;
    case FatalFault::close:
        expected = std::strcmp(status.message, "waveInClose failed during shutdown") == 0 && fake.unprepared == 16U && fake.closed == 1U;
        break;
    }

    std::_Exit(status.code == m110::StatusCode::io_error && expected ? 42 : 43);
}

void run_fatal_child(FatalFault fault)
{
    FakeWinmm fake;
    expected_fault = fault;
    fake.stop_failure = fault == FatalFault::stop;
    fake.reset_failure = fault == FatalFault::reset;
    fake.wait_failure = fault == FatalFault::wait;
    fake.unprepare_failure = fault == FatalFault::unprepare;
    fake.close_failure = fault == FatalFault::close;
    waveform_capture::WinmmCapture capture{fake.api};
    Sink sink{};
    REQUIRE(capture.open(0U, "Fake input", &samples_callback, &sink, waveform_capture::ChannelPolicy::left).is_ok());
    REQUIRE(capture.start().is_ok());
    m110::set_critical_error_handler(&fatal_handler);
    capture.close();
    FAIL("driver failure was ignored");
}

[[nodiscard]] bool child_exits_at_critical(std::wstring_view test_name)
{
    std::array<wchar_t, MAX_PATH> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0U || length >= path.size())
    {
        return false;
    }

    std::wstring command = L"\"" + std::wstring{path.data(), length} + L"\" \"" + std::wstring{test_name} + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(path.data(), command.data(), nullptr, nullptr, FALSE, 0U, nullptr, nullptr, &startup, &process) == FALSE)
    {
        return false;
    }

    const DWORD waited = WaitForSingleObject(process.hProcess, 10000U);
    DWORD exit_code{};
    if (waited != WAIT_OBJECT_0)
    {
        static_cast<void>(TerminateProcess(process.hProcess, 44U));
    }
    const bool exited = waited == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &exit_code) != FALSE && exit_code == 42U;
    static_cast<void>(CloseHandle(process.hThread));
    static_cast<void>(CloseHandle(process.hProcess));
    return exited;
}

} // namespace

TEST_CASE("WinMM adapter drains samples and closes all prepared buffers", "[winmm][capture_adapters]")
{
    FakeWinmm fake;
    waveform_capture::WinmmCapture capture{fake.api};
    Sink sink{};
    std::atomic<bool> delivered_inside_callback{};
    sink.called_inside_native_callback = &delivered_inside_callback;

    REQUIRE(capture.open(0U, "Fake input", &samples_callback, &sink, waveform_capture::ChannelPolicy::left).is_ok());
    REQUIRE(capture.start().is_ok());
    fake.deliver(0U, 4U);
    REQUIRE(wait_for_count(sink.frames, 1U));
    REQUIRE(wait_for_count(fake.added, 17U));
    REQUIRE(sink.calls == 1U);
    REQUIRE(sink.frames == 1U);
    REQUIRE_FALSE(delivered_inside_callback.load(std::memory_order_acquire));
    REQUIRE_FALSE(fake.requeued_inside_callback.load(std::memory_order_acquire));
    REQUIRE_FALSE(capture.failed());
    capture.close();
    REQUIRE_FALSE(capture.is_open());
    REQUIRE(fake.unprepared == 16U);
    REQUIRE(fake.closed == 1U);
}

TEST_CASE("WinMM refuses restart after stop returns all driver buffers", "[winmm][capture_adapters]")
{
    FakeWinmm fake;
    waveform_capture::WinmmCapture capture{fake.api};
    Sink sink{};

    REQUIRE(capture.open(0U, "Fake input", &samples_callback, &sink, waveform_capture::ChannelPolicy::left).is_ok());
    REQUIRE(capture.start().is_ok());
    capture.stop();
    REQUIRE_FALSE(capture.is_running());

    for (const auto& queued : fake.queued)
    {
        REQUIRE_FALSE(queued.load(std::memory_order_acquire));
    }

    REQUIRE_FALSE(capture.start().is_ok());
    REQUIRE_FALSE(capture.is_running());
    capture.close();
}

TEST_CASE("WinMM start refusal still drains buffers and joins its worker", "[winmm][capture_adapters]")
{
    FakeWinmm fake;
    fake.start_failure = true;
    waveform_capture::WinmmCapture capture{fake.api};
    Sink sink{};

    REQUIRE(capture.open(0U, "Fake input", &samples_callback, &sink, waveform_capture::ChannelPolicy::left).is_ok());
    REQUIRE(capture.start().code == m110::StatusCode::io_error);
    REQUIRE_FALSE(capture.is_running());
    capture.close();
    REQUIRE_FALSE(capture.is_open());
    REQUIRE(fake.unprepared == 16U);
    REQUIRE(fake.closed == 1U);
}

TEST_CASE("WinMM shutdown waits for a worker still processing after reset", "[winmm][capture_adapters]")
{
    FakeWinmm fake;
    waveform_capture::WinmmCapture capture{fake.api};
    std::atomic<bool> entered{};
    std::atomic<bool> release{};
    std::atomic<bool> reset_returned{};
    std::atomic<bool> close_finished{};
    fake.reset_returned = &reset_returned;
    Sink sink{.entered = &entered, .release = &release};

    REQUIRE(capture.open(0U, "Fake input", &samples_callback, &sink, waveform_capture::ChannelPolicy::left).is_ok());
    REQUIRE(capture.start().is_ok());
    std::thread callback_thread{[&fake]() { fake.deliver(0U, 4U); }};
    const bool callback_started = wait_for(entered);
    if (callback_started)
    {
        fake.deliver(1U, 4U);
    }
    std::thread close_thread{[&capture, &close_finished]()
                             {
                                 capture.close();
                                 close_finished.store(true, std::memory_order_release);
                             }};
    const bool reset_completed = wait_for(reset_returned);
    const bool retained = !close_finished.load(std::memory_order_acquire) && fake.unprepared.load(std::memory_order_acquire) == 0U;
    release.store(true, std::memory_order_release);
    callback_thread.join();
    close_thread.join();

    REQUIRE(callback_started);
    REQUIRE(reset_completed);
    REQUIRE(retained);
    REQUIRE(close_finished.load(std::memory_order_acquire));
    REQUIRE(sink.frames == 2U);
    REQUIRE(capture.counters().reset_samples == 0U);
    REQUIRE(capture.counters().samples_delivered == 2U);
    REQUIRE(fake.unprepared == 16U);
}

TEST_CASE("WinMM completions queue behind a busy worker", "[winmm][capture_adapters]")
{
    FakeWinmm fake;
    waveform_capture::WinmmCapture capture{fake.api};
    std::atomic<bool> entered{};
    std::atomic<bool> release{};
    Sink sink{.entered = &entered, .release = &release};

    REQUIRE(capture.open(0U, "Fake input", &samples_callback, &sink, waveform_capture::ChannelPolicy::left).is_ok());
    REQUIRE(capture.start().is_ok());
    auto* first_pcm = reinterpret_cast<std::int16_t*>(fake.headers[0U]->lpData);
    auto* second_pcm = reinterpret_cast<std::int16_t*>(fake.headers[1U]->lpData);
    first_pcm[0U] = 1000;
    second_pcm[0U] = -1000;
    fake.deliver(0U, 4U);
    const bool first_entered = wait_for(entered);
    if (first_entered)
    {
        fake.deliver(1U, 4U);
        CHECK(sink.calls == 1U);
        release.store(true, std::memory_order_release);
    }
    else
    {
        release.store(true, std::memory_order_release);
    }
    REQUIRE(first_entered);
    REQUIRE(wait_for_count(sink.frames, 2U));
    REQUIRE(sink.calls == 2U);
    REQUIRE(sink.first_samples[0U] > 0.0F);
    REQUIRE(sink.first_samples[1U] < 0.0F);
    REQUIRE_FALSE(capture.failed());
    capture.close();
}

TEST_CASE("WinMM overlapping native callbacks fail without corrupting the completion ring", "[winmm][capture_adapters]")
{
    FakeWinmm fake;
    waveform_capture::WinmmCapture capture{fake.api};
    Sink sink{};
    std::atomic<bool> callback_signal_entered{};
    std::atomic<bool> release_callback_signal{};
    fake.callback_signal_entered = &callback_signal_entered;
    fake.release_callback_signal = &release_callback_signal;

    REQUIRE(capture.open(0U, "Fake input", &samples_callback, &sink, waveform_capture::ChannelPolicy::left).is_ok());
    REQUIRE(capture.start().is_ok());
    std::thread first_callback{[&fake]() { fake.deliver(0U, 4U); }};
    const bool first_blocked = wait_for(callback_signal_entered);
    if (first_blocked)
    {
        fake.deliver(1U, 4U);
    }
    release_callback_signal.store(true, std::memory_order_release);
    first_callback.join();

    REQUIRE(first_blocked);
    REQUIRE(wait_for_failure(capture));
    REQUIRE(wait_for_count(sink.frames, 1U));
    REQUIRE(sink.calls == 1U);
    REQUIRE(capture.counters().dropped_blocks == 1U);
    REQUIRE_FALSE(fake.requeued_inside_callback.load(std::memory_order_acquire));
    capture.close();
}

TEST_CASE("WinMM signal failure reports capture error and drains on close", "[winmm][capture_adapters]")
{
    FakeWinmm fake;
    fake.signal_failure = true;
    waveform_capture::WinmmCapture capture{fake.api};
    Sink sink{};

    REQUIRE(capture.open(0U, "Fake input", &samples_callback, &sink, waveform_capture::ChannelPolicy::left).is_ok());
    REQUIRE(capture.start().is_ok());
    fake.deliver(0U, 4U);
    REQUIRE(capture.failed());
    REQUIRE(capture.status().code == m110::StatusCode::io_error);
    capture.close();
    REQUIRE(fake.unprepared == 16U);
    REQUIRE(fake.closed == 1U);
}

TEST_CASE("WinMM partial open releases only prepared buffers", "[winmm][capture_adapters]")
{
    FakeWinmm fake;
    fake.prepare_failure_at = 3U;
    waveform_capture::WinmmCapture capture{fake.api};
    Sink sink{};

    REQUIRE(capture.open(0U, "Fake input", &samples_callback, &sink, waveform_capture::ChannelPolicy::left).code == m110::StatusCode::io_error);
    REQUIRE_FALSE(capture.is_open());
    REQUIRE(fake.unprepared == 2U);
    REQUIRE(fake.closed == 1U);
}

TEST_CASE("WinMM add-buffer refusal during open releases the prepared prefix", "[winmm][capture_adapters]")
{
    FakeWinmm fake;
    fake.add_failure_at = 3U;
    waveform_capture::WinmmCapture capture{fake.api};
    Sink sink{};

    REQUIRE(capture.open(0U, "Fake input", &samples_callback, &sink, waveform_capture::ChannelPolicy::left).code == m110::StatusCode::io_error);
    REQUIRE_FALSE(capture.is_open());
    REQUIRE(fake.unprepared == 3U);
    REQUIRE(fake.closed == 1U);
}

TEST_CASE("WinMM callback refusal stops delivery and reports failure", "[winmm][capture_adapters]")
{
    FakeWinmm fake;
    waveform_capture::WinmmCapture capture{fake.api};
    Sink sink{.refuse = true};

    REQUIRE(capture.open(0U, "Fake input", &samples_callback, &sink, waveform_capture::ChannelPolicy::left).is_ok());
    REQUIRE(capture.start().is_ok());
    fake.deliver(0U, 4U);
    REQUIRE(wait_for_failure(capture));
    REQUIRE(capture.status().code == m110::StatusCode::io_error);
    REQUIRE(fake.added == 16U);
    capture.close();
}

TEST_CASE("WinMM close and reopen starts a clean capture session", "[winmm][capture_adapters]")
{
    FakeWinmm first_driver;
    waveform_capture::WinmmCapture capture{first_driver.api};
    Sink refusing_sink{.refuse = true};

    REQUIRE(capture.open(0U, "Fake input", &samples_callback, &refusing_sink, waveform_capture::ChannelPolicy::left).is_ok());
    REQUIRE(capture.start().is_ok());
    first_driver.deliver(0U, 4U);
    REQUIRE(wait_for_failure(capture));
    capture.close();

    FakeWinmm second_driver;
    Sink accepting_sink{};
    REQUIRE(capture.open(0U, "Fake input", &samples_callback, &accepting_sink, waveform_capture::ChannelPolicy::left).is_ok());
    REQUIRE_FALSE(capture.failed());
    REQUIRE(capture.counters().input_buffers == 0U);
    REQUIRE(capture.counters().dropped_blocks == 0U);
    REQUIRE(capture.start().is_ok());
    second_driver.deliver(0U, 4U);
    REQUIRE(wait_for_count(accepting_sink.frames, 1U));
    REQUIRE(accepting_sink.frames == 1U);
    REQUIRE(capture.counters().samples_delivered == 1U);
    capture.close();
}

TEST_CASE("WinMM requeue refusal stops delivery and reports failure", "[winmm][capture_adapters]")
{
    FakeWinmm fake;
    fake.add_failure_at = 17U;
    waveform_capture::WinmmCapture capture{fake.api};
    Sink sink{};

    REQUIRE(capture.open(0U, "Fake input", &samples_callback, &sink, waveform_capture::ChannelPolicy::left).is_ok());
    REQUIRE(capture.start().is_ok());
    fake.deliver(0U, 4U);
    REQUIRE(wait_for_failure(capture));
    REQUIRE(sink.frames == 1U);
    REQUIRE(capture.failed());
    REQUIRE(capture.status().code == m110::StatusCode::io_error);
    capture.close();
}

TEST_CASE("WinMM driver shutdown refusals keep ownership until critical exit", "[winmm][capture_adapters]")
{
    REQUIRE(child_exits_at_critical(L"WinMM fatal stop child"));
    REQUIRE(child_exits_at_critical(L"WinMM fatal reset child"));
    REQUIRE(child_exits_at_critical(L"WinMM fatal wait child"));
    REQUIRE(child_exits_at_critical(L"WinMM fatal unprepare child"));
    REQUIRE(child_exits_at_critical(L"WinMM fatal close child"));
}

TEST_CASE("WinMM fatal stop child", "[.][winmm]")
{
    run_fatal_child(FatalFault::stop);
}
TEST_CASE("WinMM fatal reset child", "[.][winmm]")
{
    run_fatal_child(FatalFault::reset);
}
TEST_CASE("WinMM fatal wait child", "[.][winmm]")
{
    run_fatal_child(FatalFault::wait);
}
TEST_CASE("WinMM fatal unprepare child", "[.][winmm]")
{
    run_fatal_child(FatalFault::unprepare);
}
TEST_CASE("WinMM fatal close child", "[.][winmm]")
{
    run_fatal_child(FatalFault::close);
}

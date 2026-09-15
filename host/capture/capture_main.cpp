// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#include "host/capture/capture_core.hpp"
#include "host/capture/wasapi_audio.hpp"
#include "host/capture/winmm_audio.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace
{

using waveform_capture::BoundedSampleQueue;
using waveform_capture::CaptureMeasurements;
using waveform_capture::ChannelPolicy;
using waveform_capture::SampleBlock;
using waveform_capture::WinmmCapture;
using waveform_capture::WasapiCapture;
using waveform_capture::WinmmCounters;

constexpr std::uint32_t sample_rate_hz = 48000U;
constexpr std::uint32_t bytes_per_sample = sizeof(float);
constexpr std::uint64_t maximum_data_bytes = std::numeric_limits<std::uint32_t>::max() - 36U;

struct CaptureDevice
{
    bool raw{};
    WinmmCapture winmm{};
    WasapiCapture wasapi{};

    bool open(std::uint32_t index, std::string_view name, waveform_capture::SamplesCallback callback,
              void* context, waveform_capture::ChannelPolicy policy) noexcept
    {
        return raw ? wasapi.open(name, callback, context, policy) : winmm.open(index, name, callback, context, policy);
    }
    bool start() noexcept
    {
        return raw ? wasapi.start() : winmm.start();
    }
    void stop() noexcept
    {
        if (raw)
        {
            wasapi.stop();
        }
        else
        {
            winmm.stop();
        }
    }
    void close() noexcept
    {
        if (raw)
        {
            wasapi.close();
        }
        else
        {
            winmm.close();
        }
    }
    [[nodiscard]] bool failed() const noexcept
    {
        return raw ? wasapi.failed() : winmm.failed();
    }
    [[nodiscard]] const char* error() const noexcept
    {
        return raw ? wasapi.error() : winmm.error();
    }
    [[nodiscard]] std::uint32_t device_index() const noexcept
    {
        return raw ? 0U : winmm.device_index();
    }
    [[nodiscard]] std::string_view native_name() const noexcept
    {
        return raw ? wasapi.native_name() : winmm.native_name();
    }
    [[nodiscard]] std::string_view endpoint_id() const noexcept
    {
        return raw ? wasapi.endpoint_id() : winmm.endpoint_id();
    }
    [[nodiscard]] std::string_view input_format() const noexcept
    {
        return raw ? wasapi.format_name() : WinmmCapture::input_format();
    }
    [[nodiscard]] WinmmCounters counters() const noexcept
    {
        return raw ? wasapi.counters() : winmm.counters();
    }
};

enum class CompletionReason : std::uint8_t
{
    stop,
    host_disconnected,
    duration_limit,
    capture_error
};

const char* reason_name(CompletionReason reason) noexcept
{
    switch (reason)
    {
        case CompletionReason::stop:
            return "STOP";

        case CompletionReason::host_disconnected:
            return "HOST_DISCONNECTED";

        case CompletionReason::duration_limit:
            return "DURATION_LIMIT";

        case CompletionReason::capture_error:
            return "CAPTURE_ERROR";
    }

    return "CAPTURE_ERROR";
}

void print_json_string(std::string_view value)
{
    std::putchar('"');

    for (const auto character : value)
    {
        switch (character)
        {
            case '"':
                std::fputs("\\\"", stdout);
                break;

            case '\\':
                std::fputs("\\\\", stdout);
                break;

            case '\n':
                std::fputs("\\n", stdout);
                break;

            case '\r':
                std::fputs("\\r", stdout);
                break;

            case '\t':
                std::fputs("\\t", stdout);
                break;

            default:
                std::putchar(static_cast<unsigned char>(character));
                break;
        }
    }

    std::putchar('"');
}

bool parse_u64(std::string_view text, std::uint64_t& value) noexcept
{
    if (text.empty())
    {
        return false;
    }

    std::uint64_t parsed = 0U;

    for (const auto character : text)
    {
        const auto digit = character >= '0' && character <= '9' ? static_cast<std::uint64_t>(character - '0') : 10U;

        if (digit > 9U || parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U)
        {
            return false;
        }

        parsed = parsed * 10U + digit;
    }

    value = parsed;
    return true;
}

void print_device_json(void*, const waveform_capture::WinmmDevice& device) noexcept
{
    std::printf("{\"index\":%u,\"backend\":\"winmm\",\"native_name\":", device.index);
    print_json_string(device.native_name);
    std::printf(",\"endpoint_id\":null,\"pnp_instance_id\":null,\"max_input_channels\":%u,\"requested_input_format\":\"PCM16_LE_STEREO_48000\",\"conversion\":\"stereo_pcm16_to_mono_float\",\"default_channel\":\"left\"}\n",
                device.max_input_channels);
}

void print_wasapi_device_json(void*, const waveform_capture::WinmmDevice& device) noexcept
{
    std::printf("{\"index\":%u,\"backend\":\"wasapi-raw\",\"native_name\":", device.index);
    print_json_string(device.native_name);
    std::printf(",\"endpoint_id\":null,\"pnp_instance_id\":null,\"max_input_channels\":%u,\"requested_input_format\":\"WASAPI_SHARED_RAW_MIX_FORMAT\",\"conversion\":\"interleaved_to_mono_float\",\"default_channel\":\"left\"}\n",
                device.max_input_channels);
}

struct QueueState
{
    BoundedSampleQueue<waveform_capture::capture_queue_blocks> queue{};
    CaptureMeasurements measurements{};
    HANDLE data_event{};
    std::atomic<bool> first_callback{};
    std::atomic<std::uint64_t> first_callback_ns{};
    std::atomic<std::uint64_t> last_callback_ns{};
    std::atomic<std::uint64_t> maximum_queue_samples{};
};

std::uint64_t monotonic_ns() noexcept
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

bool capture_callback(void* context, std::span<const float> samples) noexcept
{
    auto& state = *static_cast<QueueState*>(context);
    state.measurements.observe(samples);

    if (!state.first_callback.exchange(true, std::memory_order_release))
    {
        state.first_callback_ns.store(monotonic_ns(), std::memory_order_release);
    }

    state.last_callback_ns.store(monotonic_ns(), std::memory_order_release);
    const auto accepted = state.queue.push(samples);

    if (accepted)
    {
        const auto queue_samples = state.queue.size() * waveform_capture::capture_block_samples;
        auto maximum = state.maximum_queue_samples.load(std::memory_order_relaxed);

        while (queue_samples > maximum &&
                !state.maximum_queue_samples.compare_exchange_weak(maximum, queue_samples, std::memory_order_relaxed))
        {
        }

        static_cast<void>(SetEvent(state.data_event));
    }

    return accepted;
}

struct WriterState
{
    QueueState& queue;
    std::string path;
    std::thread thread{};
    std::atomic<bool> producer_done{};
    std::atomic<bool> ready{};
    std::atomic<bool> failed{};
    std::atomic<std::uint64_t> samples_written{};
    std::atomic<std::uint64_t> write_errors{};
};

void write_u16(std::ofstream& output, std::uint16_t value)
{
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void write_u32(std::ofstream& output, std::uint32_t value)
{
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

bool write_header(std::ofstream& output)
{
    write_u32(output, 0x46464952U); // RIFF
    write_u32(output, 36U);
    write_u32(output, 0x45564157U); // WAVE
    write_u32(output, 0x20746d66U); // fmt
    write_u32(output, 16U);
    write_u16(output, 3U); // IEEE float
    write_u16(output, 1U);
    write_u32(output, sample_rate_hz);
    write_u32(output, sample_rate_hz * bytes_per_sample);
    write_u16(output, bytes_per_sample);
    write_u16(output, 32U);
    write_u32(output, 0x61746164U); // data
    write_u32(output, 0U);
    return output.good();
}

void writer_main(WriterState& state) noexcept
{
    std::ofstream output{state.path, std::ios::binary | std::ios::trunc};

    if (!output || !write_header(output))
    {
        state.write_errors.fetch_add(1U, std::memory_order_relaxed);
        state.failed.store(true, std::memory_order_release);
        return;
    }

    state.ready.store(true, std::memory_order_release);
    SampleBlock block{};

    for (;;)
    {
        static_cast<void>(WaitForSingleObject(state.queue.data_event, INFINITE));

        while (state.queue.queue.pop(block))
        {
            output.write(reinterpret_cast<const char*>(block.samples.data()), static_cast<std::streamsize>(block.count * sizeof(float)));

            if (!output)
            {
                state.write_errors.fetch_add(1U, std::memory_order_relaxed);
                state.failed.store(true, std::memory_order_release);
                return;
            }

            state.samples_written.fetch_add(block.count, std::memory_order_relaxed);
        }

        if (state.producer_done.load(std::memory_order_acquire) && state.queue.queue.size() == 0U)
        {
            output.flush();

            if (!output)
            {
                state.write_errors.fetch_add(1U, std::memory_order_relaxed);
                state.failed.store(true, std::memory_order_release);
            }

            return;
        }
    }
}

bool finalize_wav(const std::string& path, std::uint64_t samples) noexcept
{
    const auto data_bytes = samples * bytes_per_sample;

    if (data_bytes > maximum_data_bytes)
    {
        return false;
    }

    std::fstream output{path, std::ios::binary | std::ios::in | std::ios::out};

    if (!output)
    {
        return false;
    }

    const auto riff_size = static_cast<std::uint32_t>(36U + data_bytes);
    const auto data_size = static_cast<std::uint32_t>(data_bytes);
    output.seekp(4);
    output.write(reinterpret_cast<const char*>(&riff_size), sizeof(riff_size));
    output.seekp(40);
    output.write(reinterpret_cast<const char*>(&data_size), sizeof(data_size));
    output.flush();
    return output.good();
}

void print_endpoint(const CaptureDevice& capture)
{
    std::printf("{\"backend\":\"%s\",\"index\":%u,\"native_name\":", capture.raw ? "wasapi-raw" : "winmm", capture.device_index());
    print_json_string(capture.native_name());
    std::fputs(",\"endpoint_id\":", stdout);

    if (capture.endpoint_id().empty())
    {
        std::fputs("null", stdout);
    }
    else
    {
        print_json_string(capture.endpoint_id());
    }

    std::fputs(",\"pnp_instance_id\":null}", stdout);
}

void print_format(const CaptureDevice& capture, ChannelPolicy policy)
{
    std::printf("{\"input_format\":\"%.*s\",\"output_sample_rate_hz\":48000,\"output_channels\":1,\"output_encoding\":\"IEEE_FLOAT32\",\"output_bits_per_sample\":32,\"conversion\":\"interleaved_to_mono_float\",\"channel_policy\":\"%.*s\"}",
                static_cast<int>(capture.input_format().size()), capture.input_format().data(),
                static_cast<int>(waveform_capture::channel_policy_name(policy).size()),
                waveform_capture::channel_policy_name(policy).data());
}

void print_u64_field(const char* name, std::uint64_t value)
{
    std::printf(",\"%s\":%llu", name, static_cast<unsigned long long>(value));
}

void print_double_field(const char* name, double value)
{
    std::printf(",\"%s\":%.9g", name, value);
}

void usage()
{
    std::fputs("usage: waveform_capture [--backend winmm|wasapi-raw] --list-devices [--json]\n"
               "       waveform_capture [--backend winmm|wasapi-raw] --output <capture.wav>\n"
               "         (--device-index <n> | --device-name <exact>) --sample-rate 48000\n"
               "         --channel left|right|average --max-seconds <seconds> [--stop-stdin]\n", stderr);
}

} // namespace

int main(int argc, char** argv)
{
    bool list_devices = false;
    bool json = false;
    bool stop_stdin = false;
    std::string backend_name = "winmm";
    std::string output_path;
    std::string device_name;
    std::uint64_t device_index_value = 0U;
    std::uint64_t max_seconds = 0U;
    ChannelPolicy channel_policy = ChannelPolicy::left;
    bool have_index = false;
    bool have_name = false;
    bool have_max_seconds = false;

    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument{argv[index]};
        auto next = [&](std::string_view & value) noexcept -> bool
        {
            if (index + 1 >= argc)
            {
                return false;
            }

            value = argv[++index];
            return true;
        };
        std::string_view value;

        if (argument == "--list-devices")
        {
            list_devices = true;
        }
        else if (argument == "--json")
        {
            json = true;
        }
        else if (argument == "--stop-stdin")
        {
            stop_stdin = true;
        }
        else if (argument == "--backend" && next(value)) backend_name = std::string{value};
        else if (argument == "--output" && next(value)) output_path = std::string{value};
        else if (argument == "--device-name" && next(value))
        {
            device_name = std::string{value};
            have_name = true;
        }
        else if (argument == "--device-index" && next(value))
        {
            have_index = parse_u64(value, device_index_value);
        }
        else if (argument == "--channel" && next(value))
        {
            if (value == "left")
            {
                channel_policy = ChannelPolicy::left;
            }
            else if (value == "right")
            {
                channel_policy = ChannelPolicy::right;
            }
            else if (value == "average")
            {
                channel_policy = ChannelPolicy::average;
            }
            else
            {
                usage();
                return 2;
            }
        }
        else if (argument == "--sample-rate" && next(value))
        {
            std::uint64_t ignored{};

            if (!parse_u64(value, ignored) || ignored != sample_rate_hz)
            {
                usage();
                return 2;
            }
        }
        else if (argument == "--max-seconds" && next(value))
        {
            have_max_seconds = parse_u64(value, max_seconds);
        }
        else
        {
            usage();
            return 2;
        }
    }

    if (list_devices)
    {
        if (!json && argc != 2)
        {
            usage();
            return 2;
        }

        if (backend_name == "winmm")
        {
            static_cast<void>(WinmmCapture::enumerate(&print_device_json, nullptr));
        }
        else if (backend_name == "wasapi-raw")
        {
            static_cast<void>(WasapiCapture::enumerate(&print_wasapi_device_json, nullptr));
        }
        else
        {
            usage();
            return 2;
        }

        return 0;
    }

    const bool raw_backend = backend_name == "wasapi-raw";

    if (!raw_backend && backend_name != "winmm")
    {
        usage();
        return 2;
    }

    constexpr auto maximum_seconds = maximum_data_bytes / bytes_per_sample / sample_rate_hz;

    if (output_path.empty() || (!have_index && !have_name) || (have_index && have_name) || !have_max_seconds || max_seconds == 0U ||
            max_seconds > maximum_seconds)
    {
        usage();
        return 2;
    }

    std::uint32_t selected_index = 0U;

    if (raw_backend && !have_name)
    {
        std::fprintf(stderr, "--backend wasapi-raw requires --device-name for stable endpoint selection\n");
        return 2;
    }

    if (raw_backend && have_index)
    {
        std::fprintf(stderr, "--backend wasapi-raw does not accept --device-index; use the exact --device-name\n");
        return 2;
    }

    if (have_name && !raw_backend)
    {
        struct NameSelection
        {
            std::string_view requested;
            std::uint32_t matches{};
            std::uint32_t selected{};
        } selection{device_name};
        static_cast<void>(WinmmCapture::enumerate(
                              [](void* context, const waveform_capture::WinmmDevice & device) noexcept
        {
            auto& state = *static_cast<NameSelection*>(context);

            if (state.requested == device.native_name)
            {
                ++state.matches;
                state.selected = device.index;
            }
        },
        &selection));
        selected_index = selection.selected;

        if (selection.matches != 1U)
        {
            std::fprintf(stderr, "requested WinMM input name was not unique: %s (matches=%u)\n", device_name.c_str(), selection.matches);
            return 1;
        }
    }
    else
    {
        if (device_index_value > std::numeric_limits<std::uint32_t>::max())
        {
            usage();
            return 2;
        }

        selected_index = static_cast<std::uint32_t>(device_index_value);
    }

    CaptureDevice capture{raw_backend};
    QueueState queue;
    queue.data_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (queue.data_event == nullptr)
    {
        std::fprintf(stderr, "failed to create capture queue event\n");
        return 1;
    }

    std::string expected_name = device_name;

    if (!have_name && !raw_backend)
    {
        struct IndexSelection
        {
            std::uint32_t requested;
            std::string* name;
        } selection{selected_index, &expected_name};
        static_cast<void>(WinmmCapture::enumerate(
                              [](void* context, const waveform_capture::WinmmDevice & device) noexcept
        {
            auto& state = *static_cast<IndexSelection*>(context);

            if (state.requested == device.index) *state.name = std::string{device.native_name};
        },
        &selection));
    }

    if (expected_name.empty() || !capture.open(selected_index, expected_name, &capture_callback, &queue, channel_policy))
    {
        std::fprintf(stderr, "failed to open %s capture: %s\n", raw_backend ? "WASAPI RAW" : "WinMM", capture.error());
        CloseHandle(queue.data_event);
        return 1;
    }

    WriterState writer{queue, output_path};
    writer.thread = std::thread{writer_main, std::ref(writer)};

    if (!capture.start())
    {
        std::fprintf(stderr, "failed to start %s capture: %s\n", raw_backend ? "WASAPI RAW" : "WinMM", capture.error());
        capture.close();
        writer.producer_done.store(true, std::memory_order_release);
        SetEvent(queue.data_event);
        writer.thread.join();
        CloseHandle(queue.data_event);
        return 1;
    }

    for (std::uint32_t wait = 0U; wait < 500U &&
            (!queue.first_callback.load(std::memory_order_acquire) || !writer.ready.load(std::memory_order_acquire)); ++wait)
    {
        if (writer.failed.load(std::memory_order_acquire) || capture.failed())
        {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    if (!queue.first_callback.load(std::memory_order_acquire) || !writer.ready.load(std::memory_order_acquire))
    {
        std::fprintf(stderr, "%s capture did not produce a first callback\n", raw_backend ? "WASAPI RAW" : "WinMM");
        capture.stop();
        writer.producer_done.store(true, std::memory_order_release);
        SetEvent(queue.data_event);
        writer.thread.join();
        capture.close();
        CloseHandle(queue.data_event);
        return 1;
    }

    const auto capture_id = std::string{"wfg-"} + std::to_string(queue.first_callback_ns.load(std::memory_order_acquire));
    std::printf("{\"record\":\"READY\",\"capture_id\":");
    print_json_string(capture_id);
    std::fputs(",\"endpoint\":", stdout);
    print_endpoint(capture);
    std::fputs(",\"format\":", stdout);
    print_format(capture, channel_policy);
    std::printf(",\"monotonic_ns\":%llu,\"samples_delivered\":%llu,\"samples_written\":%llu,\"queue_capacity_samples\":%zu}\n",
                static_cast<unsigned long long>(monotonic_ns()),
                static_cast<unsigned long long>(capture.counters().samples_delivered),
                static_cast<unsigned long long>(writer.samples_written.load()),
                waveform_capture::capture_queue_blocks * waveform_capture::capture_block_samples);
    std::fflush(stdout);
    struct ReaderState
    {
        std::atomic<bool> requested{};
        std::atomic<CompletionReason> reason{CompletionReason::stop};
    };
    auto reader = std::make_shared<ReaderState>();
    std::thread reader_thread;

    if (stop_stdin)
    {
        reader_thread = std::thread
        {
            [reader]
            {
                std::string line;

                if (!std::getline(std::cin, line))
                {
                    reader->reason.store(CompletionReason::host_disconnected, std::memory_order_release);
                    reader->requested.store(true, std::memory_order_release);
                    return;
                }

                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }

                if (line == "STOP")
                {
                    reader->requested.store(true, std::memory_order_release);
                }
            }};
    }

    CompletionReason reason = CompletionReason::duration_limit;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{max_seconds};

    while (std::chrono::steady_clock::now() < deadline)
    {
        if (reader->requested.load(std::memory_order_acquire))
        {
            reason = reader->reason.load(std::memory_order_acquire);
            break;
        }

        if (writer.failed.load(std::memory_order_acquire))
        {
            reason = CompletionReason::capture_error;
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }

    capture.stop();
    const auto counters = capture.counters();
    writer.producer_done.store(true, std::memory_order_release);
    SetEvent(queue.data_event);
    writer.thread.join();
    capture.close();

    if (!finalize_wav(output_path, writer.samples_written.load(std::memory_order_acquire)))
    {
        reason = CompletionReason::capture_error;
        writer.write_errors.fetch_add(1U, std::memory_order_relaxed);
    }

    if (counters.dropped_blocks != 0U || counters.partial_buffers != 0U ||
            counters.data_discontinuity_packets > counters.startup_discontinuity_packets ||
            queue.measurements.nonfinite_samples != 0U || writer.failed.load(std::memory_order_acquire))
    {
        reason = CompletionReason::capture_error;
    }

    if (reader_thread.joinable())
    {
        reader_thread.detach();
    }

    const auto stopped_ns = monotonic_ns();
    std::printf("{\"record\":\"FINAL\",\"capture_id\":");
    print_json_string(capture_id);
    std::fputs(",\"completion_reason\":", stdout);
    print_json_string(reason_name(reason));
    std::fputs(",\"endpoint\":", stdout);
    print_endpoint(capture);
    std::fputs(",\"format\":", stdout);
    print_format(capture, channel_policy);
    print_u64_field("samples_delivered", counters.samples_delivered);
    print_u64_field("samples_written", writer.samples_written.load());
    print_u64_field("queue_overruns", counters.dropped_blocks);
    print_u64_field("write_errors", writer.write_errors.load());
    print_u64_field("dropped_blocks", counters.dropped_blocks);
    std::fputs(",\"out_of_order\":0", stdout);
    print_u64_field("partial_buffers", counters.partial_buffers);
    print_u64_field("reset_buffers", counters.reset_buffers);
    print_u64_field("reset_partial_buffers", counters.reset_partial_buffers);
    print_u64_field("tail_samples", counters.reset_samples);
    print_u64_field("data_discontinuity_packets", counters.data_discontinuity_packets);
    print_u64_field("startup_discontinuity_packets", counters.startup_discontinuity_packets);
    print_u64_field("silent_packets", counters.silent_packets);
    print_u64_field("position_packets", counters.position_packets);
    print_u64_field("position_errors", counters.position_errors);
    print_u64_field("first_device_position", counters.first_device_position);
    print_u64_field("first_qpc_position", counters.first_qpc_position);
    print_u64_field("last_device_position", counters.last_device_position);
    print_u64_field("last_qpc_position", counters.last_qpc_position);
    std::fputs(",\"maximum_ready_batch\":0", stdout);
    print_u64_field("maximum_queue_samples", queue.maximum_queue_samples.load());
    print_u64_field("clipped_samples", queue.measurements.clipped_samples);
    print_u64_field("first_callback_ns", queue.first_callback_ns.load());
    print_u64_field("last_callback_ns", queue.last_callback_ns.load());
    print_u64_field("stopped_ns", stopped_ns);
    std::fputs(",\"driver_discontinuity_detection\":", stdout);

    if (capture.raw)
    {
        std::fputs("{\"available\":true,\"counter\":\"data_discontinuity_packets\",\"startup_packets_separate\":true}", stdout);
    }
    else
    {
        std::fputs("{\"available\":false,\"reason\":\"WinMM exposes no discontinuity counter through this API\"}", stdout);
    }

    std::fputs(",\"measurements\":{", stdout);
    std::printf("\"peak_linear\":%.9g", static_cast<double>(queue.measurements.peak));
    print_double_field("rms_linear", queue.measurements.rms());
    print_u64_field("nonfinite_samples", queue.measurements.nonfinite_samples);
    print_u64_field("clipped_samples", queue.measurements.clipped_samples);
    std::fputs(",\"peak_dbfs\":", stdout);

    if (std::isfinite(queue.measurements.peak_dbfs()))
    {
        std::printf("%.9g", queue.measurements.peak_dbfs());
    }
    else
    {
        std::fputs("null", stdout);
    }

    std::fputs(",\"rms_dbfs\":", stdout);

    if (std::isfinite(queue.measurements.rms_dbfs()))
    {
        std::printf("%.9g", queue.measurements.rms_dbfs());
    }
    else
    {
        std::fputs("null", stdout);
    }

    std::fputs("}}\n", stdout);
    std::fflush(stdout);
    CloseHandle(queue.data_event);
    return reason == CompletionReason::stop ? 0 : 1;
}

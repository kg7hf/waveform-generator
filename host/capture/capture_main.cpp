// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "common/host_critical_error.hpp"
#include "common/status.hpp"
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
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
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
using waveform_capture::CompletionReason;
using waveform_capture::SampleBlock;
using waveform_capture::WasapiCapture;
using waveform_capture::WinmmCapture;
using waveform_capture::WinmmCounters;

// The exit status of a capture that did not end by STOP, of a device that
// cannot be found or opened, and of a critical error
// (common/host_critical_error.hpp); a refused command line exits 2.
constexpr int exit_failure = 1;
constexpr std::uint32_t sample_rate_hz = 48000U;
constexpr std::uint32_t bytes_per_sample = sizeof(float);
constexpr std::uint64_t maximum_data_bytes = std::numeric_limits<std::uint32_t>::max() - 36U;

struct CaptureDevice
{
    bool raw{};
    WinmmCapture winmm{};
    WasapiCapture wasapi{};

    [[nodiscard]] m110::Status open(std::uint32_t index, std::string_view name, waveform_capture::SamplesCallback callback, void* context, waveform_capture::ChannelPolicy policy) noexcept
    {
        return raw ? wasapi.open(name, callback, context, policy) : winmm.open(index, name, callback, context, policy);
    }

    [[nodiscard]] m110::Status start() noexcept { return raw ? wasapi.start() : winmm.start(); }

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

    [[nodiscard]] bool failed() const noexcept { return raw ? wasapi.failed() : winmm.failed(); }

    // The Status that stopped a running capture (failed()); success until then.
    [[nodiscard]] m110::Status status() const noexcept { return raw ? wasapi.status() : winmm.status(); }

    [[nodiscard]] std::string_view backend_label() const noexcept { return raw ? "WASAPI RAW" : "WinMM"; }

    [[nodiscard]] std::uint32_t device_index() const noexcept { return raw ? 0U : winmm.device_index(); }

    [[nodiscard]] std::string_view native_name() const noexcept { return raw ? wasapi.native_name() : winmm.native_name(); }

    [[nodiscard]] std::string_view endpoint_id() const noexcept { return raw ? wasapi.endpoint_id() : winmm.endpoint_id(); }

    [[nodiscard]] std::string_view input_format() const noexcept { return raw ? wasapi.format_name() : WinmmCapture::input_format(); }

    [[nodiscard]] WinmmCounters counters() const noexcept { return raw ? wasapi.counters() : winmm.counters(); }
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

// A command line the tool refuses; main() prints the usage.
constexpr m110::Status bad_arguments{.code = m110::StatusCode::invalid_argument, .message = "bad arguments"};
// An option value that is no decimal below 2^64.
constexpr m110::Status not_decimal{.code = m110::StatusCode::invalid_argument, .message = "not a decimal integer below 2^64"};

// `text` as a decimal, or not_decimal.
[[nodiscard]] m110::Result<std::uint64_t> parse_u64(std::string_view text) noexcept
{
    if (text.empty())
    {
        return not_decimal;
    }

    std::uint64_t parsed = 0U;

    for (const auto character : text)
    {
        const auto digit = character >= '0' && character <= '9' ? static_cast<std::uint64_t>(character - '0') : 10U;

        if (digit > 9U || parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U)
        {
            return not_decimal;
        }

        parsed = parsed * 10U + digit;
    }

    return parsed;
}

void print_device_json(void*, const waveform_capture::WinmmDevice& device) noexcept
{
    std::printf("{\"index\":%u,\"backend\":\"winmm\",\"native_name\":", device.index);
    print_json_string(device.native_name);
    std::printf(",\"endpoint_id\":null,\"pnp_instance_id\":null,\"max_input_channels\":%u,\"requested_input_format\":\"PCM16_LE_STEREO_48000\",\"conversion\":\"stereo_pcm16_to_mono_float\",\"default_"
                "channel\":\"left\"}\n",
                device.max_input_channels);
}

void print_wasapi_device_json(void*, const waveform_capture::WinmmDevice& device) noexcept
{
    std::printf("{\"index\":%u,\"backend\":\"wasapi-raw\",\"native_name\":", device.index);
    print_json_string(device.native_name);
    std::printf(",\"endpoint_id\":null,\"pnp_instance_id\":null,\"max_input_channels\":%u,\"requested_input_format\":\"WASAPI_SHARED_RAW_MIX_FORMAT\",\"conversion\":\"interleaved_to_mono_float\","
                "\"default_channel\":\"left\"}\n",
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

m110::Status capture_callback(void* context, std::span<const float> samples) noexcept
{
    auto& state = *static_cast<QueueState*>(context);
    state.measurements.observe(samples);

    if (!state.first_callback.load(std::memory_order_acquire))
    {
        waveform_capture::publish_first_callback(state.first_callback, state.first_callback_ns, monotonic_ns());
    }

    state.last_callback_ns.store(monotonic_ns(), std::memory_order_release);
    const m110::Status accepted = state.queue.push(samples);

    if (accepted.is_ok())
    {
        const auto queue_samples = state.queue.size() * waveform_capture::capture_block_samples;
        auto maximum = state.maximum_queue_samples.load(std::memory_order_relaxed);

        while (queue_samples > maximum && !state.maximum_queue_samples.compare_exchange_weak(maximum, queue_samples, std::memory_order_relaxed))
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

// The output file's header, or a stream that failed.
constexpr m110::Status header_unwritten{.code = m110::StatusCode::io_error, .message = "the WAV header cannot be written"};
constexpr m110::Status too_long_for_wav{.code = m110::StatusCode::invalid_argument, .message = "the capture is too long for a WAV file"};
constexpr m110::Status sizes_unpatched{.code = m110::StatusCode::io_error, .message = "the WAV header sizes cannot be patched"};

// The IEEE-float WAV header with zero sizes: success, or header_unwritten.
[[nodiscard]] m110::Status write_header(std::ofstream& output)
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
    return output.good() ? m110::Status::success() : header_unwritten;
}

void writer_main(WriterState& state) noexcept
{
    std::ofstream output{state.path, std::ios::binary | std::ios::trunc};

    if (!output || !write_header(output).is_ok())
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

// Patches the RIFF and data sizes of `samples` floats into the header:
// success, too_long_for_wav, or sizes_unpatched.
[[nodiscard]] m110::Status finalize_wav(const std::string& path, std::uint64_t samples) noexcept
{
    const auto data_bytes = samples * bytes_per_sample;

    if (data_bytes > maximum_data_bytes)
    {
        return too_long_for_wav;
    }

    std::fstream output{path, std::ios::binary | std::ios::in | std::ios::out};

    if (!output)
    {
        return sizes_unpatched;
    }

    const auto riff_size = static_cast<std::uint32_t>(36U + data_bytes);
    const auto data_size = static_cast<std::uint32_t>(data_bytes);
    output.seekp(4);
    output.write(reinterpret_cast<const char*>(&riff_size), sizeof(riff_size));
    output.seekp(40);
    output.write(reinterpret_cast<const char*>(&data_size), sizeof(data_size));
    output.flush();
    return output.good() ? m110::Status::success() : sizes_unpatched;
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
    std::printf("{\"input_format\":\"%.*s\",\"output_sample_rate_hz\":48000,\"output_channels\":1,\"output_encoding\":\"IEEE_FLOAT32\",\"output_bits_per_sample\":32,\"conversion\":\"interleaved_to_"
                "mono_float\",\"channel_policy\":\"%.*s\"}",
                static_cast<int>(capture.input_format().size()), capture.input_format().data(), static_cast<int>(waveform_capture::channel_policy_name(policy).size()),
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

// One line on stderr, its parts as they are; a failed write of a diagnostic has nowhere to go.
void put_error(std::initializer_list<std::string_view> parts) noexcept
{
    for (const std::string_view part : parts)
    {
        static_cast<void>(std::fwrite(part.data(), 1U, part.size(), stderr));
    }

    static_cast<void>(std::fputc('\n', stderr));
}

// Why a running capture stopped, on stderr, when it failed.
void put_failure(const CaptureDevice& capture)
{
    if (const m110::Status failure = capture.status(); !failure.is_ok())
    {
        put_error({capture.backend_label(), " capture failed: ", failure.message});
    }
}

// A --device-index that no WinMM input has.
constexpr m110::Status no_input_at_index{.code = m110::StatusCode::invalid_argument, .message = "no WinMM input has this --device-index"};

// The name of the WinMM input at `index`, which open() checks again against
// the device it opens; no_input_at_index when no input has that index.
[[nodiscard]] m110::Result<std::string> winmm_input_name(std::uint32_t index)
{
    struct IndexSelection
    {
        std::uint32_t requested{};
        std::optional<std::string> name{};
    } selection{index};
    static_cast<void>(WinmmCapture::enumerate(
        [](void* context, const waveform_capture::WinmmDevice& device) noexcept
        {
            auto& state = *static_cast<IndexSelection*>(context);

            if (state.requested == device.index)
            {
                state.name = std::string{device.native_name};
            }
        },

        &selection));

    if (!selection.name.has_value())
    {
        return no_input_at_index;
    }

    return *std::move(selection.name);
}

void usage()
{
    std::fputs("usage: waveform_capture [--backend winmm|wasapi-raw] --list-devices [--json]\n"
               "       waveform_capture [--backend winmm|wasapi-raw] --output <capture.wav>\n"
               "         (--device-index <n> | --device-name <exact>) --sample-rate 48000\n"
               "         --channel left|right|average --max-seconds <seconds> [--stop-stdin]\n",
               stderr);
}

// The command line as main() reads it. A --device-index or --max-seconds that
// is no decimal leaves its have_ flag false, which the rules then refuse.
struct Options
{
    bool list_devices{};
    bool json{};
    bool stop_stdin{};
    std::string backend_name{"winmm"};
    std::string output_path{};
    std::string device_name{};
    std::uint64_t device_index_value{};
    std::uint64_t max_seconds{};
    ChannelPolicy channel_policy{ChannelPolicy::left};
    bool have_index{};
    bool have_name{};
    bool have_max_seconds{};
};

// --channel's value, or bad_arguments.
[[nodiscard]] m110::Result<ChannelPolicy> parse_channel(std::string_view value) noexcept
{
    if (value == "left")
    {
        return ChannelPolicy::left;
    }

    if (value == "right")
    {
        return ChannelPolicy::right;
    }

    if (value == "average")
    {
        return ChannelPolicy::average;
    }

    return bad_arguments;
}

// One option that takes a value, with that value: success, or bad_arguments
// for an option the tool does not know, a channel it does not know or a
// sample rate other than 48000.
[[nodiscard]] m110::Status apply_option(Options& options, std::string_view option, std::string_view value)
{
    if (option == "--backend")
    {
        options.backend_name = std::string{value};
    }
    else if (option == "--output")
    {
        options.output_path = std::string{value};
    }
    else if (option == "--device-name")
    {
        options.device_name = std::string{value};
        options.have_name = true;
    }
    else if (option == "--device-index")
    {
        const m110::Result<std::uint64_t> index = parse_u64(value);
        options.have_index = index.has_value();
        options.device_index_value = index.has_value() ? index.value() : options.device_index_value;
    }
    else if (option == "--channel")
    {
        const m110::Result<ChannelPolicy> policy = parse_channel(value);

        if (!policy.has_value())
        {
            return policy.error();
        }

        options.channel_policy = policy.value();
    }
    else if (option == "--sample-rate")
    {
        const m110::Result<std::uint64_t> rate = parse_u64(value);

        if (!rate.has_value() || rate.value() != sample_rate_hz)
        {
            return bad_arguments;
        }
    }
    else if (option == "--max-seconds")
    {
        const m110::Result<std::uint64_t> seconds = parse_u64(value);
        options.have_max_seconds = seconds.has_value();
        options.max_seconds = seconds.has_value() ? seconds.value() : options.max_seconds;
    }
    else
    {
        return bad_arguments;
    }

    return m110::Status::success();
}

// The command line, its program name first: the options, or bad_arguments
// (an unknown option, or an option without its value or with one refused).
[[nodiscard]] m110::Result<Options> parse_arguments(std::span<char* const> arguments)
{
    Options options;

    for (std::size_t index = 1U; index < arguments.size(); ++index)
    {
        const std::string_view argument{arguments[index]};

        if (argument == "--list-devices")
        {
            options.list_devices = true;
        }
        else if (argument == "--json")
        {
            options.json = true;
        }
        else if (argument == "--stop-stdin")
        {
            options.stop_stdin = true;
        }
        else if (index + 1U == arguments.size())
        {
            return bad_arguments;
        }
        else
        {
            ++index;

            if (const m110::Status applied = apply_option(options, argument, arguments[index]); !applied.is_ok())
            {
                return applied;
            }
        }
    }

    return options;
}

// --list-devices: one JSON line per input of the backend, exit 0; the usage
// and 2 for other options without --json, or an unknown backend.
[[nodiscard]] int list_inputs(const Options& options, std::size_t argument_count)
{
    constexpr std::size_t program_and_option = 2U;

    if (!options.json && argument_count != program_and_option)
    {
        usage();
        return 2;
    }

    if (options.backend_name == "winmm")
    {
        static_cast<void>(WinmmCapture::enumerate(&print_device_json, nullptr));
    }
    else if (options.backend_name == "wasapi-raw")
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

void print_driver_discontinuity_detection(bool raw)
{
    std::fputs(",\"driver_discontinuity_detection\":", stdout);

    if (raw)
    {
        std::fputs("{\"available\":true,\"counter\":\"data_discontinuity_packets\",\"startup_packets_separate\":true}", stdout);
    }
    else
    {
        std::fputs("{\"available\":false,\"reason\":\"WinMM exposes no discontinuity counter through this API\"}", stdout);
    }
}

void print_capture_measurements(const CaptureMeasurements& measurements)
{
    std::fputs(",\"measurements\":{", stdout);
    std::printf("\"peak_linear\":%.9g", static_cast<double>(measurements.peak));
    print_double_field("rms_linear", measurements.rms());
    print_u64_field("nonfinite_samples", measurements.nonfinite_samples);
    print_u64_field("clipped_samples", measurements.clipped_samples);
    std::fputs(",\"peak_dbfs\":", stdout);

    if (std::isfinite(measurements.peak_dbfs()))
    {
        std::printf("%.9g", measurements.peak_dbfs());
    }
    else
    {
        std::fputs("null", stdout);
    }

    std::fputs(",\"rms_dbfs\":", stdout);

    if (std::isfinite(measurements.rms_dbfs()))
    {
        std::printf("%.9g", measurements.rms_dbfs());
    }
    else
    {
        std::fputs("null", stdout);
    }

    std::fputs("}}\n", stdout);
}

} // namespace

int main(int argc, char** argv)
{
    m110::install_host_critical_error_handler("waveform_capture", exit_failure);
    const std::span<char* const> arguments{argv, argc > 0 ? static_cast<std::size_t>(argc) : 0U};
    const m110::Result<Options> parsed = parse_arguments(arguments);

    if (!parsed.has_value())
    {
        usage();
        return 2;
    }

    const Options& options = parsed.value();

    if (options.list_devices)
    {
        return list_inputs(options, arguments.size());
    }

    const bool raw_backend = options.backend_name == "wasapi-raw";

    if (!raw_backend && options.backend_name != "winmm")
    {
        usage();
        return 2;
    }

    constexpr auto maximum_seconds = maximum_data_bytes / bytes_per_sample / sample_rate_hz;

    if (options.output_path.empty() || (!options.have_index && !options.have_name) || (options.have_index && options.have_name) || !options.have_max_seconds || options.max_seconds == 0U ||
        options.max_seconds > maximum_seconds)
    {
        usage();
        return 2;
    }

    std::uint32_t selected_index = 0U;

    if (raw_backend && !options.have_name)
    {
        std::fprintf(stderr, "--backend wasapi-raw requires --device-name for stable endpoint selection\n");
        return 2;
    }

    if (raw_backend && options.have_index)
    {
        std::fprintf(stderr, "--backend wasapi-raw does not accept --device-index; use the exact --device-name\n");
        return 2;
    }

    if (options.have_name && !raw_backend)
    {
        struct NameSelection
        {
            std::string_view requested;
            std::uint32_t matches{};
            std::uint32_t selected{};
        } selection{options.device_name};
        static_cast<void>(WinmmCapture::enumerate(
            [](void* context, const waveform_capture::WinmmDevice& device) noexcept
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
            put_error({"requested WinMM input name was not unique: ", options.device_name, " (matches=", std::to_string(selection.matches), ")"});
            return 1;
        }
    }
    else
    {
        if (options.device_index_value > std::numeric_limits<std::uint32_t>::max())
        {
            usage();
            return 2;
        }

        selected_index = static_cast<std::uint32_t>(options.device_index_value);
    }

    CaptureDevice capture{raw_backend};
    QueueState queue;
    queue.data_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (queue.data_event == nullptr)
    {
        std::fprintf(stderr, "failed to create capture queue event\n");
        return 1;
    }

    std::string expected_name = options.device_name;

    if (!options.have_name && !raw_backend)
    {
        m110::Result<std::string> indexed = winmm_input_name(selected_index);

        if (!indexed.has_value())
        {
            put_error({"failed to open WinMM capture: ", indexed.error().message, ": ", std::to_string(selected_index)});
            CloseHandle(queue.data_event);
            return 1;
        }

        expected_name = std::move(indexed).value();
    }

    if (const m110::Status opened = capture.open(selected_index, expected_name, &capture_callback, &queue, options.channel_policy); !opened.is_ok())
    {
        put_error({"failed to open ", capture.backend_label(), " capture: ", opened.message});
        CloseHandle(queue.data_event);
        return 1;
    }

    WriterState writer{queue, options.output_path};
    writer.thread = std::thread{writer_main, std::ref(writer)};

    if (const m110::Status started = capture.start(); !started.is_ok())
    {
        put_error({"failed to start ", capture.backend_label(), " capture: ", started.message});
        capture.close();
        writer.producer_done.store(true, std::memory_order_release);
        SetEvent(queue.data_event);
        writer.thread.join();
        CloseHandle(queue.data_event);
        return 1;
    }

    for (std::uint32_t wait = 0U; wait < 500U && (!queue.first_callback.load(std::memory_order_acquire) || !writer.ready.load(std::memory_order_acquire)); ++wait)
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
        put_failure(capture);
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
    print_format(capture, options.channel_policy);
    std::printf(",\"monotonic_ns\":%llu,\"samples_delivered\":%llu,\"samples_written\":%llu,\"queue_capacity_samples\":%zu}\n", static_cast<unsigned long long>(monotonic_ns()),
                static_cast<unsigned long long>(capture.counters().samples_delivered), static_cast<unsigned long long>(writer.samples_written.load()),
                waveform_capture::capture_queue_blocks * waveform_capture::capture_block_samples);
    std::fflush(stdout);
    struct ReaderState
    {
        std::atomic<bool> requested{};
        std::atomic<CompletionReason> reason{CompletionReason::stop};
    };

    auto reader = std::make_shared<ReaderState>();
    std::thread reader_thread;

    if (options.stop_stdin)
    {
        reader_thread = std::thread{[reader]
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
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{options.max_seconds};

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

        if (capture.failed())
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
    put_failure(capture);

    if (!finalize_wav(options.output_path, writer.samples_written.load(std::memory_order_acquire)).is_ok())
    {
        reason = CompletionReason::capture_error;
        writer.write_errors.fetch_add(1U, std::memory_order_relaxed);
    }

    const bool integrity_failed = counters.dropped_blocks != 0U || counters.partial_buffers != 0U || counters.data_discontinuity_packets > counters.startup_discontinuity_packets ||
                                  queue.measurements.nonfinite_samples != 0U || writer.failed.load(std::memory_order_acquire);
    reason = waveform_capture::capture_completion_reason(reason, capture.failed(), integrity_failed);

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
    print_format(capture, options.channel_policy);
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
    print_driver_discontinuity_detection(capture.raw);
    print_capture_measurements(queue.measurements);
    std::fflush(stdout);
    CloseHandle(queue.data_event);
    return reason == CompletionReason::stop ? 0 : 1;
}

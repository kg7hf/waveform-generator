// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "host/capture/wasapi_audio.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <audioclient.h>
#include <combaseapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#include <windows.h>

namespace waveform_capture
{

struct WasapiCapture::State
{
    std::wstring endpoint_id{};
    HANDLE packet_event{};
    HANDLE stop_event{};
    std::thread worker{};
    std::uint32_t sample_rate{};
    std::uint16_t channels{};
    std::uint16_t bits_per_sample{};
    std::uint16_t frame_bytes{};
    bool is_float{};
    std::vector<std::byte> packet{};
    std::vector<float> converted{};
};

namespace
{

// open()'s and start()'s refusals, and the worker's failures (status()).
constexpr m110::Status invalid_arguments{.code = m110::StatusCode::invalid_argument, .message = "invalid WASAPI capture arguments"};
constexpr m110::Status com_failed{.code = m110::StatusCode::unavailable, .message = "WASAPI COM initialization failed"};
constexpr m110::Status name_not_wide{.code = m110::StatusCode::invalid_argument, .message = "WASAPI endpoint name is not representable"};
constexpr m110::Status enumeration_failed{.code = m110::StatusCode::unavailable, .message = "WASAPI endpoint enumeration failed"};
constexpr m110::Status endpoint_not_found{.code = m110::StatusCode::unavailable, .message = "WASAPI endpoint name was not unique or active"};
constexpr m110::Status no_friendly_name{.code = m110::StatusCode::unavailable, .message = "WASAPI endpoint has no friendly name"};
constexpr m110::Status not_utf8{.code = m110::StatusCode::invalid_argument, .message = "the text is not representable as UTF-8 in its buffer"};
constexpr m110::Status no_state{.code = m110::StatusCode::unavailable, .message = "cannot allocate WASAPI capture state"};
constexpr m110::Status endpoint_id_not_utf8{.code = m110::StatusCode::invalid_argument, .message = "WASAPI endpoint ID is not representable as UTF-8"};
constexpr m110::Status not_startable{.code = m110::StatusCode::unavailable, .message = "WASAPI capture is not startable"};
constexpr m110::Status no_events{.code = m110::StatusCode::unavailable, .message = "cannot create WASAPI capture events"};
constexpr m110::Status worker_com_failed{.code = m110::StatusCode::unavailable, .message = "WASAPI worker COM initialization failed"};
constexpr m110::Status mix_format_refused{.code = m110::StatusCode::invalid_configuration, .message = "WASAPI RAW mix format is not 48 kHz PCM16 or float mono/stereo"};
constexpr m110::Status raw_initialization_failed{.code = m110::StatusCode::io_error, .message = "WASAPI RAW initialization failed"};
constexpr m110::Status raw_start_failed{.code = m110::StatusCode::io_error, .message = "WASAPI RAW capture start failed"};
constexpr m110::Status raw_stop_failed{.code = m110::StatusCode::io_error, .message = "WASAPI RAW capture stop failed"};
constexpr m110::Status event_wait_failed{.code = m110::StatusCode::io_error, .message = "WASAPI RAW event wait failed"};
constexpr m110::Status packet_size_failed{.code = m110::StatusCode::io_error, .message = "WASAPI RAW packet-size query failed"};
constexpr m110::Status buffer_acquisition_failed{.code = m110::StatusCode::io_error, .message = "WASAPI RAW buffer acquisition failed"};
constexpr m110::Status buffer_release_failed{.code = m110::StatusCode::io_error, .message = "WASAPI RAW buffer release failed"};
constexpr m110::Status malformed_packet{.code = m110::StatusCode::io_error, .message = "WASAPI RAW returned a malformed capture packet"};
constexpr m110::Status callback_failed{.code = m110::StatusCode::io_error, .message = "WASAPI RAW capture dropped a sample block"};

void release(IUnknown* object) noexcept
{
    if (object != nullptr)
    {
        object->Release();
    }
}

// The endpoint's friendly name, or no_friendly_name.
[[nodiscard]] m110::Result<std::wstring> friendly_name(IMMDevice* device) noexcept
{
    IPropertyStore* store{};

    if (device == nullptr || FAILED(device->OpenPropertyStore(STGM_READ, &store)))
    {
        return no_friendly_name;
    }

    PROPVARIANT property;
    PropVariantInit(&property);
    const auto ok = SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &property)) && property.vt == VT_LPWSTR && property.pwszVal != nullptr;
    std::wstring value;

    if (ok)
    {
        value = property.pwszVal;
    }

    PropVariantClear(&property);
    release(store);

    if (!ok)
    {
        return no_friendly_name;
    }

    return value;
}

// `input` in UTF-8 and a NUL in `output`: success, or not_utf8 when it is not
// valid UTF-16 or does not fit.
[[nodiscard]] m110::Status to_utf8(std::wstring_view input, std::span<char> output) noexcept
{
    if (input.size() > static_cast<std::size_t>(INT_MAX) || output.empty() || output.size() > static_cast<std::size_t>(INT_MAX))
    {
        return not_utf8;
    }

    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), output.data(), static_cast<int>(output.size() - 1U), nullptr, nullptr);

    if (count <= 0)
    {
        return not_utf8;
    }

    output[static_cast<std::size_t>(count)] = '\0';
    return m110::Status::success();
}

// `name` from the ANSI code page, as the endpoints' friendly names are compared.
[[nodiscard]] m110::Result<std::wstring> wide_name(std::string_view name) noexcept
{
    if (name.size() > static_cast<std::size_t>(INT_MAX))
    {
        return name_not_wide;
    }

    const auto wide_count = MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED, name.data(), static_cast<int>(name.size()), nullptr, 0);

    if (wide_count <= 0)
    {
        return name_not_wide;
    }

    std::wstring wide(static_cast<std::size_t>(wide_count), L'\0');

    if (MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED, name.data(), static_cast<int>(name.size()), wide.data(), wide_count) != wide_count)
    {
        return name_not_wide;
    }

    return wide;
}

// The ID of the one active capture endpoint named `expected_name`:
// endpoint_not_found when none or more than one has the name.
[[nodiscard]] m110::Result<std::wstring> find_endpoint(std::string_view expected_name) noexcept
{
    const m110::Result<std::wstring> expected = wide_name(expected_name);

    if (!expected.has_value())
    {
        return expected.error();
    }

    IMMDeviceEnumerator* enumerator{};
    IMMDeviceCollection* collection{};

    if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) || FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection)))
    {
        release(collection);
        release(enumerator);
        return enumeration_failed;
    }

    UINT count{};
    collection->GetCount(&count);
    UINT matches{};
    std::wstring endpoint_id;

    for (UINT index = 0U; index < count; ++index)
    {
        IMMDevice* candidate{};

        if (SUCCEEDED(collection->Item(index, &candidate)))
        {
            const m110::Result<std::wstring> name = friendly_name(candidate);

            if (name.has_value() && name.value() == expected.value())
            {
                ++matches;

                if (matches == 1U)
                {
                    LPWSTR id{};

                    if (SUCCEEDED(candidate->GetId(&id)) && id != nullptr)
                    {
                        endpoint_id = id;
                        CoTaskMemFree(id);
                    }
                }
            }
        }

        release(candidate);
    }

    release(collection);
    release(enumerator);

    if (matches != 1U || endpoint_id.empty())
    {
        return endpoint_not_found;
    }

    return endpoint_id;
}

bool is_pcm(const WAVEFORMATEX& format) noexcept
{
    constexpr GUID pcm_subformat{0x00000001L, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
    return format.wFormatTag == WAVE_FORMAT_PCM || (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE && IsEqualGUID(reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format).SubFormat, pcm_subformat));
}

bool is_float(const WAVEFORMATEX& format) noexcept
{
    constexpr GUID float_subformat{0x00000003L, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
    return format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
           (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE && IsEqualGUID(reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format).SubFormat, float_subformat));
}

bool valid_mix_format(const WAVEFORMATEX& format) noexcept
{
    if (format.nSamplesPerSec != 48000U || (format.nChannels != 1U && format.nChannels != 2U))
    {
        return false;
    }

    const bool extensible = format.wFormatTag == WAVE_FORMAT_EXTENSIBLE;

    if (extensible && format.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
    {
        return false;
    }

    const bool pcm = is_pcm(format);
    const bool ieee_float = is_float(format);

    if (!pcm && !ieee_float)
    {
        return false;
    }

    if (extensible)
    {
        const auto& extensible_format = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        const auto valid_bits = extensible_format.Samples.wValidBitsPerSample;

        if (valid_bits != format.wBitsPerSample)
        {
            return false;
        }
    }

    const auto sample_type = pcm ? CaptureSampleType::pcm16 : CaptureSampleType::ieee_float32;
    return valid_capture_format(format.nSamplesPerSec, format.nChannels, sample_type, format.wBitsPerSample, format.nBlockAlign, format.nAvgBytesPerSec);
}

} // namespace

void WasapiCapture::fail_worker(m110::Status failure) noexcept
{
    if (!worker_failed_.load(std::memory_order_relaxed))
    {
        worker_status_ = failure;
        worker_failed_.store(true, std::memory_order_release);
    }
}

void WasapiCapture::record_stop_result(long result) noexcept
{
    if (FAILED(result))
    {
        fail_worker(raw_stop_failed);
    }
}

void WasapiCapture::record_drained_packet(std::size_t samples) noexcept
{
    counters_.reset_buffers.fetch_add(1U, std::memory_order_relaxed);
    counters_.reset_samples.fetch_add(samples, std::memory_order_relaxed);

    if (samples != 0U && samples < capture_block_samples)
    {
        counters_.reset_partial_buffers.fetch_add(1U, std::memory_order_relaxed);
    }
}

m110::Status WasapiCapture::status() const noexcept
{
    if (worker_failed_.load(std::memory_order_acquire))
    {
        return worker_status_;
    }

    return counters_.dropped_blocks.load(std::memory_order_acquire) != 0U ? callback_failed : m110::Status::success();
}

std::uint32_t WasapiCapture::enumerate(DeviceVisitor visitor, void* context) noexcept
{
    const auto com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    if (FAILED(com_result))
    {
        return 0U;
    }

    IMMDeviceEnumerator* enumerator{};
    IMMDeviceCollection* collection{};

    if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) || FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection)))
    {
        release(collection);
        release(enumerator);
        CoUninitialize();
        return 0U;
    }

    UINT count{};
    collection->GetCount(&count);

    for (UINT index = 0U; index < count; ++index)
    {
        IMMDevice* device{};
        const m110::Result<std::wstring> found = SUCCEEDED(collection->Item(index, &device)) ? friendly_name(device) : no_friendly_name;

        if (found.has_value() && visitor != nullptr)
        {
            const std::wstring& name = found.value();
            std::string utf8;
            utf8.resize(name.size() * 3U + 1U);
            const auto chars = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), utf8.data(), static_cast<int>(utf8.size() - 1U), nullptr, nullptr);

            if (chars > 0)
            {
                utf8.resize(static_cast<std::size_t>(chars));
                visitor(context, WinmmDevice{index, utf8, 2U});
            }
        }

        release(device);
    }

    release(collection);
    release(enumerator);
    CoUninitialize();
    return count;
}

m110::Status WasapiCapture::open(std::string_view expected_name, SamplesCallback callback, void* context, ChannelPolicy channel_policy) noexcept
{
    if (state_ != nullptr || callback == nullptr || expected_name.empty())
    {
        return invalid_arguments;
    }

    // close() joined the previous worker. Keep its evidence until a new open,
    // then clear every per-session result before a worker can start again.
    worker_status_ = m110::Status::success();
    worker_failed_.store(false, std::memory_order_relaxed);
    counters_.input_buffers.store(0U, std::memory_order_relaxed);
    counters_.partial_buffers.store(0U, std::memory_order_relaxed);
    counters_.reset_buffers.store(0U, std::memory_order_relaxed);
    counters_.reset_partial_buffers.store(0U, std::memory_order_relaxed);
    counters_.reset_samples.store(0U, std::memory_order_relaxed);
    counters_.dropped_blocks.store(0U, std::memory_order_relaxed);
    counters_.samples_delivered.store(0U, std::memory_order_relaxed);
    counters_.data_discontinuity_packets.store(0U, std::memory_order_relaxed);
    counters_.startup_discontinuity_packets.store(0U, std::memory_order_relaxed);
    counters_.silent_packets.store(0U, std::memory_order_relaxed);
    counters_.position_packets.store(0U, std::memory_order_relaxed);
    counters_.position_errors.store(0U, std::memory_order_relaxed);
    counters_.first_device_position.store(0U, std::memory_order_relaxed);
    counters_.first_qpc_position.store(0U, std::memory_order_relaxed);
    counters_.last_device_position.store(0U, std::memory_order_relaxed);
    counters_.last_qpc_position.store(0U, std::memory_order_relaxed);
    std::ranges::fill(format_name_, '\0');
    std::ranges::copy(std::string_view{"unknown"}, std::begin(format_name_));

    const auto com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    if (FAILED(com_result))
    {
        return com_failed;
    }

    m110::Result<std::wstring> endpoint = find_endpoint(expected_name);
    CoUninitialize();

    if (!endpoint.has_value())
    {
        return endpoint.error();
    }

    auto* state = new (std::nothrow) State{};

    if (state == nullptr)
    {
        return no_state;
    }

    state->endpoint_id = std::move(endpoint).value();

    if (!to_utf8(state->endpoint_id, endpoint_id_).is_ok())
    {
        delete state;
        return endpoint_id_not_utf8;
    }

    state_ = state;
    // The view need not end in a NUL: copy at most its length, then terminate.
    std::ranges::fill(native_name_, '\0');
    std::ranges::copy(expected_name.substr(0U, sizeof(native_name_) - 1U), std::begin(native_name_));
    callback_ = callback;
    context_ = context;
    channel_policy_ = channel_policy;
    stopped_.store(false, std::memory_order_release);
    return m110::Status::success();
}

m110::Status WasapiCapture::start() noexcept
{
    if (!capture_start_allowed(state_ != nullptr, stopped_.load(std::memory_order_acquire), accepting_.load(std::memory_order_acquire), state_ != nullptr && state_->worker.joinable()))
    {
        return not_startable;
    }

    state_->packet_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    state_->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    if (state_->packet_event == nullptr || state_->stop_event == nullptr)
    {
        if (state_->packet_event != nullptr)
        {
            CloseHandle(state_->packet_event);
        }

        if (state_->stop_event != nullptr)
        {
            CloseHandle(state_->stop_event);
        }

        state_->packet_event = nullptr;
        state_->stop_event = nullptr;
        return no_events;
    }

    stopped_.store(false, std::memory_order_release);
    accepting_.store(true, std::memory_order_release);
    stopping_.store(false, std::memory_order_release);
    // No worker runs here (the last one was joined), so this thread is their one writer.
    worker_status_ = m110::Status::success();
    worker_failed_.store(false, std::memory_order_release);
    state_->worker = std::thread{[this] { worker_main(); }};
    return m110::Status::success();
}

void WasapiCapture::stop() noexcept
{
    if (state_ == nullptr || stopped_.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }

    accepting_.store(false, std::memory_order_release);
    stopping_.store(true, std::memory_order_release);

    if (state_->stop_event != nullptr)
    {
        SetEvent(state_->stop_event);
    }

    if (state_->packet_event != nullptr)
    {
        SetEvent(state_->packet_event);
    }

    if (state_->worker.joinable())
    {
        state_->worker.join();
    }
}

void WasapiCapture::close() noexcept
{
    stop();

    if (state_ == nullptr)
    {
        return;
    }

    if (state_->packet_event != nullptr)
    {
        CloseHandle(state_->packet_event);
    }

    if (state_->stop_event != nullptr)
    {
        CloseHandle(state_->stop_event);
    }

    delete state_;
    state_ = nullptr;
}

WasapiCapture::~WasapiCapture() noexcept
{
    close();
}

void WasapiCapture::worker_main() noexcept
{
    auto* state = state_;
    const auto com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    if (FAILED(com_result))
    {
        fail_worker(worker_com_failed);
        accepting_.store(false, std::memory_order_release);
        return;
    }

    IMMDeviceEnumerator* enumerator{};
    IMMDevice* device{};
    IAudioClient2* client{};
    IAudioCaptureClient* capture{};
    WAVEFORMATEX* mix_format{};
    const auto setup_ok = SUCCEEDED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) &&
                          SUCCEEDED(enumerator->GetDevice(state->endpoint_id.c_str(), &device)) &&
                          SUCCEEDED(device->Activate(IID_IAudioClient2, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) && SUCCEEDED(client->GetMixFormat(&mix_format));

    if (!setup_ok || mix_format == nullptr || !valid_mix_format(*mix_format))
    {
        fail_worker(mix_format_refused);

        if (mix_format != nullptr)
        {
            CoTaskMemFree(mix_format);
        }

        release(capture);
        release(client);
        release(device);
        release(enumerator);
        CoUninitialize();
        accepting_.store(false, std::memory_order_release);
        return;
    }

    state->sample_rate = mix_format->nSamplesPerSec;
    state->channels = mix_format->nChannels;
    state->bits_per_sample = mix_format->wBitsPerSample;
    state->frame_bytes = mix_format->nBlockAlign;
    state->is_float = is_float(*mix_format);
    std::snprintf(format_name_, sizeof(format_name_), "%s_%s_%u", state->is_float ? "IEEE_FLOAT32" : "PCM16_LE", state->channels == 1U ? "MONO" : "STEREO", state->sample_rate);
    AudioClientProperties properties{};
    properties.cbSize = sizeof(properties);
    properties.Options = AUDCLNT_STREAMOPTIONS_RAW;
    const auto flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    const auto initialized = SUCCEEDED(client->SetClientProperties(&properties)) && SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 10'000'000, 0, mix_format, nullptr)) &&
                             SUCCEEDED(client->SetEventHandle(state->packet_event));
    UINT32 buffer_frames{};

    if (!initialized || FAILED(client->GetBufferSize(&buffer_frames)) || FAILED(client->GetService(IID_PPV_ARGS(&capture))))
    {
        fail_worker(raw_initialization_failed);
        CoTaskMemFree(mix_format);
        release(capture);
        release(client);
        release(device);
        release(enumerator);
        CoUninitialize();
        accepting_.store(false, std::memory_order_release);
        return;
    }

    state->packet.resize(static_cast<std::size_t>(buffer_frames) * state->frame_bytes);
    state->converted.resize(capture_block_samples);
    CoTaskMemFree(mix_format);

    if (FAILED(client->Start()))
    {
        fail_worker(raw_start_failed);
        release(capture);
        release(client);
        release(device);
        release(enumerator);
        CoUninitialize();
        accepting_.store(false, std::memory_order_release);
        return;
    }

    bool stopping = false;

    for (;;)
    {
        const HANDLE events[] = {state->stop_event, state->packet_event};
        const auto wait = WaitForMultipleObjects(2U, events, FALSE, 1000U);

        if (wait == WAIT_FAILED)
        {
            fail_worker(event_wait_failed);
            break;
        }

        if (wait == WAIT_OBJECT_0 || stopping_.load(std::memory_order_acquire))
        {
            stopping = true;
            const auto stop_result = client->Stop();
            record_stop_result(stop_result);
            if (FAILED(stop_result))
            {
                break;
            }
        }

        UINT32 packet_frames{};

        for (;;)
        {
            const auto next_result = capture->GetNextPacketSize(&packet_frames);

            if (FAILED(next_result))
            {
                fail_worker(packet_size_failed);
                stopping = true;
                break;
            }

            if (packet_frames == 0U)
            {
                break;
            }

            BYTE* data{};
            DWORD packet_flags{};
            UINT32 frames{};
            UINT64 device_position{};
            UINT64 qpc_position{};

            if (FAILED(capture->GetBuffer(&data, &frames, &packet_flags, &device_position, &qpc_position)))
            {
                fail_worker(buffer_acquisition_failed);
                stopping = true;
                break;
            }

            const auto silent = (packet_flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0U;
            const auto packet_capacity = state->packet.size() / state->frame_bytes;

            if (!valid_capture_packet(packet_frames, frames, packet_capacity, silent, data != nullptr))
            {
                const auto released = capture->ReleaseBuffer(frames);
                fail_worker(FAILED(released) ? buffer_release_failed : malformed_packet);
                stopping = true;
                break;
            }

            counters_.position_packets.fetch_add(1U, std::memory_order_relaxed);

            if (counters_.position_packets.load(std::memory_order_relaxed) == 1U)
            {
                counters_.first_device_position.store(device_position, std::memory_order_relaxed);
                counters_.first_qpc_position.store(qpc_position, std::memory_order_relaxed);
            }

            counters_.last_device_position.store(device_position, std::memory_order_relaxed);
            counters_.last_qpc_position.store(qpc_position, std::memory_order_relaxed);

            if (device_position == UINT64_MAX || qpc_position == UINT64_MAX)
            {
                counters_.position_errors.fetch_add(1U, std::memory_order_relaxed);
            }

            const auto available = static_cast<std::size_t>(frames);

            if (silent)
            {
                counters_.silent_packets.fetch_add(1U, std::memory_order_relaxed);
            }

            if ((packet_flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0U)
            {
                counters_.data_discontinuity_packets.fetch_add(1U, std::memory_order_relaxed);

                if (counters_.input_buffers.load(std::memory_order_relaxed) == 0U)
                {
                    counters_.startup_discontinuity_packets.fetch_add(1U, std::memory_order_relaxed);
                }
            }

            if (silent || data == nullptr)
            {
                std::fill_n(state->packet.begin(), available * state->frame_bytes, std::byte{});
            }
            else
            {
                std::memcpy(state->packet.data(), data, available * state->frame_bytes);
            }

            if (FAILED(capture->ReleaseBuffer(frames)))
            {
                fail_worker(buffer_release_failed);
                stopping = true;
                break;
            }

            counters_.input_buffers.fetch_add(1U, std::memory_order_relaxed);
            counters_.samples_delivered.fetch_add(available, std::memory_order_relaxed);

            if (stopping)
            {
                record_drained_packet(available);
            }

            for (std::size_t offset = 0U; offset < available; offset += capture_block_samples)
            {
                const auto chunk = std::min<std::size_t>(capture_block_samples, available - offset);

                for (std::size_t index = 0U; index < chunk; ++index)
                {
                    const auto frame = offset + index;
                    const auto* bytes = state->packet.data() + frame * state->frame_bytes;
                    auto value = [&](std::size_t channel) noexcept
                    {
                        if (state->is_float)
                        {
                            float sample{};
                            std::memcpy(&sample, bytes + channel * sizeof(float), sizeof(sample));
                            return sample;
                        }

                        std::int16_t sample{};
                        std::memcpy(&sample, bytes + channel * sizeof(std::int16_t), sizeof(sample));
                        return pcm16_to_float(sample);
                    };

                    const auto left = value(0U);
                    const auto right = state->channels == 1U ? left : value(1U);
                    state->converted[index] = select_channels_to_mono(left, right, channel_policy_);
                }

                if (!callback_(context_, std::span<const float>{state->converted.data(), chunk}).is_ok())
                {
                    latch_capture_failure(accepting_, counters_.dropped_blocks);
                    stopping = true;
                    record_stop_result(client->Stop());
                    break;
                }
            }

            if (stopping && !accepting_.load(std::memory_order_acquire))
            {
                break;
            }
        }

        if (stopping)
        {
            break;
        }
    }

    record_stop_result(client->Stop());

    accepting_.store(false, std::memory_order_release);

    release(capture);

    release(client);

    release(device);

    release(enumerator);

    CoUninitialize();
}

WinmmCounters WasapiCapture::counters() const noexcept
{
    return {
        counters_.input_buffers.load(std::memory_order_acquire),
        counters_.partial_buffers.load(std::memory_order_acquire),
        counters_.reset_buffers.load(std::memory_order_acquire),
        counters_.reset_partial_buffers.load(std::memory_order_acquire),
        counters_.reset_samples.load(std::memory_order_acquire),
        counters_.dropped_blocks.load(std::memory_order_acquire),
        counters_.samples_delivered.load(std::memory_order_acquire),
        counters_.data_discontinuity_packets.load(std::memory_order_acquire),
        counters_.startup_discontinuity_packets.load(std::memory_order_acquire),
        counters_.silent_packets.load(std::memory_order_acquire),
        counters_.position_packets.load(std::memory_order_acquire),
        counters_.position_errors.load(std::memory_order_acquire),
        counters_.first_device_position.load(std::memory_order_acquire),
        counters_.first_qpc_position.load(std::memory_order_acquire),
        counters_.last_device_position.load(std::memory_order_acquire),
        counters_.last_qpc_position.load(std::memory_order_acquire),
    };
}

} // namespace waveform_capture

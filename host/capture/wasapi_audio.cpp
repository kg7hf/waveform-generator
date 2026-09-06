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
#include <windows.h>
#include <audioclient.h>
#include <combaseapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propidl.h>

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

void release(IUnknown* object) noexcept
{
    if (object != nullptr) object->Release();
}

bool friendly_name(IMMDevice* device, std::wstring& value) noexcept
{
    IPropertyStore* store{};
    if (device == nullptr || FAILED(device->OpenPropertyStore(STGM_READ, &store))) return false;
    PROPVARIANT property;
    PropVariantInit(&property);
    const auto ok = SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &property)) &&
                    property.vt == VT_LPWSTR && property.pwszVal != nullptr;
    if (ok) value = property.pwszVal;
    PropVariantClear(&property);
    release(store);
    return ok;
}

bool to_utf8(std::wstring_view input, char* output, std::size_t capacity) noexcept
{
    if (input.size() > static_cast<std::size_t>(INT_MAX) || capacity == 0U) return false;
    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()),
                                           output, static_cast<int>(capacity - 1U), nullptr, nullptr);
    if (count <= 0) return false;
    output[count] = '\0';
    return true;
}

bool find_endpoint(std::string_view expected_name, std::wstring& endpoint_id, std::string_view& selected_name) noexcept
{
    if (expected_name.size() > static_cast<std::size_t>(INT_MAX)) return false;
    const auto wide_count = MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED, expected_name.data(), static_cast<int>(expected_name.size()), nullptr, 0);
    if (wide_count <= 0) return false;
    std::wstring expected(static_cast<std::size_t>(wide_count), L'\0');
    if (MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED, expected_name.data(), static_cast<int>(expected_name.size()), expected.data(), wide_count) != wide_count)
        return false;
    IMMDeviceEnumerator* enumerator{};
    IMMDeviceCollection* collection{};
    if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) ||
        FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection)))
    {
        release(collection);
        release(enumerator);
        return false;
    }
    UINT count{};
    collection->GetCount(&count);
    UINT matches{};
    for (UINT index = 0U; index < count; ++index)
    {
        IMMDevice* candidate{};
        std::wstring name;
        if (SUCCEEDED(collection->Item(index, &candidate)) && friendly_name(candidate, name) && name == expected)
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
        release(candidate);
    }
    release(collection);
    release(enumerator);
    if (matches != 1U || endpoint_id.empty()) return false;
    selected_name = expected_name;
    return true;
}

bool is_pcm(const WAVEFORMATEX& format) noexcept
{
    constexpr GUID pcm_subformat{0x00000001L, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
    return format.wFormatTag == WAVE_FORMAT_PCM ||
           (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            IsEqualGUID(reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format).SubFormat, pcm_subformat));
}

bool is_float(const WAVEFORMATEX& format) noexcept
{
    constexpr GUID float_subformat{0x00000003L, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
    return format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
           (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            IsEqualGUID(reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format).SubFormat, float_subformat));
}

} // namespace

void WasapiCapture::set_error(const char* message) noexcept
{
    std::strncpy(error_, message, sizeof(error_) - 1U);
    error_[sizeof(error_) - 1U] = '\0';
}

std::uint32_t WasapiCapture::enumerate(DeviceVisitor visitor, void* context) noexcept
{
    const auto com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_result)) return 0U;
    IMMDeviceEnumerator* enumerator{};
    IMMDeviceCollection* collection{};
    if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) ||
        FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection)))
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
        std::wstring name;
        if (SUCCEEDED(collection->Item(index, &device)) && friendly_name(device, name) && visitor != nullptr)
        {
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

bool WasapiCapture::open(std::string_view expected_name, SamplesCallback callback, void* context,
                         ChannelPolicy channel_policy) noexcept
{
    if (state_ != nullptr || callback == nullptr || expected_name.empty())
    {
        set_error("invalid WASAPI capture arguments");
        return false;
    }
    const auto com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_result))
    {
        set_error("WASAPI COM initialization failed");
        return false;
    }
    std::wstring endpoint_id;
    std::string_view selected_name;
    const auto found = find_endpoint(expected_name, endpoint_id, selected_name);
    CoUninitialize();
    if (!found)
    {
        set_error("WASAPI endpoint name was not unique or active");
        return false;
    }
    auto* state = new (std::nothrow) State{};
    if (state == nullptr)
    {
        set_error("cannot allocate WASAPI capture state");
        return false;
    }
    state->endpoint_id = std::move(endpoint_id);
    if (!to_utf8(state->endpoint_id, endpoint_id_, sizeof(endpoint_id_)))
    {
        delete state;
        set_error("WASAPI endpoint ID is not representable as UTF-8");
        return false;
    }
    state_ = state;
    std::strncpy(native_name_, expected_name.data(), sizeof(native_name_) - 1U);
    native_name_[sizeof(native_name_) - 1U] = '\0';
    callback_ = callback;
    context_ = context;
    channel_policy_ = channel_policy;
    stopped_.store(false, std::memory_order_release);
    set_error("ok");
    return true;
}

bool WasapiCapture::start() noexcept
{
    if (state_ == nullptr || accepting_.load(std::memory_order_acquire) || state_->worker.joinable())
    {
        set_error("WASAPI capture is not startable");
        return false;
    }
    state_->packet_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    state_->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (state_->packet_event == nullptr || state_->stop_event == nullptr)
    {
        set_error("cannot create WASAPI capture events");
        if (state_->packet_event != nullptr) CloseHandle(state_->packet_event);
        if (state_->stop_event != nullptr) CloseHandle(state_->stop_event);
        state_->packet_event = nullptr;
        state_->stop_event = nullptr;
        return false;
    }
    stopped_.store(false, std::memory_order_release);
    accepting_.store(true, std::memory_order_release);
    stopping_.store(false, std::memory_order_release);
    worker_failed_.store(false, std::memory_order_release);
    state_->worker = std::thread{[this] { worker_main(); }};
    return true;
}

void WasapiCapture::stop() noexcept
{
    if (state_ == nullptr || stopped_.exchange(true, std::memory_order_acq_rel)) return;
    accepting_.store(false, std::memory_order_release);
    stopping_.store(true, std::memory_order_release);
    if (state_->stop_event != nullptr) SetEvent(state_->stop_event);
    if (state_->packet_event != nullptr) SetEvent(state_->packet_event);
    if (state_->worker.joinable()) state_->worker.join();
}

void WasapiCapture::close() noexcept
{
    stop();
    if (state_ == nullptr) return;
    if (state_->packet_event != nullptr) CloseHandle(state_->packet_event);
    if (state_->stop_event != nullptr) CloseHandle(state_->stop_event);
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
        worker_failed_.store(true, std::memory_order_release);
        set_error("WASAPI worker COM initialization failed");
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
                          SUCCEEDED(device->Activate(IID_IAudioClient2, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) &&
                          SUCCEEDED(client->GetMixFormat(&mix_format));
    if (!setup_ok || mix_format == nullptr || mix_format->nSamplesPerSec != 48000U ||
        (mix_format->nChannels != 1U && mix_format->nChannels != 2U) ||
        ((!is_pcm(*mix_format) || mix_format->wBitsPerSample != 16U) &&
         (!is_float(*mix_format) || mix_format->wBitsPerSample != 32U)))
    {
        worker_failed_.store(true, std::memory_order_release);
        set_error("WASAPI RAW mix format is not 48 kHz PCM16 or float mono/stereo");
        if (mix_format != nullptr) CoTaskMemFree(mix_format);
        release(capture); release(client); release(device); release(enumerator); CoUninitialize();
        accepting_.store(false, std::memory_order_release);
        return;
    }
    state->sample_rate = mix_format->nSamplesPerSec;
    state->channels = mix_format->nChannels;
    state->bits_per_sample = mix_format->wBitsPerSample;
    state->frame_bytes = mix_format->nBlockAlign;
    state->is_float = is_float(*mix_format);
    std::snprintf(format_name_, sizeof(format_name_), "%s_%s_%u",
                  state->is_float ? "IEEE_FLOAT32" : "PCM16_LE",
                  state->channels == 1U ? "MONO" : "STEREO", state->sample_rate);
    AudioClientProperties properties{};
    properties.cbSize = sizeof(properties);
    properties.Options = AUDCLNT_STREAMOPTIONS_RAW;
    const auto flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    const auto initialized = SUCCEEDED(client->SetClientProperties(&properties)) &&
                             SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 10'000'000, 0, mix_format, nullptr)) &&
                             SUCCEEDED(client->SetEventHandle(state->packet_event));
    UINT32 buffer_frames{};
    if (!initialized || FAILED(client->GetBufferSize(&buffer_frames)) || FAILED(client->GetService(IID_PPV_ARGS(&capture))))
    {
        worker_failed_.store(true, std::memory_order_release);
        set_error("WASAPI RAW initialization failed");
        CoTaskMemFree(mix_format);
        release(capture); release(client); release(device); release(enumerator); CoUninitialize();
        accepting_.store(false, std::memory_order_release);
        return;
    }
    state->packet.resize(static_cast<std::size_t>(buffer_frames) * state->frame_bytes);
    state->converted.resize(capture_block_samples);
    CoTaskMemFree(mix_format);
    if (FAILED(client->Start()))
    {
        worker_failed_.store(true, std::memory_order_release);
        set_error("WASAPI RAW capture start failed");
        release(capture); release(client); release(device); release(enumerator); CoUninitialize();
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
            worker_failed_.store(true, std::memory_order_release);
            set_error("WASAPI RAW event wait failed");
            break;
        }
        if (wait == WAIT_OBJECT_0 || stopping_.load(std::memory_order_acquire))
        {
            stopping = true;
            static_cast<void>(client->Stop());
        }
        UINT32 packet_frames{};
        for (;;)
        {
            const auto next_result = capture->GetNextPacketSize(&packet_frames);
            if (FAILED(next_result))
            {
                worker_failed_.store(true, std::memory_order_release);
                set_error("WASAPI RAW packet-size query failed");
                stopping = true;
                break;
            }
            if (packet_frames == 0U) break;
            BYTE* data{};
            DWORD packet_flags{};
            UINT32 frames{};
            UINT64 device_position{};
            UINT64 qpc_position{};
            if (FAILED(capture->GetBuffer(&data, &frames, &packet_flags, &device_position, &qpc_position)))
            {
                worker_failed_.store(true, std::memory_order_release);
                set_error("WASAPI RAW buffer acquisition failed");
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
                counters_.position_errors.fetch_add(1U, std::memory_order_relaxed);
            const auto available = std::min<std::size_t>(frames, state->packet.size() / state->frame_bytes);
            const auto silent = (packet_flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0U;
            if (silent) counters_.silent_packets.fetch_add(1U, std::memory_order_relaxed);
            if ((packet_flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0U)
            {
                counters_.data_discontinuity_packets.fetch_add(1U, std::memory_order_relaxed);
                if (counters_.input_buffers.load(std::memory_order_relaxed) == 0U)
                    counters_.startup_discontinuity_packets.fetch_add(1U, std::memory_order_relaxed);
            }
            if (silent || data == nullptr) std::fill(state->packet.begin(), state->packet.begin() + available * state->frame_bytes, std::byte{});
            else std::memcpy(state->packet.data(), data, available * state->frame_bytes);
            if (FAILED(capture->ReleaseBuffer(frames)))
            {
                worker_failed_.store(true, std::memory_order_release);
                set_error("WASAPI RAW buffer release failed");
                stopping = true;
                break;
            }
            counters_.input_buffers.fetch_add(1U, std::memory_order_relaxed);
            counters_.samples_delivered.fetch_add(available, std::memory_order_relaxed);
            if (stopping)
            {
                counters_.reset_buffers.fetch_add(1U, std::memory_order_relaxed);
                counters_.reset_samples.fetch_add(available, std::memory_order_relaxed);
                if (available != 0U && available < capture_block_samples)
                    counters_.reset_partial_buffers.fetch_add(1U, std::memory_order_relaxed);
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
                if (!callback_(context_, std::span<const float>{state->converted.data(), chunk}))
                {
                    counters_.dropped_blocks.fetch_add(1U, std::memory_order_relaxed);
                    accepting_.store(false, std::memory_order_release);
                    stopping = true;
                    static_cast<void>(client->Stop());
                    break;
                }
            }
            if (stopping && !accepting_.load(std::memory_order_acquire)) break;
        }
        if (stopping) break;
    }
    static_cast<void>(client->Stop());
    accepting_.store(false, std::memory_order_release);
    release(capture); release(client); release(device); release(enumerator); CoUninitialize();
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

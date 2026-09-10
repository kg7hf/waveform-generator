#include "native-m110/source.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>

namespace native_m110
{

waveform_source::Encoder* create_encoder(void* workspace, std::size_t bytes) noexcept
{
    if (workspace == nullptr || bytes < sizeof(Source) || reinterpret_cast<std::uintptr_t>(workspace) % alignof(Source) != 0U) { return nullptr; }
    return new (workspace) Source{};
}

m110::Status Source::configure(std::string_view profile, ByteSource& payload, std::size_t payload_bytes) noexcept
{
    const auto delimiter = profile.find(':');
    if (delimiter == std::string_view::npos || profile.size() - delimiter - 1U >= 8U)
    {
        stop(); status_ = {m110::StatusCode::invalid_argument, "M110 profile must be rate:long|short|zero"}; return status_;
    }
    std::uint32_t rate{};
    const auto parsed = std::from_chars(profile.data(), profile.data() + delimiter, rate);
    char interleave[8]{};
    const auto length = profile.size() - delimiter - 1U;
    std::memcpy(interleave, profile.data() + delimiter + 1U, length);
    m110::BodyMode mode{};
    if (parsed.ec != std::errc{} || parsed.ptr != profile.data() + delimiter || !parse_mode(rate, interleave, mode))
    {
        stop(); status_ = {m110::StatusCode::invalid_argument, "unsupported M110 encoder profile"}; return status_;
    }
    return configure(mode, payload, payload_bytes, 0U);
}

std::size_t MemoryBytes::read(std::uint8_t* bytes, std::size_t capacity) noexcept
{
    if (bytes == nullptr) { return 0U; }
    const auto count = std::min(capacity, bytes_.size() - position_);
    if (count != 0U) { std::memcpy(bytes, bytes_.data() + position_, count); }
    position_ += count;
    return count;
}

void Source::stop() noexcept
{
    configured_ = false;
    payload_ = nullptr;
    total_frames_ = 0U;
    audio_count_ = audio_cursor_ = 0U;
    silence_remaining_ = 0U;
    status_ = {m110::StatusCode::invalid_configuration, "native source is not configured"};
}

m110::Status Source::configure(m110::BodyMode mode, ByteSource& payload, std::size_t payload_bytes,
                                std::uint32_t trailing_silence_frames) noexcept
{
    stop();
    const auto planned = m110::body_transmission_plan(mode, payload_bytes);
    if (!planned) { status_ = planned.status(); return status_; }
    const auto candidate = planned.value();
    if (candidate.block.information_bits > information_.size() || candidate.block.coded_bits > coded_.size() ||
        candidate.block.transmitted_symbols > segment_.size() || candidate.preamble_symbols > segment_.size())
    {
        status_ = {m110::StatusCode::buffer_too_small, "native mode exceeds the bounded TX workspace"};
        return status_;
    }
    if (candidate.audio_samples > std::numeric_limits<std::uint64_t>::max() - trailing_silence_frames)
    {
        status_ = {m110::StatusCode::invalid_argument, "native transmission frame count overflows"};
        return status_;
    }
    plan_ = candidate;
    encode_state_ = {};
    payload_ = &payload;
    payload_bytes_ = payload_bytes;
    fetched_bytes_ = payload_buffer_count_ = payload_buffer_cursor_ = 0U;
    octet_ = 0U;
    segment_index_ = 0U;
    window_first_ = window_size_ = next_symbol_ = rendered_symbols_ = 0U;
    silence_remaining_ = trailing_silence_frames;
    total_frames_ = static_cast<std::uint64_t>(plan_.audio_samples) + trailing_silence_frames;
    status_ = modulator_.initialize();
    if (!status_.is_ok()) { return status_; }
    status_ = m110::generate_body_preamble(mode, std::span<std::uint8_t>{segment_}.first(plan_.preamble_symbols));
    configured_ = status_.is_ok();
    return status_;
}

std::uint8_t Source::framed_bit(std::size_t index) noexcept
{
    if (index < plan_.payload_bits)
    {
        if (index % 8U == 0U)
        {
            if (payload_buffer_cursor_ == payload_buffer_count_)
            {
                const auto request = std::min(payload_buffer_.size(), payload_bytes_ - fetched_bytes_);
                const auto count = payload_->read(payload_buffer_.data(), request);
                if (count == 0U || count > request)
                {
                    status_ = {m110::StatusCode::io_error, "native payload ended before its declared length"};
                    return 0U;
                }
                fetched_bytes_ += count;
                payload_buffer_count_ = count;
                payload_buffer_cursor_ = 0U;
            }
            octet_ = payload_buffer_[payload_buffer_cursor_++];
        }
        return static_cast<std::uint8_t>((octet_ >> (index % 8U)) & 1U);
    }
    const auto eom_index = index - plan_.payload_bits;
    return eom_index < m110::body_eom_bits
        ? static_cast<std::uint8_t>((m110::body_eom_word >> (m110::body_eom_bits - eom_index - 1U)) & 1U) : 0U;
}

bool Source::encode_segment(std::size_t index) noexcept
{
    const auto information_offset = (index - 1U) * plan_.block.information_bits;
    for (std::size_t bit = 0U; bit < plan_.block.information_bits; ++bit)
    {
        information_[bit] = framed_bit(information_offset + bit);
        if (!status_.is_ok()) { return false; }
    }
    status_ = m110::encode_body_block(std::span<const std::uint8_t>{information_}.first(plan_.block.information_bits), plan_.block,
        {std::span<std::uint8_t>{coded_}.first(plan_.block.coded_bits), std::span<std::uint8_t>{matrix_}.first(plan_.block.coded_bits),
         std::span<std::uint8_t>{interleaved_}.first(plan_.block.coded_bits)},
        std::span<std::uint8_t>{segment_}.first(plan_.block.transmitted_symbols), encode_state_);
    if (status_.is_ok()) { segment_index_ = index; }
    return status_.is_ok();
}

bool Source::pull_symbol(std::uint8_t& value) noexcept
{
    std::size_t wanted_segment = 0U;
    std::size_t offset = next_symbol_;
    if (next_symbol_ >= plan_.preamble_symbols)
    {
        const auto body_symbol = next_symbol_ - plan_.preamble_symbols;
        wanted_segment = 1U + body_symbol / plan_.block.transmitted_symbols;
        offset = body_symbol % plan_.block.transmitted_symbols;
    }
    if (wanted_segment != segment_index_ && !encode_segment(wanted_segment)) { return false; }
    value = segment_[offset];
    ++next_symbol_;
    return true;
}

bool Source::refill_window(std::size_t output_symbols) noexcept
{
    // Same bounded shaping context as donor app/modem_transmitter.cpp. A segment
    // is copied into this window before its storage is reused for the next one.
    const auto desired_first = rendered_symbols_ > m110::body_audio_shaping_span_symbols
        ? rendered_symbols_ - m110::body_audio_shaping_span_symbols : 0U;
    if (desired_first < window_first_ || desired_first > window_first_ + window_size_)
    {
        status_ = {m110::StatusCode::internal_error, "native shaping window lost continuity"};
        return false;
    }
    const auto discard = desired_first - window_first_;
    if (discard != 0U)
    {
        std::memmove(window_.data(), window_.data() + discard, window_size_ - discard);
        window_size_ -= discard;
        window_first_ = desired_first;
    }
    const auto desired_end = std::min(plan_.transmitted_symbols, rendered_symbols_ + output_symbols + m110::body_audio_shaping_span_symbols);
    while (next_symbol_ < desired_end)
    {
        if (window_size_ == window_.size())
        {
            status_ = {m110::StatusCode::internal_error, "native shaping window overflow"};
            return false;
        }
        if (!pull_symbol(window_[window_size_])) { return false; }
        ++window_size_;
    }
    return true;
}

bool Source::render_next() noexcept
{
    const auto symbols = std::min(render_symbols, plan_.transmitted_symbols - rendered_symbols_);
    if (symbols == 0U || !refill_window(symbols)) { return false; }
    audio_count_ = symbols * m110::body_audio_samples_per_symbol;
    audio_cursor_ = 0U;
    status_ = modulator_.render_window(std::span<const std::uint8_t>{window_}.first(window_size_), rendered_symbols_ - window_first_,
                                     symbols, std::span<float>{audio_}.first(audio_count_));
    if (!status_.is_ok()) { audio_count_ = 0U; return false; }
    rendered_symbols_ += symbols;
    return true;
}

std::size_t Source::read_float(float* frames, std::size_t capacity) noexcept
{
    if (!configured_ || !status_.is_ok() || capacity == 0U) { return 0U; }
    if (frames == nullptr)
    {
        status_ = {m110::StatusCode::invalid_argument, "native float destination is null"};
        return 0U;
    }
    std::size_t produced = 0U;
    while (produced < capacity)
    {
        if (audio_cursor_ == audio_count_)
        {
            if (rendered_symbols_ == plan_.transmitted_symbols)
            {
                const auto zeros = std::min<std::size_t>(capacity - produced, silence_remaining_);
                std::fill_n(frames + produced, zeros, 0.0F);
                silence_remaining_ -= static_cast<std::uint32_t>(zeros);
                produced += zeros;
                break;
            }
            if (!render_next()) { break; }
        }
        const auto count = std::min(capacity - produced, audio_count_ - audio_cursor_);
        std::copy_n(audio_.data() + audio_cursor_, count, frames + produced);
        audio_cursor_ += count;
        produced += count;
    }
    return produced;
}

std::size_t Source::read(std::int16_t* frames, std::size_t capacity) noexcept
{
    if (!configured_ || !status_.is_ok() || capacity == 0U) { return 0U; }
    if (frames == nullptr)
    {
        status_ = {m110::StatusCode::invalid_argument, "native PCM16 destination is null"};
        return 0U;
    }
    std::array<float, 64U> source{};
    std::size_t produced = 0U;
    while (produced < capacity)
    {
        const auto count = read_float(source.data(), std::min(source.size(), capacity - produced));
        if (count == 0U) { break; }
        for (std::size_t index = 0U; index < count; ++index)
        {
            const auto scaled = std::clamp(source[index] * 32767.0F, -32768.0F, 32767.0F);
            frames[produced++] = static_cast<std::int16_t>(
                static_cast<double>(scaled) + (scaled < 0.0F ? -0.5 : 0.5));
        }
    }
    return produced;
}

bool parse_mode(std::uint32_t rate, const char* interleave, m110::BodyMode& mode) noexcept
{
    if (interleave == nullptr) { return false; }
    switch (rate)
    {
    case 75U: mode.data_rate = m110::DataRate::bps75; break;
    case 150U: mode.data_rate = m110::DataRate::bps150; break;
    case 300U: mode.data_rate = m110::DataRate::bps300; break;
    case 600U: mode.data_rate = m110::DataRate::bps600; break;
    case 1200U: mode.data_rate = m110::DataRate::bps1200; break;
    case 2400U: mode.data_rate = m110::DataRate::bps2400; break;
    case 4800U: mode.data_rate = m110::DataRate::bps4800; break;
    default: return false;
    }
    const std::string_view name{interleave};
    if (name == "long") { mode.interleave = m110::BodyInterleave::long_block; }
    else if (name == "short") { mode.interleave = m110::BodyInterleave::short_block; }
    else if (name == "zero") { mode.interleave = m110::BodyInterleave::zero; }
    else { return false; }
    return static_cast<bool>(m110::body_block_plan(mode));
}

} // namespace native_m110

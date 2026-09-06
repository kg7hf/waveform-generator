#include "waveform-source/wav_generator.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace waveform_source
{
namespace
{
void put16(std::uint8_t* bytes, std::uint16_t value) noexcept
{
    bytes[0] = static_cast<std::uint8_t>(value);
    bytes[1] = static_cast<std::uint8_t>(value >> 8U);
}
void put32(std::uint8_t* bytes, std::uint32_t value) noexcept
{
    for (std::size_t i = 0U; i < 4U; ++i) { bytes[i] = static_cast<std::uint8_t>(value >> (8U * i)); }
}
}

JobState WavGenerator::fail(m110::Status status) noexcept
{
    status_ = status;
    if (source_ != nullptr) { source_->stop(); }
    state_ = JobState::failed;
    return state_;
}

void WavGenerator::stop() noexcept
{
    if (source_ != nullptr && state_ == JobState::running) { source_->stop(); }
    source_ = nullptr;
    payload_ = nullptr;
    sink_ = nullptr;
    state_ = JobState::idle;
    status_ = m110::Status::success();
}

m110::Status WavGenerator::begin(Encoder& source, std::string_view profile, ByteSource& payload, std::size_t payload_bytes,
                                 ByteSink& sink, std::uint32_t trailing_silence_frames) noexcept
{
    stop();
    source_ = &source;
    payload_ = &payload;
    sink_ = &sink;
    payload_bytes_ = payload_bytes;
    payload_read_ = 0U;
    frames_written_ = total_frames_ = 0U;
    payload_digest_ = {};
    wav_digest_ = {};
    pcm_digest_ = {};
    const auto configured = source.configure(profile, *this, payload_bytes);
    if (!configured.is_ok()) { (void)fail(configured); return status_; }
    waveform_frames_ = source.total_frames();
    constexpr auto maximum_frames = (std::numeric_limits<std::uint32_t>::max() - 36U) / 2U;
    if (waveform_frames_ > maximum_frames || trailing_silence_frames > maximum_frames - waveform_frames_)
    {
        (void)fail({m110::StatusCode::invalid_argument, "native WAV exceeds the RIFF size limit"});
        return status_;
    }
    total_frames_ = waveform_frames_ + trailing_silence_frames;
    const auto data_bytes = static_cast<std::uint32_t>(total_frames_ * 2U);
    std::array<std::uint8_t, 44U> header{};
    std::memcpy(header.data(), "RIFF", 4U); put32(header.data() + 4U, 36U + data_bytes);
    std::memcpy(header.data() + 8U, "WAVEfmt ", 8U); put32(header.data() + 16U, 16U);
    put16(header.data() + 20U, 1U); put16(header.data() + 22U, 1U); put32(header.data() + 24U, 48000U);
    put32(header.data() + 28U, 96000U); put16(header.data() + 32U, 2U); put16(header.data() + 34U, 16U);
    std::memcpy(header.data() + 36U, "data", 4U); put32(header.data() + 40U, data_bytes);
    if (!sink_->write(header.data(), header.size()))
    {
        (void)fail({m110::StatusCode::io_error, "failed to write native WAV header"}); return status_;
    }
    wav_digest_.update(header.data(), header.size());
    state_ = total_frames_ == 0U && payload_read_ == payload_bytes_ ? JobState::complete : JobState::running;
    return status_;
}

std::size_t WavGenerator::read(std::uint8_t* bytes, std::size_t capacity) noexcept
{
    const auto count = payload_->read(bytes, capacity);
    if (count > capacity || count > payload_bytes_ - payload_read_) { return 0U; }
    payload_read_ += count;
    payload_digest_.update(bytes, count);
    return count;
}

JobState WavGenerator::step(std::size_t max_frames) noexcept
{
    if (state_ != JobState::running || max_frames == 0U) { return state_; }
    const auto limit = static_cast<std::size_t>(std::min<std::uint64_t>(std::min(max_frames, frames_.size()), total_frames_ - frames_written_));
    std::size_t count{};
    if (frames_written_ < waveform_frames_)
    {
        const auto request = static_cast<std::size_t>(std::min<std::uint64_t>(limit, waveform_frames_ - frames_written_));
        count = source_->read(frames_.data(), request);
        if (!source_->status().is_ok()) { return fail(source_->status()); }
        if (count == 0U || count > request) { return fail({m110::StatusCode::io_error, "encoder did not honor its declared frame count"}); }
    }
    if (frames_written_ + count == waveform_frames_ || frames_written_ >= waveform_frames_)
    {
        std::fill(frames_.begin() + static_cast<std::ptrdiff_t>(count), frames_.begin() + static_cast<std::ptrdiff_t>(limit), std::int16_t{});
        count = limit;
    }
    if (count == 0U) { return fail({m110::StatusCode::io_error, "encoder ended without consuming its declared payload"}); }
    for (std::size_t i = 0U; i < count; ++i) { put16(bytes_.data() + 2U * i, static_cast<std::uint16_t>(frames_[i])); }
    if (!sink_->write(bytes_.data(), count * 2U)) { return fail({m110::StatusCode::io_error, "failed to write native WAV PCM"}); }
    wav_digest_.update(bytes_.data(), count * 2U);
    pcm_digest_.update(bytes_.data(), count * 2U);
    frames_written_ += count;
    if (frames_written_ == total_frames_)
    {
        if (payload_read_ != payload_bytes_) { return fail({m110::StatusCode::io_error, "native payload consumption was incomplete"}); }
        state_ = JobState::complete;
    }
    return state_;
}

ArtifactHashes WavGenerator::hashes() const noexcept
{
    ArtifactHashes result;
    if (state_ == JobState::complete)
    {
        payload_digest_.hex(result.payload);
        wav_digest_.hex(result.wav);
        pcm_digest_.hex(result.pcm);
    }
    return result;
}

} // namespace waveform_source

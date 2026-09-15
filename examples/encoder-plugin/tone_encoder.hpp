// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#pragma once
#include "waveform-source/encoder.hpp"
#include <charconv>
#include <cmath>

namespace waveform_example
{
// Demonstration adapter, not a communications modulation. Each payload byte
// advances 480 frames (10 ms); its value is ignored. Payload I/O is still checked.
// All state belongs to this object and each read consumes at most 2048 frames.
class ToneEncoder : public waveform_source::Encoder
{
public:
    [[nodiscard]] waveform_source::Status configure(std::string_view profile,
            waveform_source::ByteSource& payload, std::size_t bytes) noexcept override
    {
        stop();

        if (profile.empty())
        {
            return {m110::StatusCode::invalid_argument, "BAD_TONE_PROFILE_OR_LENGTH"};
        }

        unsigned int frequency{};
        const auto parsed = std::from_chars(profile.data(), profile.data() + profile.size(), frequency);

        if (parsed.ec != std::errc{} || parsed.ptr != profile.data() + profile.size() ||
                frequency == 0U || frequency > 20000U || bytes == 0U || bytes > 1048576U)
        {
            status_ = {m110::StatusCode::invalid_argument, "BAD_TONE_PROFILE_OR_LENGTH"};
            return status_;
        }
        payload_ = &payload;
        frequency_ = frequency;
        total_ = static_cast<std::uint64_t>(bytes) * 480U;
        status_ = waveform_source::Status::success();
        return status_;
    }
    [[nodiscard]] std::size_t read_float(float* frames, std::size_t capacity) noexcept override
    {
        if (frames == nullptr || payload_ == nullptr || !status_.is_ok())
        {
            return 0U;
        }

        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
                               std::min(capacity, std::size_t{2048U}), total_ - position_));

        for (std::size_t i = 0U; i < count; ++i)
        {
            if (position_ % 480U == 0U)
            {
                std::uint8_t byte{};

                if (payload_->read(&byte, 1U) != 1U)
                {
                    status_ = {m110::StatusCode::io_error, "PAYLOAD_READ_ERROR"};
                    return i;
                }
            }

            const auto phase = (position_ * frequency_) % 48000U;
            frames[i] = 0.125F * std::sin(6.2831853071795864769F * static_cast<float>(phase) / 48000.0F);
            ++position_;
        }

        return count;
    }
    [[nodiscard]] std::size_t read(std::int16_t* frames, std::size_t capacity) noexcept override
    {
        if (frames == nullptr)
        {
            return 0U;
        }

        std::array<float, 64U> block{};
        const auto count = read_float(block.data(), std::min(capacity, block.size()));

        for (std::size_t i = 0U; i < count; ++i)
        {
            frames[i] = static_cast<std::int16_t>(block[i] * 32768.0F);
        }

        return count;
    }
    [[nodiscard]] std::uint64_t total_frames() const noexcept override
    {
        return total_;
    }
    [[nodiscard]] waveform_source::Status status() const noexcept override
    {
        return status_;
    }
    void stop() noexcept override
    {
        payload_ = nullptr;
        position_ = total_ = 0U;
        status_ = {m110::StatusCode::invalid_configuration, "TONE_NOT_CONFIGURED"};
    }
private:
    waveform_source::ByteSource* payload_{};
    std::uint64_t total_{};
    std::uint64_t position_{};
    unsigned int frequency_{};
    waveform_source::Status status_{m110::StatusCode::invalid_configuration, "TONE_NOT_CONFIGURED"};
};
}

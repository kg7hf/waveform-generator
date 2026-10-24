// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

#include "modem/m110a/transmitter.hpp"
#include "waveform-source/encoder.hpp"

#include <array>
#include <limits>
#include <span>

namespace waveform_example
{
// Bounded adapter: modem-core owns framing, FEC, preamble and pulse shaping.
// This object only feeds payload blocks and holds the adjacent symbol windows.
class M110Encoder final : public waveform_source::Encoder
{
public:
    [[nodiscard]] waveform_source::Status configure(std::string_view profile, waveform_source::ByteSource& payload, std::size_t payload_bytes) noexcept override;
    [[nodiscard]] std::size_t read_float(float* frames, std::size_t capacity) noexcept override;
    [[nodiscard]] m110::Result<std::size_t> read(std::int16_t* frames, std::size_t capacity) noexcept override;
    [[nodiscard]] std::uint64_t total_frames() const noexcept override { return mTotalFrames; }
    [[nodiscard]] waveform_source::Status status() const noexcept override { return mStatus; }
    void stop() noexcept override;

private:
    [[nodiscard]] m110::Result<std::uint8_t> read_payload_bit() noexcept;
    [[nodiscard]] waveform_source::Status generate_next() noexcept;
    [[nodiscard]] waveform_source::Status render_symbol() noexcept;
    [[nodiscard]] std::span<std::uint8_t> symbol_buffer(std::size_t index) noexcept;
    void advance_block() noexcept;

    static constexpr std::size_t max_symbols = modem::m110a::body_scratch_max.transmitted_symbols;
    static constexpr std::size_t max_coded = modem::m110a::body_scratch_max.coded_bits;
    static constexpr std::size_t max_information = modem::m110a::body_scratch_max.information_bits;
    static constexpr std::size_t context = modem::m110a::body_audio_shaping_span_symbols;
    static constexpr std::size_t samples_per_symbol = modem::m110a::body_audio_samples_per_symbol;
    static constexpr std::uint8_t bits_per_octet = std::numeric_limits<std::uint8_t>::digits;
    static constexpr std::size_t pcm16_block_frames = 64U;
    static constexpr float pcm16_scale = 32768.0F;

    modem::m110a::BodyTransmissionPlan mPlan{};
    modem::m110a::BodyEncodeState mEncodeState{};
    modem::m110a::BodyAudioStreamModulator mModulator{};
    waveform_source::ByteSource* mPayload{};
    waveform_source::Status mStatus{m110::StatusCode::invalid_configuration, "M110_NOT_CONFIGURED"};
    std::uint64_t mTotalFrames{};
    std::uint64_t mPosition{};
    std::size_t mPayloadBitsLeft{};
    std::size_t mBlocksGenerated{};
    std::size_t mTailBlocks{};
    std::size_t mTailIndex{};
    std::size_t mCurrentCount{};
    std::size_t mNextCount{};
    std::size_t mCurrentSymbol{};
    std::size_t mPreviousCount{};
    std::size_t mSampleIndex{samples_per_symbol};
    std::size_t mActive{};
    std::uint8_t mPayloadOctet{};
    std::uint8_t mPayloadOctetBit{bits_per_octet};
    bool mConfigured{};

    std::array<std::uint8_t, 2U * max_information> mInformation{};
    std::array<std::uint8_t, max_coded> mCoded{};
    std::array<std::uint8_t, max_coded> mMatrix{};
    std::array<std::uint8_t, max_coded> mInterleaved{};
    std::array<std::array<std::uint8_t, max_symbols>, 2U> mSymbols{};
    std::array<std::uint8_t, context> mPrevious{};
    std::array<std::uint8_t, 2U * context + 1U> mWindow{};
    std::array<float, samples_per_symbol> mSamples{};
};
} // namespace waveform_example

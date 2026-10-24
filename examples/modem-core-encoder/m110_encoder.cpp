// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "m110_encoder.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <span>

namespace waveform_example
{
namespace
{
[[nodiscard]] waveform_source::Status adapt(modem::common::Status status) noexcept
{
    using From = modem::common::StatusCode;
    using To = m110::StatusCode;
    switch (status.code)
    {
    case From::ok:
        return waveform_source::Status::success();
    case From::invalid_argument:
        return {To::invalid_argument, status.message};
    case From::invalid_configuration:
        return {To::invalid_configuration, status.message};
    case From::buffer_too_small:
        return {To::buffer_too_small, status.message};
    case From::unavailable:
        return {To::unavailable, status.message};
    case From::io_error:
        return {To::io_error, status.message};
    case From::timeout:
        return {To::timeout, status.message};
    case From::busy:
        return {To::busy, status.message};
    case From::internal_error:
        return {To::internal_error, status.message};
    }
    return {To::internal_error, "UNKNOWN_MODEM_STATUS"};
}

[[nodiscard]] m110::Result<modem::m110a::BodyMode> parse_mode(std::string_view profile) noexcept
{
    if (profile.size() < 2U)
    {
        return waveform_source::Status{m110::StatusCode::invalid_argument, "BAD_M110_PROFILE"};
    }
    modem::m110a::BodyMode mode{};
    const auto suffix = profile.back();
    unsigned rate{};
    const auto result = std::from_chars(profile.data(), profile.data() + profile.size() - 1U, rate);
    if (result.ec != std::errc{} || result.ptr != profile.data() + profile.size() - 1U)
    {
        return waveform_source::Status{m110::StatusCode::invalid_argument, "BAD_M110_PROFILE"};
    }
    using modem::common::BodyInterleave;
    using modem::common::DataRate;
    switch (rate)
    {
    case static_cast<unsigned>(DataRate::bps75):
        mode.data_rate = DataRate::bps75;
        break;
    case static_cast<unsigned>(DataRate::bps150):
        mode.data_rate = DataRate::bps150;
        break;
    case static_cast<unsigned>(DataRate::bps300):
        mode.data_rate = DataRate::bps300;
        break;
    case static_cast<unsigned>(DataRate::bps600):
        mode.data_rate = DataRate::bps600;
        break;
    case static_cast<unsigned>(DataRate::bps1200):
        mode.data_rate = DataRate::bps1200;
        break;
    case static_cast<unsigned>(DataRate::bps2400):
        mode.data_rate = DataRate::bps2400;
        break;
    case static_cast<unsigned>(DataRate::bps4800):
        mode.data_rate = DataRate::bps4800;
        break;
    default:
        return waveform_source::Status{m110::StatusCode::invalid_argument, "BAD_M110_PROFILE"};
    }
    if (mode.data_rate == DataRate::bps4800)
    {
        if (suffix != 'Z')
        {
            return waveform_source::Status{m110::StatusCode::invalid_argument, "BAD_M110_PROFILE"};
        }
        mode.interleave = BodyInterleave::zero;
        return mode;
    }
    if (suffix != 'S' && suffix != 'L')
    {
        return waveform_source::Status{m110::StatusCode::invalid_argument, "BAD_M110_PROFILE"};
    }
    mode.interleave = suffix == 'S' ? BodyInterleave::short_block : BodyInterleave::long_block;
    return mode;
}
} // namespace

void M110Encoder::stop() noexcept
{
    mPayload = nullptr;
    mStatus = {m110::StatusCode::invalid_configuration, "M110_NOT_CONFIGURED"};
    mTotalFrames = mPosition = 0U;
    mPayloadBitsLeft = mBlocksGenerated = mTailBlocks = mTailIndex = 0U;
    mCurrentCount = mNextCount = mCurrentSymbol = mPreviousCount = mActive = 0U;
    mSampleIndex = samples_per_symbol;
    mPayloadOctetBit = bits_per_octet;
    mConfigured = false;
    mPrevious.fill(0U);
    mModulator.reset();
}

waveform_source::Status M110Encoder::configure(std::string_view profile, waveform_source::ByteSource& payload, std::size_t payload_bytes) noexcept
{
    stop();
    const auto mode = parse_mode(profile);
    if (!mode)
    {
        mStatus = mode.error();
        return mStatus;
    }
    const auto planned = modem::m110a::body_transmission_plan(mode.value(), payload_bytes);
    if (!planned)
    {
        mStatus = adapt(planned.error());
        return mStatus;
    }
    mPlan = planned.value();
    if (mPlan.body_blocks == 0U || mPlan.preamble_symbols < context || mPlan.block.transmitted_symbols < context || mPlan.block.information_bits == 0U || mPlan.preamble_symbols > max_symbols ||
        mPlan.block.transmitted_symbols > max_symbols || mPlan.block.coded_bits > max_coded || mPlan.block.information_bits > max_information ||
        mPlan.audio_samples > std::numeric_limits<std::uint32_t>::max() / 3U)
    {
        mStatus = {m110::StatusCode::invalid_argument, "M110_BURST_TOO_LARGE"};
        return mStatus;
    }
    mStatus = adapt(mModulator.initialize());
    if (!mStatus.is_ok())
    {
        return mStatus;
    }
    mPayload = &payload;
    mPayloadBitsLeft = mPlan.payload_bits;
    mTotalFrames = mPlan.audio_samples;
    mStatus = adapt(modem::m110a::generate_body_preamble(mode.value(), symbol_buffer(mActive).first(mPlan.preamble_symbols)));
    if (!mStatus.is_ok())
    {
        mPayload = nullptr;
        return mStatus;
    }
    mCurrentCount = mPlan.preamble_symbols;
    mStatus = generate_next();
    if (!mStatus.is_ok())
    {
        mPayload = nullptr;
        return mStatus;
    }
    mConfigured = true;
    return mStatus;
}

m110::Result<std::uint8_t> M110Encoder::read_payload_bit() noexcept
{
    if (mPayloadBitsLeft == 0U)
    {
        return waveform_source::Status{m110::StatusCode::internal_error, "PAYLOAD_PAST_END"};
    }
    if (mPayloadOctetBit == bits_per_octet)
    {
        if (mPayload->read(&mPayloadOctet, 1U) != 1U)
        {
            return waveform_source::Status{m110::StatusCode::io_error, "PAYLOAD_READ_ERROR"};
        }
        mPayloadOctetBit = 0U;
    }
    const auto bit = static_cast<std::uint8_t>((mPayloadOctet >> mPayloadOctetBit) & 1U);
    ++mPayloadOctetBit;
    --mPayloadBitsLeft;
    return bit;
}

std::span<std::uint8_t> M110Encoder::symbol_buffer(std::size_t index) noexcept
{
    return std::span{std::span{mSymbols}[index]};
}

waveform_source::Status M110Encoder::generate_next() noexcept
{
    mNextCount = 0U;
    if (mBlocksGenerated == mPlan.body_blocks)
    {
        return waveform_source::Status::success();
    }
    const auto count = mPlan.block.information_bits;
    const auto information_storage = std::span{mInformation};
    const auto coded_storage = std::span{mCoded};
    std::span<const std::uint8_t> information;
    if (mTailIndex < mTailBlocks)
    {
        information = information_storage.subspan(mTailIndex * count, count);
        ++mTailIndex;
    }
    else if (mPayloadBitsLeft >= count)
    {
        for (std::size_t i = 0U; i < count; ++i)
        {
            const auto bit = read_payload_bit();
            if (!bit)
            {
                return bit.error();
            }
            information_storage[i] = bit.value();
        }
        information = information_storage.first(count);
    }
    else
    {
        const auto remainder = mPayloadBitsLeft;
        for (std::size_t i = 0U; i < remainder; ++i)
        {
            const auto bit = read_payload_bit();
            if (!bit)
            {
                return bit.error();
            }
            coded_storage[i] = bit.value();
        }
        // Reuse the coding scratch for the short final payload before encoding
        // begins. Keep it separate from the framed output; the public framing
        // API makes no aliasing promise.
        const auto framed = modem::m110a::append_body_eom_and_flush(coded_storage.first(remainder), count, information_storage);
        if (!framed)
        {
            return adapt(framed.error());
        }
        if (framed.value() == 0U || framed.value() > information_storage.size() || framed.value() % count != 0U)
        {
            return {m110::StatusCode::internal_error, "M110_INVALID_FRAME_RESULT"};
        }
        mTailBlocks = framed.value() / count;
        mTailIndex = 1U;
        information = information_storage.first(count);
    }
    if (mTailBlocks != 0U && mBlocksGenerated + mTailBlocks - mTailIndex + 1U != mPlan.body_blocks)
    {
        return {m110::StatusCode::internal_error, "M110_FRAME_COUNT_MISMATCH"};
    }
    const auto free = 1U - mActive;
    const auto encoded = adapt(modem::m110a::encode_body_block(information, mPlan.block, {mCoded, mMatrix, mInterleaved}, symbol_buffer(free).first(mPlan.block.transmitted_symbols), mEncodeState));
    if (!encoded.is_ok())
    {
        return encoded;
    }
    mNextCount = mPlan.block.transmitted_symbols;
    ++mBlocksGenerated;
    return waveform_source::Status::success();
}

void M110Encoder::advance_block() noexcept
{
    const auto current = symbol_buffer(mActive).subspan(mCurrentCount - context, context);
    std::ranges::copy(current, mPrevious.begin());
    mPreviousCount = context;
    mActive = 1U - mActive;
    mCurrentCount = mNextCount;
    mCurrentSymbol = 0U;
}

waveform_source::Status M110Encoder::render_symbol() noexcept
{
    if (mCurrentSymbol == mCurrentCount)
    {
        advance_block();
        if (const auto generated = generate_next(); !generated.is_ok())
        {
            return generated;
        }
    }
    const auto left = std::min(context, mCurrentSymbol + mPreviousCount);
    const auto right = std::min(context, mCurrentCount - mCurrentSymbol - 1U + mNextCount);
    const auto window = std::span{mWindow};
    const auto previous = std::span{mPrevious};
    const auto current = symbol_buffer(mActive);
    const auto next = symbol_buffer(1U - mActive);
    for (std::size_t i = 0U; i < left + 1U + right; ++i)
    {
        const auto relative = static_cast<std::ptrdiff_t>(mCurrentSymbol) + static_cast<std::ptrdiff_t>(i) - static_cast<std::ptrdiff_t>(left);
        if (relative < 0)
        {
            window[i] = previous[static_cast<std::size_t>(relative + static_cast<std::ptrdiff_t>(context))];
        }
        else if (static_cast<std::size_t>(relative) < mCurrentCount)
        {
            window[i] = current[static_cast<std::size_t>(relative)];
        }
        else
        {
            const auto following = static_cast<std::size_t>(relative) - mCurrentCount;
            window[i] = following < mNextCount ? next[following] : 0U;
        }
    }
    if (const auto rendered = adapt(mModulator.render_window(window.first(left + 1U + right), left, 1U, mSamples)); !rendered.is_ok())
    {
        return rendered;
    }
    ++mCurrentSymbol;
    mSampleIndex = 0U;
    return waveform_source::Status::success();
}

std::size_t M110Encoder::read_float(float* frames, std::size_t capacity) noexcept
{
    if (frames == nullptr || !mConfigured || !mStatus.is_ok())
    {
        return 0U;
    }
    const std::span<float> output{frames, capacity};
    std::size_t produced{};
    while (produced < output.size() && mPosition < mTotalFrames)
    {
        if (mSampleIndex == samples_per_symbol)
        {
            mStatus = render_symbol();
            if (!mStatus.is_ok())
            {
                return produced;
            }
        }
        output[produced++] = std::span{mSamples}[mSampleIndex++];
        ++mPosition;
    }
    return produced;
}

m110::Result<std::size_t> M110Encoder::read(std::int16_t* frames, std::size_t capacity) noexcept
{
    if (frames == nullptr)
    {
        return waveform_source::Status{m110::StatusCode::invalid_argument, "NO_FRAME_BUFFER"};
    }
    std::array<float, pcm16_block_frames> block{};
    const auto count = read_float(block.data(), std::min(capacity, block.size()));
    if (!mStatus.is_ok())
    {
        return mStatus;
    }
    const std::span<std::int16_t> output{frames, capacity};
    for (std::size_t i = 0U; i < count; ++i)
    {
        output[i] = static_cast<std::int16_t>(std::clamp(std::span{block}[i], -1.0F, 1.0F - 1.0F / pcm16_scale) * pcm16_scale);
    }
    return count;
}
} // namespace waveform_example

// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "modem/m110a/transmitter.hpp"
#include "waveform-source/encoder_registry.hpp"
#include "waveform-source/wav_generator.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <span>
#include <string_view>
#include <vector>

namespace
{
class Payload final : public waveform_source::ByteSource
{
public:
    explicit Payload(std::span<const std::uint8_t> data) noexcept : data_{data} {}
    std::size_t read(std::uint8_t* bytes, std::size_t capacity) noexcept override
    {
        const auto count = std::min(capacity, data_.size() - position_);
        std::copy_n(data_.data() + position_, count, bytes);
        position_ += count;
        return count;
    }
    [[nodiscard]] std::size_t position() const noexcept { return position_; }

private:
    std::span<const std::uint8_t> data_{};
    std::size_t position_{};
};

class WavSink final : public waveform_source::ByteSink
{
public:
    std::vector<std::uint8_t> data{};
    m110::Status write(const std::uint8_t* bytes, std::size_t count) noexcept override
    {
        data.insert(data.end(), bytes, bytes + count);
        return m110::Status::success();
    }
};

struct Case
{
    std::string_view profile;
    modem::m110a::BodyMode mode;
    std::size_t payload_bytes;
};

[[nodiscard]] bool compare(const Case& item, waveform_source::Encoder& encoder)
{
    std::vector<std::uint8_t> octets(item.payload_bytes);
    for (std::size_t i = 0U; i < octets.size(); ++i)
    {
        octets[i] = static_cast<std::uint8_t>(i * 53U + 17U);
    }
    Payload payload{octets};
    if (!encoder.configure(item.profile, payload, octets.size()).is_ok())
    {
        std::printf("configure failed: %.*s %zu: %s\n", static_cast<int>(item.profile.size()), item.profile.data(), item.payload_bytes, encoder.status().message);
        return false;
    }
    const auto planned = modem::m110a::body_transmission_plan(item.mode, octets.size());
    if (!planned || encoder.total_frames() != planned.value().audio_samples)
    {
        return false;
    }
    const auto plan = planned.value();
    std::vector<std::uint8_t> framed(plan.framed_bits);
    std::vector<std::uint8_t> symbols(plan.transmitted_symbols);
    std::vector<std::uint8_t> coded(plan.block.coded_bits);
    std::vector<std::uint8_t> matrix(plan.block.coded_bits);
    std::vector<std::uint8_t> interleaved(plan.block.coded_bits);
    std::vector<float> expected(plan.audio_samples);
    if (!modem::m110a::generate_body_transmission_audio(plan, octets, {framed, symbols, coded, matrix, interleaved}, expected).is_ok())
    {
        return false;
    }
    std::array<float, 37U> actual{};
    std::size_t offset{};
    while (offset < expected.size())
    {
        const auto count = encoder.read_float(actual.data(), std::min(actual.size(), expected.size() - offset));
        if (count == 0U || !encoder.status().is_ok())
        {
            std::printf("read failed at frame %zu: %s\n", offset, encoder.status().message);
            return false;
        }
        for (std::size_t i = 0U; i < count; ++i)
        {
            if (std::bit_cast<std::uint32_t>(actual[i]) != std::bit_cast<std::uint32_t>(expected[offset + i]))
            {
                std::printf("sample mismatch: %.*s bytes=%zu frame=%zu\n", static_cast<int>(item.profile.size()), item.profile.data(), item.payload_bytes, offset + i);
                return false;
            }
        }
        offset += count;
    }
    const bool completed = encoder.read_float(actual.data(), actual.size()) == 0U && encoder.status().is_ok() && payload.position() == octets.size();
    encoder.stop();
    return completed;
}
} // namespace

TEST_CASE("M110 encoder matches public modem-core and produces PCM24", "[encoder]")
{
    using namespace modem::common;
    const auto* descriptor = waveform_generator::find_encoder("M110");
    REQUIRE(descriptor != nullptr);
    REQUIRE(descriptor->workspace_bytes <= waveform_generator::encoder_workspace_capacity);
    alignas(waveform_generator::encoder_workspace_alignment) static std::array<std::byte, waveform_generator::encoder_workspace_capacity> workspace{};
    REQUIRE(descriptor->construct(nullptr, descriptor->workspace_bytes) == nullptr);
    REQUIRE(descriptor->construct(workspace.data(), descriptor->workspace_bytes - 1U) == nullptr);
    if (descriptor->workspace_alignment > 1U)
    {
        REQUIRE(descriptor->construct(workspace.data() + 1U, workspace.size() - 1U) == nullptr);
    }
    auto* encoder = descriptor->construct(workspace.data(), workspace.size());
    REQUIRE(encoder != nullptr);
    constexpr std::array cases{
        Case{"75S", {DataRate::bps75, BodyInterleave::short_block}, 0U},    Case{"75S", {DataRate::bps75, BodyInterleave::short_block}, 7U},
        Case{"75L", {DataRate::bps75, BodyInterleave::long_block}, 46U},    Case{"150S", {DataRate::bps150, BodyInterleave::short_block}, 12U},
        Case{"150L", {DataRate::bps150, BodyInterleave::long_block}, 1U},   Case{"300S", {DataRate::bps300, BodyInterleave::short_block}, 13U},
        Case{"300L", {DataRate::bps300, BodyInterleave::long_block}, 181U}, Case{"600S", {DataRate::bps600, BodyInterleave::short_block}, 46U},
        Case{"600L", {DataRate::bps600, BodyInterleave::long_block}, 23U},  Case{"1200S", {DataRate::bps1200, BodyInterleave::short_block}, 89U},
        Case{"1200L", {DataRate::bps1200, BodyInterleave::long_block}, 1U}, Case{"2400S", {DataRate::bps2400, BodyInterleave::short_block}, 181U},
        Case{"2400L", {DataRate::bps2400, BodyInterleave::long_block}, 1U}, Case{"4800Z", {DataRate::bps4800, BodyInterleave::zero}, 361U},
    };
    for (const auto& item : cases)
    {
        INFO("profile=" << item.profile << " payload_bytes=" << item.payload_bytes);
        REQUIRE(compare(item, *encoder));
    }
    Payload empty{std::span<const std::uint8_t>{}};
    REQUIRE_FALSE(encoder->configure("2400X", empty, 0U).is_ok());
    REQUIRE_FALSE(encoder->configure("4800S", empty, 0U).is_ok());
    const std::array<std::uint8_t, 3U> bytes{0x12U, 0xA5U, 0x77U};
    Payload payload{bytes};
    WavSink sink;
    waveform_source::WavGenerator writer;
    REQUIRE(writer.begin(*encoder, "2400S", payload, bytes.size(), sink).is_ok());
    while (writer.state() == waveform_source::JobState::running)
    {
        (void)writer.step();
    }
    REQUIRE(writer.state() == waveform_source::JobState::complete);
    REQUIRE(sink.data.size() == 44U + writer.total_frames() * 3U);
    REQUIRE(sink.data[34U] == 24U);
    REQUIRE(payload.position() == bytes.size());
    writer.stop();
    const std::array<std::uint8_t, 1U> short_bytes{0x12U};
    Payload short_payload{short_bytes};
    REQUIRE_FALSE(encoder->configure("2400S", short_payload, 3U).is_ok());
    REQUIRE(encoder->status().code == m110::StatusCode::io_error);
    REQUIRE(compare(cases[1U], *encoder));
    encoder->~Encoder();
}

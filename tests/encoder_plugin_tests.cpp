// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#include "waveform-source/encoder_registry.hpp"
#include "waveform-source/wav_generator.hpp"
#include <array>
#include <cstdio>
#include <cstring>

namespace
{
class Bytes final : public waveform_source::ByteSource
{
public:
    std::size_t remaining{3U};
    std::size_t read(std::uint8_t* output, std::size_t capacity) noexcept override
    {
        const auto count = std::min(remaining, capacity);
        std::memset(output, 0x55, count);
        remaining -= count;
        return count;
    }
};
class Sink final : public waveform_source::ByteSink
{
public:
    std::array<std::uint8_t, 44U> header{};
    std::size_t bytes{};
    bool write(const std::uint8_t* input, std::size_t count) noexcept override
    {
        if (bytes == 0U && count >= header.size())
        {
            std::copy_n(input, header.size(), header.data());
        }

        bytes += count;
        return true;
    }
};
}
int main()
{
    using namespace waveform_generator;
    const auto encoders = registered_encoders();

    if (find_encoder("NOT_REGISTERED") != nullptr)
    {
        return 1;
    }

    #if defined(WFG_TEST_EXPECT_PLUGIN)

    if (encoders.empty())
    {
        return 2;
    }

    alignas(encoder_workspace_alignment) std::array<std::byte, encoder_workspace_capacity> workspace{};

    for (const auto& descriptor : encoders)
    {
        if (find_encoder(descriptor.id) != &descriptor || descriptor.construct(nullptr, 0U) != nullptr ||
                descriptor.construct(workspace.data(), 0U) != nullptr)
        {
            return 3;
        }

        auto* encoder = descriptor.construct(workspace.data(), workspace.size());

        if (encoder == nullptr)
        {
            return 4;
        }

        encoder->stop();
        encoder->~Encoder();
    }

    #else

    if (!encoders.empty() || find_encoder("") != nullptr)
    {
        return 5;
    }

    #endif
    #if defined(WFG_TEST_EXAMPLE_PLUGIN)
    const auto* descriptor = find_encoder("TONE");

    if (descriptor == nullptr || find_encoder("") != nullptr)
    {
        return 6;
    }

    auto* encoder = descriptor->construct(workspace.data(), workspace.size());
    Bytes payload;
    Sink output;
    waveform_source::WavGenerator writer;

    if (!writer.begin(*encoder, "1000", payload, 3U, output).is_ok())
    {
        return 7;
    }

    unsigned steps{};

    while (writer.state() == waveform_source::JobState::running && ++steps < 100U)
    {
        (void)writer.step();
    }

    if (writer.state() != waveform_source::JobState::complete || writer.total_frames() != 49440U ||
            output.bytes != 44U + 49440U * 3U || output.header[34U] != 24U || payload.remaining != 0U)
    {
        return 8;
    }

    writer.stop();
    payload.remaining = 0U;

    if (!writer.begin(*encoder, "1000", payload, 3U, output).is_ok())
    {
        return 9;
    }

    if (writer.step() != waveform_source::JobState::failed)
    {
        return 10;
    }

    writer.stop();

    if (encoder->configure("nan", payload, 3U).is_ok() || encoder->configure("1000", payload, 0U).is_ok())
    {
        return 11;
    }

    encoder->~Encoder();
    #endif
    std::puts("Encoder plugin contracts: PASS");
    return 0;
}

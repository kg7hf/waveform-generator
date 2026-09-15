// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

// Test the artifact owner's legacy protocol compatibility without a modem.
#include "examples/encoder-plugin/tone_encoder.hpp"
#include "waveform-source/encoder_registry.hpp"
#include <new>

namespace waveform_generator
{
namespace
{
class TestEncoder final : public waveform_example::ToneEncoder
{
public:
    [[nodiscard]] waveform_source::Status configure(std::string_view profile,
            waveform_source::ByteSource& payload, std::size_t bytes) noexcept override
    {
        if (profile != "600:long" && profile != "600:short" && profile != "4800:zero")
        {
            return {m110::StatusCode::invalid_argument, "BAD_TEST_PROFILE"};
        }

        return ToneEncoder::configure("1000", payload, bytes);
    }
};
waveform_source::Encoder* construct(void* workspace, std::size_t bytes) noexcept
{
    if (workspace == nullptr || bytes < sizeof(TestEncoder) ||
            reinterpret_cast<std::uintptr_t>(workspace) % alignof(TestEncoder) != 0U)
    {
        return nullptr;
    }

    return new (workspace) TestEncoder{};
}
}
std::span<const EncoderDescriptor> encoder_plugin_descriptors() noexcept
{
    static constexpr EncoderDescriptor encoders[]
    {
        {"TEST", "test/1", sizeof(TestEncoder), alignof(TestEncoder), &construct, true}
    };
    return encoders;
}
}

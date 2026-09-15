// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#include "tone_encoder.hpp"
#include "waveform-source/encoder_registry.hpp"
#include <new>

namespace waveform_generator
{
namespace
{
waveform_source::Encoder* construct(void* workspace, std::size_t bytes) noexcept
{
    if (workspace == nullptr || bytes < sizeof(waveform_example::ToneEncoder) ||
            reinterpret_cast<std::uintptr_t>(workspace) % alignof(waveform_example::ToneEncoder) != 0U)
    {
        return nullptr;
    }

    return new (workspace) waveform_example::ToneEncoder{};
}
}
std::span<const EncoderDescriptor> encoder_plugin_descriptors() noexcept
{
    static constexpr EncoderDescriptor encoders[]
    {
        {
            "TONE", "example/1", sizeof(waveform_example::ToneEncoder),
            alignof(waveform_example::ToneEncoder), &construct
        }
    };
    return encoders;
}
}

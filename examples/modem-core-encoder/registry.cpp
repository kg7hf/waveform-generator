// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "m110_encoder.hpp"
#include "waveform-source/encoder_registry.hpp"

#include <array>
#include <memory>

namespace waveform_generator
{
namespace
{
waveform_source::Encoder* construct(void* workspace, std::size_t bytes) noexcept
{
    if (workspace == nullptr)
    {
        return nullptr;
    }
    void* aligned = workspace;
    std::size_t available = bytes;
    if (std::align(alignof(waveform_example::M110Encoder), sizeof(waveform_example::M110Encoder), aligned, available) != workspace)
    {
        return nullptr;
    }
    return std::construct_at(static_cast<waveform_example::M110Encoder*>(workspace));
}
} // namespace

std::span<const EncoderDescriptor> encoder_plugin_descriptors() noexcept
{
    static_assert(sizeof(waveform_example::M110Encoder) <= encoder_workspace_capacity);
    static_assert(alignof(waveform_example::M110Encoder) <= encoder_workspace_alignment);
    static constexpr std::array<EncoderDescriptor, 1U> encoders{{{"M110", "modem-core/1", sizeof(waveform_example::M110Encoder), alignof(waveform_example::M110Encoder), &construct}}};
    return encoders;
}
} // namespace waveform_generator

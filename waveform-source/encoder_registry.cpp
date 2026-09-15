// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#include "waveform-source/encoder_registry.hpp"

namespace waveform_generator
{
namespace
{
bool valid_token(const char* value, std::size_t maximum, bool version) noexcept
{
    if (value == nullptr || value[0] == '\0')
    {
        return false;
    }

    for (std::size_t i = 0U; i <= maximum; ++i)
    {
        const char c = value[i];

        if (c == '\0')
        {
            return true;
        }

        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                (version && ((c >= 'a' && c <= 'z') || c == '-' || c == '.' || c == '/'))))
        {
            return false;
        }
    }

    return false;
}
}

#if !defined(WFG_ENABLE_ENCODER_PLUGINS)
std::span<const EncoderDescriptor> encoder_plugin_descriptors() noexcept
{
    return {};
}
#endif

std::span<const EncoderDescriptor> registered_encoders() noexcept
{
    const auto encoders = encoder_plugin_descriptors();

    if (encoders.size() > maximum_encoders)
    {
        return {};
    }

    bool legacy_seen = false;

    for (std::size_t i = 0U; i < encoders.size(); ++i)
    {
        const auto& encoder = encoders[i];
        const auto alignment = encoder.workspace_alignment;

        if (!valid_token(encoder.id, 15U, false) || !valid_token(encoder.version, 63U, true) ||
                encoder.workspace_bytes == 0U || encoder.workspace_bytes > encoder_workspace_capacity ||
                alignment == 0U || alignment > encoder_workspace_alignment || (alignment & (alignment - 1U)) != 0U ||
                encoder.construct == nullptr || (encoder.legacy_default && legacy_seen))
        {
            return {};
        }

        legacy_seen = legacy_seen || encoder.legacy_default;

        for (std::size_t j = 0U; j < i; ++j)
        {
            if (std::string_view(encoders[j].id) == encoder.id)
            {
                return {};
            }
        }
    }

    return encoders;
}

const EncoderDescriptor* find_encoder(std::string_view id) noexcept
{
    for (const auto& encoder : registered_encoders())
    {
        if (id == encoder.id || (id.empty() && encoder.legacy_default))
        {
            return &encoder;
        }
    }

    return nullptr;
}
}

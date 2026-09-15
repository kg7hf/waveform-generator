// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#include "waveform-source/encoder_registry.hpp"
#include <array>

namespace
{
using waveform_generator::EncoderDescriptor;
waveform_source::Encoder* unused_factory(void*, std::size_t) noexcept
{
    return nullptr;
}
constexpr EncoderDescriptor good{"CUSTOM", "example/1", 64U, 8U, &unused_factory};
std::array<EncoderDescriptor, 9U> entries{};
std::size_t count{};
bool rejected(const EncoderDescriptor& descriptor)
{
    entries[0] = descriptor;
    count = 1U;
    return waveform_generator::registered_encoders().empty();
}
}
namespace waveform_generator
{
std::span<const EncoderDescriptor> encoder_plugin_descriptors() noexcept
{
    return {entries.data(), count};
}
}
int main()
{
    using namespace waveform_generator;

    if (!registered_encoders().empty())
    {
        return 1;
    }

    entries.fill(good);
    count = 1U;

    if (registered_encoders().size() != 1U || find_encoder("CUSTOM") != &entries[0] || find_encoder("") != nullptr)
    {
        return 2;
    }

    entries[0].legacy_default = true;

    if (find_encoder("") != &entries[0])
    {
        return 3;
    }

    count = 2U;

    if (!registered_encoders().empty())
    {
        return 4;    // Duplicate IDs.
    }

    entries[1].id = "SECOND";
    entries[1].legacy_default = true;

    if (!registered_encoders().empty())
    {
        return 5;    // Ambiguous legacy routing.
    }

    count = entries.size();

    if (!registered_encoders().empty())
    {
        return 6;
    }

    for (const char* id :
{"", "lowercase", "BAD-ID", "QUOTE\"", "1234567890123456", static_cast<const char*>(nullptr)
    })
    {
        auto descriptor = good;
        descriptor.id = id;

        if (!rejected(descriptor))
        {
            return 7;
        }
    }

    for (const auto bytes :
{
    std::size_t{0U}, encoder_workspace_capacity + 1U
})
    {
        auto descriptor = good;
        descriptor.workspace_bytes = bytes;

        if (!rejected(descriptor))
        {
            return 8;
        }
    }

    for (const auto alignment :
{
    0U, 3U, 64U
})
    {
        auto descriptor = good;
        descriptor.workspace_alignment = alignment;

        if (!rejected(descriptor))
        {
            return 9;
        }
    }
    auto descriptor = good;
    descriptor.construct = nullptr;

    if (!rejected(descriptor))
    {
        return 10;
    }

    descriptor = good;
    descriptor.version = "unsafe\"version";

    if (!rejected(descriptor))
    {
        return 11;
    }

    return 0;
}

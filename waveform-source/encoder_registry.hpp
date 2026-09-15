// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#pragma once
#include "waveform-source/encoder.hpp"
#include <span>

namespace waveform_generator
{
// Static, caller-owned storage; no heap allocation or dynamic library loading.
inline constexpr std::size_t encoder_workspace_capacity = 128U * 1024U;
inline constexpr std::size_t encoder_workspace_alignment = 32U;
inline constexpr std::size_t maximum_encoders = 8U;

struct EncoderDescriptor
{
    const char* id;      // 1..15 ASCII uppercase letters, digits or underscore (CDC grammar).
    const char* version; // 1..63 ASCII letters, digits, underscore, hyphen, dot or slash.
    std::size_t workspace_bytes;
    std::size_t workspace_alignment;
    waveform_source::Encoder* (*construct)(void*, std::size_t) noexcept;
    // Opt in to old rate:interleave commands. At most one adapter can do this.
    bool legacy_default{false};
};

// Supplied by the optional module. The array and strings must have static lifetime.
// All adapters share this compile-time interface and must be rebuilt with the host.
[[nodiscard]] std::span<const EncoderDescriptor> encoder_plugin_descriptors() noexcept;

// Invalid registrations fail closed as an empty catalog. Never allocate or throw.
[[nodiscard]] std::span<const EncoderDescriptor> registered_encoders() noexcept;
// Empty ID selects only an explicitly registered legacy default; unknown IDs fail.
[[nodiscard]] const EncoderDescriptor* find_encoder(std::string_view id) noexcept;
}

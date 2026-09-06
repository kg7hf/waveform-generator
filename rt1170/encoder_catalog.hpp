#pragma once
#include "waveform-source/encoder.hpp"
#include <cstddef>
#include <string_view>

namespace waveform_generator
{
inline constexpr std::size_t encoder_workspace_capacity = 128U * 1024U;
struct EncoderDescriptor
{
    const char* id;
    const char* version;
    waveform_source::Encoder* (*construct)(void*, std::size_t) noexcept;
};
[[nodiscard]] const EncoderDescriptor* find_encoder(std::string_view id) noexcept;
}

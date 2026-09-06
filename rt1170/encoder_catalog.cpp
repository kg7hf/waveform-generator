#include "encoder_catalog.hpp"
#include "native-m110/source.hpp"

namespace waveform_generator
{
static_assert(native_m110::encoder_workspace_bytes <= encoder_workspace_capacity);
static_assert(native_m110::encoder_workspace_alignment <= 32);
// Only this composition file knows the concrete encoders. New adapters register
// here; the SD utility, WAV writer, player and impairment stages stay unchanged.
const EncoderDescriptor* find_encoder(std::string_view id) noexcept
{
    static constexpr EncoderDescriptor encoders[] = {
        {"M110B", "wfg-native-m110/0.1.0", &native_m110::create_encoder},
    };
    for (const auto& encoder : encoders) if (id == encoder.id) return &encoder;
    return nullptr;
}
}
